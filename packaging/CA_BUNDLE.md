# TLS trust for GDAL's curl-based drivers

Online imagery templates (WMS/TMS) fetch tiles over HTTPS through
GDAL's curl-based drivers. Whether certificate verification works on an
end-user machine depends on which TLS backend the packaged libcurl
uses.

## Preferred: a native TLS backend (no bundle)

Where libcurl can use the operating system's certificate store, ship
that and do **not** package a CA bundle:

- **Windows:** build against a Schannel-backed curl
  (`mingw-w64-x86_64-curl-winssl` in MSYS2). Verification uses the
  Windows certificate store, which the OS keeps up to date.
- **macOS:** the system libcurl uses Secure Transport and the system
  keychain.

A native backend needs no refresh cadence and keeps the installer
smaller.

## Fallback: `Mapper_PACKAGE_CA_BUNDLE`

Some builds link a libcurl with no usable default trust store (for
example an OpenSSL-backed MinGW curl, whose baked-in CA path
`/mingw64/etc/ssl/certs/ca-bundle.crt` does not exist on end-user
machines — every HTTPS tile fetch then fails and online templates
render all white).

For such builds, pass a PEM bundle at configure time:

    -DMapper_PACKAGE_CA_BUNDLE=/path/to/ca-bundle.crt

It is installed as `gdal/curl-ca-bundle.crt` next to the other GDAL
data files. At startup, `gdal_manager.cpp` points GDAL's
`CURL_CA_BUNDLE` config option at it, unless the environment already
sets `CURL_CA_BUNDLE` or `SSL_CERT_FILE`.

### Refresh cadence

A packaged bundle is a frozen snapshot of the trust store at build
time. It must be refreshed:

- **At every release** — rebuild against current CA packages rather
  than reusing cached build trees. In CI this means making sure the
  CA-certificates package version participates in any cache key.
- **Immediately** when a root CA used by common tile providers is
  distrusted or expires (the September 2021 Let's Encrypt
  `DST Root CA X3` expiry is the canonical example of working builds
  going stale in the field).

A bundle older than ~12 months should be treated as expired.

### Pitfalls

- `mingw-w64-x86_64-ca-certificates` ships `ca-bundle.crt` as a
  zero-byte file; a post-install scriptlet fills it, and pacman
  reports success even if that scriptlet fails. Verify content
  (certificate count), not existence.
- Never ship a bundle alongside a native-backend curl: the packaged
  file would shadow the always-current system store with the frozen
  snapshot.
