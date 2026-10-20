# Mozilla CA trust bundle

`cacert.pem` was downloaded unchanged from
<https://curl.se/ca/cacert.pem> on **2026-10-20**.

- Mozilla certificate data date: **2026-09-25 03:12:01 GMT**
- SHA-256: `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`
- The hash was compared with <https://curl.se/ca/cacert.pem.sha256>.
- Source and extraction details: <https://curl.se/docs/caextract.html>
- License: Mozilla Public License 2.0; see `LICENSE-MPL-2.0.txt`.

The NRO includes the file at `romfs:/certs/cacert.pem`. Its HTTPS client must
enable both peer and hostname verification. A system module cannot access
another process's RomFS and needs its own installed copy of this bundle.

Update this file intentionally, preserving the Mozilla data date, verified
hash and license notice. Do not work around certificate failures by disabling
TLS verification.
