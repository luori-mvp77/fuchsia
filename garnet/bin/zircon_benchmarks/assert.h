// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_ZIRCON_BENCHMARKS_ASSERT_H_
#define GARNET_BIN_ZIRCON_BENCHMARKS_ASSERT_H_

#include <zircon/assert.h>
#include <zircon/status.h>

inline void AssertOk(const char* source_file, int source_line, const char* expr,
                     zx_status_t status) {
  if (unlikely(status != ZX_OK)) {
    ZX_PANIC("ASSERT FAILED at (%s:%d): %s returned %s (%d)\n", source_file, source_line, expr,
             zx_status_get_string(status), status);
  }
}

#define ASSERT_OK(x) AssertOk(__FILE__, __LINE__, #x, (x))

#endif  // GARNET_BIN_ZIRCON_BENCHMARKS_ASSERT_H_
