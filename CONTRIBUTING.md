# Contributing

Build all three Switch targets with `./scripts/docker-build.sh`. Build the
desktop target and run `ctest --test-dir build/desktop --output-on-failure`
when changing API, parsing or application behavior. Audio engine changes also
need the local and throttled HTTPS playback tests documented in
`docs/background/ARCHITECTURE.md`.

Keep networking and decoding off the GUI thread. Update `docs/API_CATALOG.md`
and its generator when changing endpoint behavior. Preserve the versioned C
IPC ABI, or bump its version and update both clients with a compatibility
error message. Do not mix GPL-3-only code into the GPL-2-only overlay.

For GUI changes, run the desktop build and the actual NRO in the virtual
desktop. Inspect Chinese text, focus, dialogs, loading and empty/error states
at 1280×720. Include an actual screenshot when a layout changes.

Describe what changed, how it was checked and what still needs hardware
verification. Keep commits focused and record meaningful checkpoints in
`docs/DEVELOPMENT_LOG.md`. Never commit cookies, login QR keys, access tokens,
audio caches, user settings, emulator account data or crash dumps containing
private paths.

Use the repository's `.clang-format` for application C++ changes; the initial
application pass used clang-format 18.1.8. Keep third-party formatting and
license headers intact unless a targeted dependency patch needs adjustment.

For a hardware bug, include the application version, exact artifact SHA-256,
Atmosphère/HOS/overlay-loader versions, boot method, reproduction steps and
whether other system modules were enabled. Keep the matching ELF locally.
