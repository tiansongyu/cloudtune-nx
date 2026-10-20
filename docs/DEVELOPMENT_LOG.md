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

## 2026-10-20 · User-assisted sign-in and authenticated reads

- Opened the final NRO through Eden's Qt interface on the user's current
  desktop, using the existing isolated emulator profile.
- Explained the missing-decryption-keys prompt: the self-built NRO runs
  without those keys. Also disclosed Eden 0.2.1's SSL-verification limitation.
- The user authorized sign-in; the NRO displayed an authenticated account.
- Used the existing C++ API implementation and read-only cookie loading for
  eight authenticated host HTTPS checks. Login status, liked IDs, playlists,
  subscribed albums/radio, cloud files, recommendations and personal FM all
  returned success. No account mutation or public identity data was recorded.
- Kept the user's emulator session running; physical-console BGM and overlay
  acceptance remain separate from these account/API results.

## 2026-10-20 · Chinese text in native dialogs

- A user screenshot exposed missing Chinese glyphs in the Switch account
  confirmation dialog, while the music canvas still rendered Chinese.
- Borealis widgets selected `zh-Hans` from the emulator's shared fonts; the
  canvas separately loaded bundled Noto under a private font name.
- The Switch application now selects bundled Noto before Borealis loads its
  default font. The canvas reuses that same CJK handle, removing the duplicate
  font allocation while retaining platform controller/icon fallbacks.
- Docker NRO and desktop builds passed. Actual NRO verification on a fresh
  isolated Eden profile at `:121` confirmed account-dialog and quality-menu
  Chinese, A/B hints, checkmarks and system icons; the app exited normally.
- Evidence: `build/validation/font-fix-account-dialog.png`,
  `font-fix-quality-dialog.png` and `font-fix-tested-SHA256SUMS`.
  The regression profile contained no account credentials.
- At the user's request, further GUI regression runs stay on isolated virtual
  desktops; the current desktop is reserved for the user's own interaction.

## 2026-10-20 · CloudTune NX project identity

- The user selected **CloudTune NX** with canonical repository
  `https://github.com/tiansongyu/cloudtune-nx`.
- Current build and installation documentation uses `CloudTune.nro`,
  `cloudtune-background.nsp`, `CloudTune.ovl` and the `CloudTuneNX` package
  prefix. App and overlay symbols retain separate directories.
- Session storage remains `config/netease-switch/`; the `ncm:bg` IPC service,
  program ID `42000000004E434D` and existing Eden profile remain compatible.
- New environment variables are `CLOUDTUNE_DATA` and `CLOUDTUNE_IMAGE`,
  with the original variable names retained as fallbacks.
- Earlier logs, binary names, hashes and screenshots retain their historical
  meaning. A renamed build must have its own validation and package manifest;
  the documentation change does not imply new runtime or hardware results.

## 2026-10-20 · Publication checks and redistributable history

- Rebuilt all three CloudTune NX components with the Docker toolchain and
  checked package headers, program ID, NACP branding and matching ELF IDs.
- Passed all five host tests, the independent XEAPI verifier and local/HTTPS
  audio integration checks. The actual NRO passed anonymous startup and
  Chinese native-dialog regression on isolated virtual display `:121`.
- Refreshed public discovery and vinyl screenshots from the desktop client
  on a separate anonymous virtual display. No user session data was copied.
- Audited every historical blob before publishing. Two proprietary font
  blobs from the reference baseline were replaced with the current licensed
  DejaVu assets, and their license was added to the initial snapshot.
- Preserved all ten existing development milestones, author/committer
  identities, timestamps and messages. Only historical font assets and their
  license changed; the current source tree remained identical. Commit IDs
  changed as a consequence. Earlier local validation hashes retain their
  original historical meaning.
- Kept an ignored local backup bundle and commit map. The publication branch
  cannot reach the two original font blobs; private runtime files, credentials
  and generated artifacts are excluded from the source repository.

## 2026-10-20 · CI/CD and release packaging

- Added GitHub Actions host tests and Docker Switch builds for pull requests,
  `main` pushes and manual runs. Version tags reuse that workflow before
  a separate, narrowly permissioned release job can publish.
- Release publication selects the exact artifact from the same run, checks
  tag/version/commit consistency, uploads a new draft, downloads its assets
  and compares their bytes before making it public. Existing versions and
  assets are not replaced.
- Moved overlay binaries and intermediate files under `build/overlay/`;
  staging lives under `build/package/` and the five public attachments under
  `dist/release/`. Timestamped local candidates remain supported.
- The application settings version and overlay NACP now derive from the
  CMake project version. The packager checks NACP versions, ExeFS/NPDM,
  matching ELF IDs, exact SD layout, ZIP CRCs and per-file SHA-256 hashes.
- Release mode requires both a clean source tree and matching build
  provenance. Documentation archives retain offline instructions, images
  and dependency licenses; private runtime directories are not packaged.
- Workflow lint and positive/negative version checks passed locally.
  Hosted CI outcomes are recorded by the corresponding GitHub Actions runs;
  each published release links its own run and source commit.

## 2026-10-20 · Interactive overlay preview

- Attempted the actual OVL in an isolated Eden 0.2.1 profile. The emulator
  exited before rendering because Tesla could not acquire HOME/SLEEP event
  handles. Preserved the exact binary hash, error log and black-screen result.
- Added an optional native preview that compiles the production overlay UI
  unchanged through an SDL2/stb adapter with bundled Chinese fonts.
- Kept the GUI and GPLv3 audio engine in separate processes, using the shared
  status/command ABI over a private Unix socket. A generated local queue
  avoids account access; a new playback process starts paused and muted.
- Added a launcher, explicit process cleanup, isolated build/runtime
  directories and a reusable 26-check service regression in GitHub Actions.
- Native builds, CTest, nine client boundary checks and 13 isolated GUI
  checks passed. Screenshot comparison verified rotating vinyl during
  playback and a stationary disc while paused. Closing the window left
  playback advancing; resized mouse hit testing also passed.
- The user requested a visible desktop demonstration for this task.
  Preparation remained isolated; the final preview is explicitly labeled
  as desktop simulation. Atmosphère/Tesla and game BGM remain hardware checks.

## 2026-10-20 · AirPlay 1 tool integration

- Added a separate classic RAOP receiver and Runtime owner for NetEase music
  and incoming stereo audio. Receiver callbacks hand bounded PCM to the
  existing audio worker; only one source owns output at a time.
- Opening the application starts tools according to independent
  `airplay.json` preferences. Reception defaults to enabled on first use,
  and a saved disabled preference survives restarts. Settings expose actual
  device name, IPv4 address, state, enable/disable and session disconnect.
- The NRO prefers the background owner and avoids creating a local listener
  when a known service is still starting or an old service lacks tool APIs.
  AirPlay status uses new IPC commands 30–32; the original 216-byte playback
  status remains unchanged for existing overlay clients.
- Added mutex protection around Player backend state and tool/music calls.
  The background owns Runtime beyond the NRO lifetime; local fallback stops
  with the NRO. Incoming audio suspends the music queue, and ending a session
  restores its saved position paused for explicit user resume.
- Pinned an ALAC-only FFmpeg 7.1.5 build with a verified archive SHA-256.
  Retained its upstream license texts and Shairport Sync's MIT protocol/key
  attribution; no old Shairport ALAC or tinysvcmdns implementation is linked.
- Host and Switch integration syntax checks passed. Isolated Player
  substitution tests covered concurrent calls, shutdown, old-service
  compatibility and prevention of a second listener. The generated-audio
  regression passed bounded PCM, resampling, pause, flush, exclusive handoff
  and restoration of the music cursor.
- Review found settings rollback, stale settings-error and disconnect
  acknowledgement issues; the Runtime implementation was corrected.
  External audio now reports output-device errors or stalls, and restoring
  a saved music position checks seek failure instead of silently continuing.
- Independent encrypted RAOP tests passed exact ALAC/L16 PCM checks,
  challenge responses, timing exchange, retransmission, FLUSH, PAUSE,
  metadata, single-owner behavior and malformed-request rejection.
  An occupied port recovers through automatic retry, and retry waits remain
  promptly cancellable. The receiver retries network/listener failures every
  two seconds and detects automatic IPv4 interface changes every three seconds.
- These are implementation and component checks, not a completed real-phone
  or physical-console acceptance result. Apple sender compatibility,
  long-running reception, Atmosphère/game coexistence and the 16 MiB
  background heap need end-to-end validation. Sleep reception, AirPlay 2,
  video, mirroring and multiroom synchronization are not claimed.
- The actual desktop application was discovered by unmodified pyatv 0.16.1
  and received a complete generated 45-second stream. Isolated Xvfb checks
  verified the Chinese settings dialog, persistent disable, listener removal,
  track metadata, and rotating/paused vinyl. This exposed and fixed the UI's
  assumption that every active source has a NetEase track ID.
- Full Docker builds produced all three Switch components. The complete host
  suite passed 10/10 tests plus the existing local/HTTPS music regressions.
  The new RAOP checks run automatically in the existing GitHub Actions host job.
- ASan/UBSan checks instrumented the ALAC library as well as the receiver.
  Mutation tests exposed malformed UTF-8 metadata; validation/replacement was
  added and retested with Chinese and invalid-byte fixtures.
- Packaging now includes the pinned FFmpeg source archive, build recipe and
  upstream licenses alongside the retained Shairport notice. The verifier
  checks their checksums and completeness while retaining the five-asset
  release layout. No account or runtime files are included.

### AirPlay receiver in Eden

- Opened the actual Switch NRO on the visible desktop using an isolated
  emulator profile and a separate control port from the existing host
  receiver.
- Fixed Eden compatibility: poll only active descriptors and use zero
  send flags on Switch. Native builds retain SIGPIPE suppression.
- Added an optional Linux/Avahi discovery helper because Eden 0.2.1
  ignores IP-level multicast socket options. Only discovery runs on the
  host; the emulator NRO receives and decodes the audio.
- Confirmed RTSP replies, independent discovery and a live LAN sender
  session with Chinese track metadata and an advancing player clock.
  The local test sender correctly received an occupied-session rejection
  while the phone session owned the receiver, so it did not replace it.
- Kept the phone's session running for user testing. The user explicitly
  confirmed that real iPhone playback was audible and normal in the
  emulator. Detailed iPhone controls, long-run stability and physical
  Switch acceptance remain separate checks.
