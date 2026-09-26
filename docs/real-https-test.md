# Optional real-internet HTTPS test

`make test-https-real` drives the in-guest `tlsprobe` through a complete TLS 1.3
handshake against a real public server (`www.google.com` by default) and prints
the HTTP status line it reads off the wire. It is deliberately kept out of the
normal test matrix: a test that needs the internet should be skipped, not
failed, when there is no connectivity.

## What it exercises that the local test does not

The local `make test64-tls` handshakes against a throwaway `openssl s_server`
on `127.0.0.1`, using a small test root compiled into `tlsprobe` only. That
proves the record layer and handshake state machine work, but it never touches:

- the built-in production trust store (ISRG Root X1, DigiCert Global Root G2,
  GTS Root R1),
- an RSA-4096 trust anchor (GTS Root R1), which drives the modular
  exponentiation far deeper than the test root does,
- a genuine server certificate chain (`www.google.com` leaf, ECDSA P-256,
  signed by an RSA-2048 intermediate, signed by the RSA-4096 root),
- real-world record fragmentation from a peer nobody here controls.

## How it is wired

- `src/user64/tlsprobe/main.c` builds in two flavours. With `-DMICH_TLS_REAL`
  it presents `www.google.com` in SNI, validates against
  `x509_builtin_trust_store()`, and echoes the real status line. Without it,
  the original local behaviour is unchanged.
- The Makefile builds the real flavour into a separate `disk-tls-real.img` so
  the local test image is untouched.
- `scripts/qemu-smoke64.sh` gains a `tls-real` profile. It resolves an IPv4 for
  the target on the host and bridges the guest's fixed `10.0.2.4:443` straight
  to it through slirp, so the guest code path is identical to the local test.
  If the name does not resolve or the host is unreachable, the run prints a
  SKIP and exits success.

Override the target with `MICH_TLS_REAL_HOST`, for example:

```
MICH_TLS_REAL_HOST=example.com make test-https-real
```

## A kernel bug this test found

A spawned user image was given a single 4 KiB stack page. That is enough for the
ECDSA test root but overflows during RSA-4096 anchor verification, so the real
handshake faulted right after `ClientHello`. `spawn64_image` now maps
`SPAWN_STACK_PAGES` (8) pages, keeping the unmapped guard page above the top so
an overflow still faults instead of corrupting the heap.
