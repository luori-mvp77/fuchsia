// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package ssh

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/pem"
	"fmt"
	"time"

	"fuchsia.googlesource.com/tools/retry"

	"golang.org/x/crypto/ssh"
)

const (
	// Default SSH server port.
	Port = 22

	// Default RSA key size.
	KeySize = 2048
)

// GenerateKeyPair generates a pair of private/public keys.
func GenerateKeyPairt(bitSize int) ([]byte, []byte, error) {
	key, err := rsa.GenerateKey(rand.Reader, bitSize)
	if err != nil {
		return nil, nil, err
	}

	pubkey, err := ssh.NewPublicKey(&key.PublicKey)
	if err != nil {
		return nil, nil, err
	}
	pembuf := pubkey.Marshal()

	var privateKey = &pem.Block{
		Type:  "RSA PRIVATE KEY",
		Bytes: x509.MarshalPKCS1PrivateKey(key),
	}
	buf := pem.EncodeToMemory(privateKey)

	return pembuf, buf, nil
}

// Connect establishes a new SSH connection to a server with the given
// address and port, using the provided user name and private key.
func Connect(ctx context.Context, address string, port int, user string, privateKey []byte) (*ssh.Client, error) {
	signer, err := ssh.ParsePrivateKey(privateKey)
	if err != nil {
		return nil, fmt.Errorf("cannot parse the private key: %v", err)
	}

	config := &ssh.ClientConfig{
		User: user,
		Auth: []ssh.AuthMethod{
			ssh.PublicKeys(signer),
		},
		Timeout:         time.Minute, // TODO: allow passing the timeout
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
	}

	var client *ssh.Client
	addr := fmt.Sprintf("%s:%d", address, port)
	// TODO: figure out optimal backoff time and number of retries
	err = retry.Retry(ctx, retry.WithMaxRetries(retry.NewConstantBackoff(time.Second), 10), func() error {
		client, err = ssh.Dial("tcp", addr, config)
		return err
	})
	if err != nil {
		return nil, fmt.Errorf("cannot connect to \"%s\": %v", addr, err)
	}

	return client, nil
}
