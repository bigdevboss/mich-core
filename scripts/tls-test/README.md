# Test-only key material

The private key in this directory is **published on purpose** and protects
nothing. It exists so `make test64-tls` can start a local `openssl s_server`
that the guest will actually trust.

Anyone can use it. That is fine, because:

- it is only ever served on `127.0.0.1:4433` by the test runner,
- the certificate names `mich.test`, a name that resolves nowhere,
- the root that signed it is a throwaway generated for this test and lives
  nowhere outside `src/user64/tlsprobe/test_anchor.h`.

It cannot be generated on the fly: the guest carries that root as a DER
constant compiled into its image, so the server certificate has to be signed
by that exact root. Regenerating the pair means rebuilding the image, which a
test has no business doing.

**Never point anything real at these files.**
