# Development log

This log records implementation checkpoints and their evidence. A successful
build is not treated as proof of account access or hardware compatibility.
Dates use Asia/Shanghai.

## 2026-10-20 · Environment and reference baseline

- Started from an empty `netease-switch` working directory.
- Inspected Switify at `e4b86610598dcf511b3682be0e047855e7445ac6`
  and netease-cloud-music-gtk at
  `225d14fb42bc0e0fdce1cbcb640577c307efc641`.
- Selected Switify's vendored Borealis GUI implementation. Preserve its
  Apache-2.0 notices and its dependency licenses.
- Traced GTK's API to `netease-cloud-music-api` tag `2.2.0`.
  The API catalog and C++ port are developed together.
- Created a Docker devkitPro environment pinned by image digest.
  `scripts/docker-build.sh --smoke` produced an AArch64 ELF, NACP and NRO.
- Installed Eden's official `v0.2.1` Linux AppImage in an isolated local
  directory. Its virtual desktop on display `:119` successfully loaded and
  rendered the toolchain smoke NRO without keys or firmware.
- Created a separate virtual desktop on display `:120` for the desktop
  Borealis build. Existing desktop sessions are left in place.
- Toolchain packages and test logs are retained in `build/validation/`;
  reference documentation, source URLs and checksums are in `docs/reference/`.
- Found that official Switch portlibs have mbedTLS, but no OpenSSL package.
  The API cryptography implementation therefore uses mbedTLS.

Further checkpoints are appended after their corresponding build or test.
