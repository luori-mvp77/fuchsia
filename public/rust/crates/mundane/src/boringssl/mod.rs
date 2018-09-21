// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The BoringSSL API.
//!
//! This module provides a safe access to the BoringSSL API.
//!
//! It accomplishes this using the following structure:
//! - The internal `raw` module provides nearly-raw access to the BoringSSL API.
//!   For each function in the BoringSSL API, it exposes an equivalent Rust
//!   function which performs error checking. Functions which return pointers
//!   return `Result<NonNull<T>, BoringError>`, functions which return status
//!   codes return `Result<(), BoringError>`, etc. This API makes it less likely
//!   to accidentally forget to check for null pointers or error status codes.
//! - The internal `wrapper` module provides types which wrap C objects and
//!   handle many of the details of their lifecycles. These include
//!   `CStackWrapper`, which handles initializing and destructing
//!   stack-allocated C objects; `CHeapWrapper`, which is analogous to Rust's
//!   `Box` or `Rc`, and handles allocation, reference counting, and freeing;
//!   and `CRef`, which is analogous to a Rust reference.
//! - This module builds on top of the `raw` and `wrapper` modules to provide a
//!   safe API. This allows us to `#![forbid(unsafe_code)]` in the rest of the
//!   crate, which in turn means that this is the only module whose memory
//!   safety needs to be manually verified.
//!
//! # Usage
//!
//! Each type, `T`, from the BoringSSL API is exposed as either a
//! `CStackWrapper<T>`, a `CHeapWrapper<T>`, or a `CRef<T>`. Each function from
//! the BoringSSL API which operates on a particular type is exposed as a method
//! on the wrapped version of that type. For example, the BoringSSL `CBS_len`
//! function operates on a `CBS`; we provide the `cbs_len` method on the
//! `CStackWrapper<CBS>` type. While BoringSSL functions that operate on a
//! particular type take the form `TYPE_method`, the Rust equivalents are all
//! lower-case - `type_method`.
//!
//! Some functions which do not make sense as methods are exposed as bare
//! functions. For example, the BoringSSL `ECDSA_sign` function is exposed as a
//! bare function as `ecdsa_sign`.
//!
//! Types which can be constructed without arguments implement `Default`. Types
//! which require arguments to be constructed provide associated functions which
//! take those arguments and return a new instance of that type. For example,
//! the `CHeapWrapper<EC_KEY>::ec_key_parse_private_key` function parses a
//! private key from an input stream and returns a new `CHeapWrapper<EC_KEY>`.
//!
//! # API Guidelines
//!
//! This module is meant to be as close as possible to a direct set of FFI
//! bindings while still providing a safe API. While memory safety is handled
//! internally, and certain error conditions which could affect memory safety
//! are checked internally (and cause the process to abort if they fail), most
//! errors are returned from the API, as they are considered business logic,
//! which is outside the scope of this module.

// NOTES on safety requirements of the BoringSSL API:
// - Though it may not be explicitly documented, calling methods on uinitialized
//   values is UB. Remember, this is C! Always initialize (usually via XXX_init
//   or a similarly-named function) before calling any methods or functions.
// - Any BoringSSL documentation that says "x property must hold" means that, if
//   that property doesn't hold, it may cause UB - you are not guaranteed that
//   it will be detected and an error will be returned.

#[macro_use]
mod abort;
#[macro_use]
mod wrapper;
mod raw;

// C types
pub use boringssl_sys::{CBB, CBS, EC_GROUP, EC_KEY, EVP_MD, EVP_PKEY, HMAC_CTX, SHA256_CTX,
                        SHA512_CTX, SHA_CTX};
// C constants
pub use boringssl_sys::{NID_X9_62_prime256v1, NID_secp384r1, NID_secp521r1,
                        ED25519_PRIVATE_KEY_LEN, ED25519_PUBLIC_KEY_LEN, ED25519_SIGNATURE_LEN,
                        SHA256_DIGEST_LENGTH, SHA384_DIGEST_LENGTH, SHA512_DIGEST_LENGTH,
                        SHA_DIGEST_LENGTH};
// wrapper types
pub use boringssl::wrapper::{CHeapWrapper, CRef, CStackWrapper};

use std::ffi::CStr;
use std::fmt::{self, Debug, Display, Formatter};
use std::num::NonZeroUsize;
use std::os::raw::{c_char, c_int, c_uint, c_void};
use std::{mem, ptr, slice};

use boringssl::abort::UnwrapAbort;
use boringssl::raw::{CBB_data, CBB_init, CBB_len, CBS_init, CBS_len, CRYPTO_memcmp, ECDSA_sign,
                     ECDSA_size, ECDSA_verify, EC_GROUP_get_curve_name,
                     EC_GROUP_new_by_curve_name, EC_KEY_generate_key, EC_KEY_get0_group,
                     EC_KEY_marshal_private_key, EC_KEY_parse_private_key, EC_KEY_set_group,
                     EC_curve_nid2nist, ED25519_keypair, ED25519_keypair_from_seed, ED25519_sign,
                     ED25519_verify, ERR_print_errors_cb, EVP_PBE_scrypt, EVP_PKEY_assign_EC_KEY,
                     EVP_PKEY_get1_EC_KEY, EVP_marshal_public_key, EVP_parse_public_key,
                     HMAC_CTX_init, HMAC_Final, HMAC_Init_ex, HMAC_Update, HMAC_size, RAND_bytes,
                     SHA384_Init, PKCS5_PBKDF2_HMAC};

impl CStackWrapper<CBB> {
    /// Creates a new `CBB` and initializes it with `CBB_init`.
    ///
    /// `cbb_new` can only fail due to OOM.
    #[must_use]
    pub fn cbb_new(initial_capacity: usize) -> Result<CStackWrapper<CBB>, BoringError> {
        unsafe {
            let mut cbb = mem::uninitialized();
            CBB_init(&mut cbb, initial_capacity)?;
            Ok(CStackWrapper::new(cbb))
        }
    }

    /// Invokes a callback on the contents of a `CBB`.
    ///
    /// `cbb_with_data` accepts a callback, and invokes that callback, passing a
    /// slice of the current contents of this `CBB`.
    #[must_use]
    pub fn cbb_with_data<O, F: Fn(&[u8]) -> O>(&self, with_data: F) -> O {
        unsafe {
            // NOTE: The return value of CBB_data is only valid until the next
            // operation on the CBB. This method is safe because the slice
            // reference cannot outlive this function body, and thus cannot live
            // beyond another method call that could invalidate the buffer.
            let len = CBB_len(self.as_const());
            if len == 0 {
                // If len is 0, then CBB_data could technically return a null
                // pointer. Constructing a slice from a null pointer is likely
                // invalid, so we do this instead.
                with_data(&[])
            } else {
                // Since the length is non-zero, CBB_data should not return a
                // null pointer.
                let ptr = CBB_data(self.as_const()).unwrap_abort();
                // TODO(joshlf): Can with_data use this to smuggle out the
                // reference, outliving the lifetime of self?
                with_data(slice::from_raw_parts(ptr.as_ptr(), len))
            }
        }
    }
}

impl CStackWrapper<CBS> {
    /// The `CBS_len` function.
    #[must_use]
    pub fn cbs_len(&self) -> usize {
        unsafe { CBS_len(self.as_const()) }
    }

    /// Invokes a callback on a temporary `CBS`.
    ///
    /// `cbs_with_temp_buffer` constructs a `CBS` from the provided byte slice,
    /// and invokes a callback on the `CBS`. The `CBS` is destructed before
    /// `cbs_with_temp_buffer` returns.
    // TODO(joshlf): Holdover until we figure out how to put lifetimes in CStackWrappers.
    #[must_use]
    pub fn cbs_with_temp_buffer<O, F: Fn(&mut CStackWrapper<CBS>) -> O>(
        bytes: &[u8], with_cbs: F,
    ) -> O {
        unsafe {
            let mut cbs = mem::uninitialized();
            CBS_init(&mut cbs, bytes.as_ptr(), bytes.len());
            let mut cbs = CStackWrapper::new(cbs);
            with_cbs(&mut cbs)
        }
    }
}

impl CRef<'static, EC_GROUP> {
    /// The `EC_GROUP_new_by_curve_name` function.
    #[must_use]
    pub fn ec_group_new_by_curve_name(nid: c_int) -> Result<CRef<'static, EC_GROUP>, BoringError> {
        unsafe { Ok(CRef::new(EC_GROUP_new_by_curve_name(nid)?)) }
    }
}

impl<'a> CRef<'a, EC_GROUP> {
    /// The `EC_GROUP_get_curve_name` function.
    #[must_use]
    pub fn ec_group_get_curve_name(&self) -> c_int {
        unsafe { EC_GROUP_get_curve_name(self.as_const()) }
    }
}

/// The `EC_curve_nid2nist` function.
#[must_use]
pub fn ec_curve_nid2nist(nid: c_int) -> Result<&'static CStr, BoringError> {
    unsafe { Ok(CStr::from_ptr(EC_curve_nid2nist(nid)?.as_ptr())) }
}

impl CHeapWrapper<EC_KEY> {
    /// The `EC_KEY_generate_key` function.
    #[must_use]
    pub fn ec_key_generate_key(&mut self) -> Result<(), BoringError> {
        unsafe { EC_KEY_generate_key(self.as_mut()) }
    }

    /// The `EC_KEY_parse_private_key` function.
    ///
    /// If `group` is `None`, then the group pointer argument to
    /// `EC_KEY_parse_private_key` will be NULL.
    #[must_use]
    pub fn ec_key_parse_private_key(
        cbs: &mut CStackWrapper<CBS>, group: Option<CRef<'static, EC_GROUP>>,
    ) -> Result<CHeapWrapper<EC_KEY>, BoringError> {
        unsafe {
            Ok(CHeapWrapper::new_from(EC_KEY_parse_private_key(
                cbs.as_mut(),
                group.map(|g| g.as_const()).unwrap_or(ptr::null()),
            )?))
        }
    }

    /// The `EC_KEY_get0_group` function.
    #[must_use]
    #[allow(clippy::needless_lifetimes)] // to be more explicit
    pub fn ec_key_get0_group<'a>(&'a self) -> Result<CRef<'a, EC_GROUP>, BoringError> {
        // get0 doesn't increment the refcount; the lifetimes ensure that the
        // returned CRef can't outlive self
        unsafe { Ok(CRef::new(EC_KEY_get0_group(self.as_const())?)) }
    }

    /// The `EC_KEY_set_group` function.
    #[must_use]
    pub fn ec_key_set_group(&mut self, group: &CRef<'static, EC_GROUP>) -> Result<(), BoringError> {
        unsafe { EC_KEY_set_group(self.as_mut(), group.as_const()) }
    }

    /// The `EC_KEY_marshal_private_key` function.
    #[must_use]
    pub fn ec_key_marshal_private_key(
        &self, cbb: &mut CStackWrapper<CBB>,
    ) -> Result<(), BoringError> {
        unsafe { EC_KEY_marshal_private_key(cbb.as_mut(), self.as_const(), 0) }
    }
}

/// The `ECDSA_sign` function.
///
/// `ecdsa_sign` returns the number of bytes written to `sig`.
///
/// # Panics
///
/// `ecdsa_sign` panics if `sig` is shorter than the minimum required signature
/// size given by `ecdsa_size`, or if `key` doesn't have a group set.
#[must_use]
pub fn ecdsa_sign(
    digest: &[u8], sig: &mut [u8], key: &CHeapWrapper<EC_KEY>,
) -> Result<usize, BoringError> {
    unsafe {
        // If we call ECDSA_sign with sig.len() < min_size, it will invoke UB.
        // ECDSA_size fails if the key doesn't have a group set.
        let min_size = ecdsa_size(key).unwrap_abort();
        assert_abort!(sig.len() >= min_size.get());

        let mut sig_len: c_uint = 0;
        ECDSA_sign(
            0,
            digest.as_ptr(),
            digest.len(),
            sig.as_mut_ptr(),
            &mut sig_len,
            key.as_const(),
        )?;
        // ECDSA_sign guarantees that it only needs ECDSA_size bytes for the
        // signature.
        assert_abort!(sig_len as usize <= min_size.get());
        Ok(sig_len as usize)
    }
}

/// The `ECDSA_verify` function.
#[must_use]
pub fn ecdsa_verify(digest: &[u8], sig: &[u8], key: &CHeapWrapper<EC_KEY>) -> bool {
    unsafe {
        ECDSA_verify(
            0,
            digest.as_ptr(),
            digest.len(),
            sig.as_ptr(),
            sig.len(),
            key.as_const(),
        )
    }
}

/// The `ECDSA_size` function.
#[must_use]
pub fn ecdsa_size(key: &CHeapWrapper<EC_KEY>) -> Result<NonZeroUsize, BoringError> {
    unsafe { ECDSA_size(key.as_const()) }
}

/// The `ED25519_keypair` function.
#[must_use]
pub fn ed25519_keypair() -> [u8; ED25519_PRIVATE_KEY_LEN as usize] {
    let mut public_unused = [0u8; ED25519_PUBLIC_KEY_LEN as usize];
    let mut private = [0u8; ED25519_PRIVATE_KEY_LEN as usize];
    unsafe {
        ED25519_keypair(
            (&mut public_unused[..]).as_mut_ptr(),
            (&mut private[..]).as_mut_ptr(),
        )
    };
    private
}

/// The `ED25519_sign` function.
#[must_use]
pub fn ed25519_sign(message: &[u8], private_key: &[u8; 64]) -> Result<[u8; 64], BoringError> {
    let mut sig = [0u8; 64];
    unsafe { ED25519_sign(&mut sig, message.as_ptr(), message.len(), private_key)? };
    Ok(sig)
}

/// The `ED25519_keypair_from_seed` function.
#[must_use]
pub fn ed25519_keypair_from_seed(seed: &[u8; 32]) -> ([u8; 32], [u8; 64]) {
    let mut public = [0u8; 32];
    let mut private = [0u8; 64];
    unsafe {
        ED25519_keypair_from_seed(
            (&mut public[..]).as_mut_ptr(),
            (&mut private[..]).as_mut_ptr(),
            (&seed[..]).as_ptr(),
        )
    };
    (public, private)
}

/// The `ED25519_verify` function.
#[must_use]
pub fn ed25519_verify(message: &[u8], signature: &[u8; 64], public_key: &[u8; 32]) -> bool {
    unsafe { ED25519_verify(message.as_ptr(), message.len(), signature, public_key) }
}

impl CHeapWrapper<EVP_PKEY> {
    /// The `EVP_parse_public_key` function.
    #[must_use]
    pub fn evp_parse_public_key(
        cbs: &mut CStackWrapper<CBS>,
    ) -> Result<CHeapWrapper<EVP_PKEY>, BoringError> {
        unsafe { Ok(CHeapWrapper::new_from(EVP_parse_public_key(cbs.as_mut())?)) }
    }

    /// The `EVP_marshal_public_key` function.
    #[must_use]
    pub fn evp_marshal_public_key(&self, cbb: &mut CStackWrapper<CBB>) -> Result<(), BoringError> {
        unsafe { EVP_marshal_public_key(cbb.as_mut(), self.as_const()) }
    }

    /// The `EVP_PKEY_assign_EC_KEY` function.
    #[must_use]
    pub fn evp_pkey_assign_ec_key(&mut self, ec_key: CHeapWrapper<EC_KEY>) {
        unsafe {
            // NOTE: It's very important that we use 'into_mut' here so that
            // ec_key's refcount is not decremented. That's because
            // EVP_PKEY_assign_EC_KEY doesn't increment the refcount of its
            // argument.
            let key = ec_key.into_mut();
            // EVP_PKEY_assign_EC_KEY only fails if key is NULL.
            EVP_PKEY_assign_EC_KEY(self.as_mut(), key).unwrap_abort()
        }
    }

    /// The `EVP_PKEY_get1_EC_KEY` function.
    #[must_use]
    pub fn evp_pkey_get1_ec_key(&mut self) -> Result<CHeapWrapper<EC_KEY>, BoringError> {
        // NOTE: It's important that we use get1 here, as it increments the
        // refcount of the EC_KEY before returning a pointer to it.
        unsafe { Ok(CHeapWrapper::new_from(EVP_PKEY_get1_EC_KEY(self.as_mut())?)) }
    }
}

/// The `EVP_PBE_scrypt` function.
#[allow(non_snake_case)]
#[must_use]
pub fn evp_pbe_scrypt(
    password: &[u8], salt: &[u8], N: u64, r: u64, p: u64, max_mem: usize, out_key: &mut [u8],
) -> Result<(), BoringError> {
    unsafe {
        EVP_PBE_scrypt(
            password.as_ptr() as *const c_char,
            password.len(),
            salt.as_ptr(),
            salt.len(),
            N,
            r,
            p,
            max_mem,
            out_key.as_mut_ptr(),
            out_key.len(),
        )
    }
}

/// The `PKCS5_PBKDF2_HMAC` function.
#[must_use]
pub fn pkcs5_pbkdf2_hmac(
    password: &[u8], salt: &[u8], iterations: c_uint, digest: &CRef<'static, EVP_MD>,
    out_key: &mut [u8],
) -> Result<(), BoringError> {
    unsafe {
        PKCS5_PBKDF2_HMAC(
            password.as_ptr() as *const c_char,
            password.len(),
            salt.as_ptr(),
            salt.len(),
            iterations,
            digest.as_const(),
            out_key.len(),
            out_key.as_mut_ptr(),
        )
    }
}

impl CStackWrapper<SHA512_CTX> {
    /// Initializes a new `CStackWrapper<SHA512_CTX>` as a SHA-384 hash.
    ///
    /// The BoringSSL `SHA512_CTX` is used for both the SHA-512 and SHA-384 hash
    /// functions. The implementation of `Default` for
    /// `CStackWrapper<SHA512_CTX>` produces a context initialized for a SHA-512
    /// hash. In order to produce a context for a SHA-384 hash, use this
    /// constructor instead.
    #[must_use]
    pub fn sha384_new() -> CStackWrapper<SHA512_CTX> {
        unsafe {
            let mut ctx = mem::uninitialized();
            SHA384_Init(&mut ctx);
            CStackWrapper::new(ctx)
        }
    }
}

macro_rules! impl_evp_digest {
    (#[$doc:meta] $name:ident, $raw_name:ident) => {
        #[$doc]
        #[must_use]
        pub fn $name() -> CRef<'static, EVP_MD> {
            unsafe { CRef::new(::boringssl::raw::$raw_name()) }
        }
    };
}

impl CRef<'static, EVP_MD> {
    impl_evp_digest!(
        /// The `EVP_sha1` function.
        evp_sha1,
        EVP_sha1
    );
    impl_evp_digest!(
        /// The `EVP_sha256` function.
        evp_sha256,
        EVP_sha256
    );
    impl_evp_digest!(
        /// The `EVP_sha384` function.
        evp_sha384,
        EVP_sha384
    );
    impl_evp_digest!(
        /// The `EVP_sha512` function.
        evp_sha512,
        EVP_sha512
    );
}

impl CStackWrapper<HMAC_CTX> {
    /// Initializes a new `HMAC_CTX`.
    ///
    /// `hmac_ctx_new` initializes a new `HMAC_CTX` using `HMAC_CTX_init` and
    /// then further initializes it with `HMAC_CTX_Init_ex`. It can only fail
    /// due to OOM.
    #[must_use]
    pub fn hmac_ctx_new(
        key: &[u8], md: &CRef<'static, EVP_MD>,
    ) -> Result<CStackWrapper<HMAC_CTX>, BoringError> {
        unsafe {
            let mut ctx = mem::uninitialized();
            HMAC_CTX_init(&mut ctx);
            HMAC_Init_ex(
                &mut ctx,
                key.as_ptr() as *const c_void,
                key.len(),
                md.as_const(),
            )?;
            Ok(CStackWrapper::new(ctx))
        }
    }

    /// The `HMAC_Update` function.
    #[must_use]
    pub fn hmac_update(&mut self, data: &[u8]) {
        unsafe { HMAC_Update(self.as_mut(), data.as_ptr(), data.len()) }
    }

    // NOTE(joshlf): We require exactly the right length (as opposed to just
    // long enough) so that we don't have to have hmac_final return a length.

    /// The `HMAC_Final` function.
    ///
    /// # Panics
    ///
    /// `hmac_final` panics if `out` is not exactly the right length (as defined
    /// by `HMAC_size`).
    #[must_use]
    pub fn hmac_final(&mut self, out: &mut [u8]) {
        unsafe {
            let size = HMAC_size(self.as_const());
            assert_abort_eq!(out.len(), size);
            let mut size = 0;
            // HMAC_Final is documented to fail on allocation failure, but an
            // internal comment states that it's infallible. In either case, we
            // want to panic. Normally, for allocation failure, we'd put the
            // unwrap higher in the stack, but since this is supposed to be
            // infallible anyway, we put it here.
            //
            // TODO(joshlf): Remove this comment once HMAC_Final is documented
            // as being infallible.
            HMAC_Final(self.as_mut(), out.as_mut_ptr(), &mut size).unwrap_abort();
            // Guaranteed to be the value returned by HMAC_size.
            assert_abort_eq!(out.len(), size as usize);
        }
    }
}

/// Implements `CStackWrapper` for a hash context type.
///
/// The caller provides doc comments, a public method name, and a private
/// function name (from the `raw` module) for an update function and a final
/// function (e.g., `SHA256_Update` and `SHA256_Final`). Note that, as multiple
/// impl blocks are allowed for a particular type, the same context type may be
/// used multiple times. This is useful because both SHA-384 and SHA-512 use the
/// `SHA512_CTX` context type.
macro_rules! impl_hash {
    ($ctx:ident, $digest_len:ident, #[$update_doc:meta] $update:ident, $update_raw:ident, #[$final_doc:meta] $final:ident, $final_raw:ident) => {
        impl CStackWrapper<$ctx> {
            #[$update_doc]
            pub fn $update(&mut self, data: &[u8]) {
                unsafe {
                    ::boringssl::raw::$update_raw(
                        self.as_mut(),
                        data.as_ptr() as *const c_void,
                        data.len(),
                    )
                }
            }

            #[$final_doc]
            #[must_use]
            pub fn $final(
                &mut self,
            ) -> [u8; ::boringssl_sys::$digest_len as usize] {
                unsafe {
                    let mut md: [u8; ::boringssl_sys::$digest_len as usize] = mem::uninitialized();
                    // SHA1_Final promises to return 1. SHA256_Final,
                    // SHA384_Final, and SHA512_Final all document that they
                    // only fail due to programmer error. The only input to the
                    // function which could cause this is the context. I suspect
                    // that the error condition is that XXX_Final is called
                    // twice without resetting, but I'm not sure. Until we
                    // figure it out, let's err on the side of caution and abort
                    // here.
                    //
                    // TODO(joshlf): Figure out how XXX_Final can fail.
                    ::boringssl::raw::$final_raw((&mut md[..]).as_mut_ptr(), self.as_mut()).unwrap_abort();
                    md
                }
            }
        }
    };
    (@doc_string $s:expr) => (#[doc="The `"] #[doc=$s] #[doc="` function."]);
}

impl_hash!(
    SHA_CTX,
    SHA_DIGEST_LENGTH,
    /// The `SHA1_Update` function.
    sha1_update,
    SHA1_Update,
    /// The `SHA1_Final` function.
    sha1_final,
    SHA1_Final
);
impl_hash!(
    SHA256_CTX,
    SHA256_DIGEST_LENGTH,
    /// The `SHA256_Update` function.
    sha256_update,
    SHA256_Update,
    /// The `SHA256_Final` function.
    sha256_final,
    SHA256_Final
);
impl_hash!(
    SHA512_CTX,
    SHA384_DIGEST_LENGTH,
    /// The `SHA384_Update` function.
    sha384_update,
    SHA384_Update,
    /// The `SHA384_Final` function.
    sha384_final,
    SHA384_Final
);
impl_hash!(
    SHA512_CTX,
    SHA512_DIGEST_LENGTH,
    /// The `SHA512_Update` function.
    sha512_update,
    SHA512_Update,
    /// The `SHA512_Final` function.
    sha512_final,
    SHA512_Final
);

/// The `CRYPTO_memcmp` function.
///
/// `crypto_memcmp` first verifies that `a.len() == b.len()` before calling
/// `CRYPTO_memcmp`.
#[must_use]
pub fn crypto_memcmp(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    unsafe {
        CRYPTO_memcmp(
            a.as_ptr() as *const c_void,
            b.as_ptr() as *const c_void,
            a.len(),
        ) == 0
    }
}

/// The `RAND_bytes` function.
pub fn rand_bytes(buf: &mut [u8]) {
    unsafe { RAND_bytes(buf.as_mut_ptr(), buf.len()) }
}

/// An error generated by BoringSSL.
///
/// The `Debug` impl prints a stack trace. Each element of the trace corresponds
/// to a function within BoringSSL which voluntarily pushed itself onto the
/// stack. In this sense, it is not the same as a normal stack trace. Each
/// element of the trace is of the form `[thread id]:error:[error code]:[library
/// name]:OPENSSL_internal:[reason string]:[file]:[line number]:[optional string
/// data]`.
///
/// The `Display` impl prints the first element of the stack trace.
///
/// Some BoringSSL functions do not record any error in the error stack. Errors
/// generated from such functions are printed as `error calling <function name>`
/// for both `Debug` and `Display` impls.
pub struct BoringError {
    stack_trace: Vec<String>,
}

impl BoringError {
    /// Consumes the error stack.
    ///
    /// `f` is the name of the function that failed. If the error stack is empty
    /// (some BoringSSL functions do not push errors onto the stack when
    /// returning errors), the returned `BoringError` will simply note that the
    /// named function failed; both the `Debug` and `Display` implementations
    /// will return `error calling f`, where `f` is the value of the `f`
    /// argument.
    #[must_use]
    fn consume_stack(f: &str) -> BoringError {
        let stack_trace = {
            let trace = get_error_stack_trace();
            if trace.is_empty() {
                vec![format!("error calling {}", f)]
            } else {
                trace
            }
        };
        BoringError { stack_trace }
    }

    /// The number of frames in the stack trace.
    ///
    /// Guaranteed to be at least 1.
    #[must_use]
    pub fn stack_depth(&self) -> usize {
        self.stack_trace.len()
    }
}

fn get_error_stack_trace() -> Vec<String> {
    // Credit to agl@google.com for this implementation.

    unsafe extern "C" fn error_callback(s: *const c_char, s_len: usize, ctx: *mut c_void) -> c_int {
        let stack_trace = ctx as *mut Vec<String>;
        let s = ::std::slice::from_raw_parts(s as *const u8, s_len - 1);
        (*stack_trace).push(String::from_utf8_lossy(s).to_string());
        1
    }

    let mut stack_trace = Vec::new();
    unsafe {
        ERR_print_errors_cb(
            Some(error_callback),
            &mut stack_trace as *mut _ as *mut c_void,
        )
    };
    stack_trace
}

impl Display for BoringError {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        write!(f, "{}", self.stack_trace[0])
    }
}

impl Debug for BoringError {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        for elem in &self.stack_trace {
            writeln!(f, "{}", elem)?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use util::should_fail;

    #[test]
    fn test_boring_error() {
        CStackWrapper::cbs_with_temp_buffer(&[], |cbs| {
            should_fail(
                CHeapWrapper::evp_parse_public_key(cbs),
                "boringssl::EVP_parse_public_key",
                "public key routines:OPENSSL_internal:DECODE_ERROR",
            );
        });
    }
}
