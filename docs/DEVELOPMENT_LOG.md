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

## 2026-10-20 · API and playback checkpoints

- Implemented all 49 reference API/download methods and the four session helpers.
  Added the parameter catalog and machine-readable mapping.
- Verified WEAPI/EAPI known vectors and independently decrypted XEAPI output
  with Python cryptography. Both API translation units cross-compiled for
  AArch64. Twenty-one live read-only API operations succeeded; no real
  user's account authorization or collection writes were claimed.
- Built the independent Atmosphère ExeFS NSP and Tesla overlay.
- Audio integration tests decoded generated MP3 locally and over throttled
  HTTPS, checking pause, seek, volume, repeat/shuffle, next/previous, automatic
  advance and final-queue stop. SDL's dummy sink verifies data flow, not
  subjective audible quality.
- The actual NetEase song `347230` was resolved and streamed through the
  engine; the anonymous 45-second preview advanced normally.
- Corrected the absent-service connection path: first detect Atmosphère and
  query service availability without blocking. Eden cannot provide the
  custom `ncm:bg` service.
- Corrected queue replacement for Horizon's non-overwriting rename semantics.
  Fault-injection tests cover `EEXIST`, failed promotion and failed rollback;
  the old queue remains recoverable.
- Initialized time services explicitly in the system module so TLS
  certificate dates and request timestamps use the system clock.

## 2026-10-20 · Native GUI integration and emulator iteration

- Added the controller/touch canvas, discovery, charts, search, collection
  views, account flows, settings, mini-player, queue and full vinyl player.
- The vinyl record advances only in `Playing` state at 33⅓ RPM, with a
  rotating cover and a separate tonearm. Synced bilingual LRC is rendered
  from the actual playback position.
- Both desktop and Switch builds completed. Desktop virtual display `:120`
  loaded real playlists and covers, opened a 97-song playlist and played its
  authorized preview through the shared engine.
- Eden iteration 1 exposed unimplemented wireless-priority IPC 80. Removed
  that optional optimization. Iteration 2 exposed blocking `smGetService`
  for a missing custom service. Applied the nonblocking client fix.
- Eden iteration 3 rendered the actual NRO with Chinese text and live
  playlists/covers, then generated a real QR waiting for authorization.
- Fixed canvas-wide focus effects and ellipsized long text after inspecting
  real virtual-desktop screenshots. Fixed bilingual newline glyphs.
- Replaced inherited reference font files with redistributable DejaVu Sans
  and generated controller hint glyphs. The main canvas uses OFL Noto CJK.
- Added deterministic model tests for task ordering, busy state and stale
  navigation. Corrected nullable subscription values and paginated libraries.

Hardware-only results remain unfilled in `docs/HARDWARE_TESTING.md`.

## 2026-10-20 · Persistence, input and continuous audio regressions

- Replaced libcurl's cookie-jar output with explicit serialization using a
  recoverable backup/promotion sequence. The same storage helper protects
  settings and image downloads; shutdown reports failures without throwing.
- Added visible-range artwork scheduling, duplicate suppression and delayed
  retry, so scrolling past the initial artwork batch remains usable.
- Added wheel and touch scrolling, corrected docked touch coordinates, and
  bound directional actions to Borealis navigation buttons so analog sticks
  and horizontal Joy-Con layouts are handled.
- Fixed collection-header touch actions, isolated seek from play/skip hit
  regions, bounded scrolling after changing categories, removed duplicate
  history entries on refresh and used the playing track as intelligence seed.
- Eden exposed an `audout` lifecycle stall after a 44.1 kHz track advanced to
  a 48 kHz track. Opening a fresh audio session per track corrected it.
  A second regression found the same issue after pause/resume; pause now
  releases the output and resume seeks back to the audible position before
  creating a fresh session.
- The real ARM64 NRO subsequently passed automatic track advance,
  pause/resume and seeking. A four-second paused interval produced identical
  vinyl pixels; resumed playback produced changing PCM and advancing time.
- Cache identity now includes quality and a session fingerprint, preventing
  an anonymous preview from being reused after signing in. No raw cookie is
  stored in a cache filename or diagnostic log.
- Overlay controls are localized, animation follows elapsed time, and the
  overlay retains the service-manager reference needed for reconnecting.
- Reopening the client restores the validated queue file without starting
  playback. Per-track metadata hydration restores missing cover artwork;
  generation checks prevent stale responses from overwriting a new queue.
- Final desktop checks exercised actual text input/search, collection menus,
  restored queues, artwork hydration, native scrolling and clean exit on the
  isolated display. Public screenshots use an instrumental track and contain
  no account credentials or login QR codes.
- Source changes are committed at toolchain, API, audio, persistence, GUI
  and validation milestones. Generated packages retain matching ELF build
  IDs, source commit metadata and SHA-256 manifests.
