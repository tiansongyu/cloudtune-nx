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

## QR accounts, vector icon and expanded audio (2026-10-20)

- Removed password/SMS sign-in routes and UI; retained the original API inventory
  with the three removed methods marked as reference only (46 callable methods).
- Added transactional local multi-account storage, legacy-cookie migration,
  isolated in-memory QR authorization, cancellation generations and saved-account
  switching/removal. Old account jobs, library state and playback queues are
  invalidated before publishing a different session. AirPlay remains independent.
- Added a synchronous music-queue clearing command with an acknowledgement barrier;
  old background services reject unsupported switching instead of mixing sessions.
- Added a scalable icon adapted from the referenced GTK project, retaining its
  attribution; exported crisp desktop PNG and NRO JPEG assets from the SVG master.
- Added content detection and streaming FLAC/AAC decoders, 24-bit/192-kHz FLAC
  validation, multichannel-to-stereo mixing, eight quality request levels and
  format-neutral caches. FFmpeg remains pinned, with only AAC/ALAC codecs and
  AAC/MOV demuxers; upstream source and license packaging were updated.
- Fixed a nested-dropdown lifecycle problem discovered in actual UI testing:
  account dialogs now open after the parent dropdown has finished closing.
  Current account and quality selections are reflected in the menu markers.
- Fixed a progressive M4A issue found by the real throttled-HTTPS engine test:
  a seekable MOV demuxer had waited for the end of the media before decoding.
  Unknown-length streams now parse their headers without that premature seek.
- Host validation passed all 15 CTest checks, independent XEAPI verification,
  and generated local/throttled-HTTPS MP3 integration. New cases cover FLAC/AAC
  PCM fidelity, seeking, mixed formats, incomplete downloads, account barriers,
  cookie persistence, QR cancellation and AirPlay handoff. AAC also passed
  ASan/UBSan/leak checks and strict devkitA64 compilation.
- Tested settings and account interactions on an isolated Xvfb display using
  synthetic accounts; no live user profile or the visible iPhone receiver was
  interrupted. Switch builds use separate output directories for the same reason.
- Built all three Switch components from clean implementation commit `bb7522e`
  and verified their binary metadata, matching ELFs, source/license package and
  CI results. Both GitHub Actions jobs passed in run `39399550532`.
- Replayed generated 24-bit/96-kHz FLAC followed by M4A/AAC using that final NRO
  in an isolated Eden profile. Automatic detection, natural track advance,
  nonzero PCM, advancing clocks and rotating vinyl were observed. Host output
  was muted, so this adds emulator execution evidence rather than a listening
  result. Shader translator diagnostics and physical-console limits are
  recorded in `VALIDATION.md`; the user profile was not used for these checks.

## Cross-module code review (2026-10-20)

- Reviewed application, API/model/accounts, shared audio, AirPlay, background
  IPC, overlay, build scripts and CI. Dependency review covered integration,
  source/license packaging and static-analysis findings.
- Fixed authenticated API redirects, cookie-domain selection, private JSON
  diagnostics and integer parsing. Added a local TLS transport fixture that
  also verifies public CDN downloads after a signed-in request.
- Made damaged and empty account archives visible and non-destructive.
  Shared read-only validation protects cold background/overlay queue loading;
  existing loaded music and AirPlay are not revoked by a GUI recovery error.
- Corrected unknown-length FLAC seeking, orphan cache-marker recovery and
  decoder offset arithmetic. Added exact PCM and progressive engine regressions.
- Hardened CMIF request bounds, argument sizes and transferred-handle cleanup;
  validated status versions and bounded strings in the Switch client.
- Corrected the settings account-row hit region, UTF-8 queue truncation and
  exceptional teardown ordering. Kept curl cleanup after worker/handle teardown.
- The final host run passed 18/18 CTest checks, independent XEAPI verification
  and generated local/HTTPS playback. Rebuilt all three Switch components and
  verified package inputs. Targeted sanitizer results and isolated virtual
  desktop checks are recorded in `VALIDATION.md`.
- Added shell/Python checks to GitHub Actions and corrected outdated background
  architecture notes. No real session files or private runtime evidence were
  added to source control. Findings and remaining limits are in `CODE_REVIEW.md`.

## Second detailed review (2026-10-20)

- Added fault injection and independent cross-review around account publication.
  Queue playback now captures the committed, normalized account session; API
  authentication and cache ownership stay consistent until queue replacement.
  Expired and path-restricted cookies no longer supply custom authentication.
- Guarded first account publication with a non-secret pending marker. A failed
  publication plus failed rollback cannot expose an uncommitted legacy session,
  including retries through the same store object. Shutdown cancels pending
  account publication after a playback barrier.
- Fixed sample-offset errors in MP3 seeking and early-EOF detection when a
  reliable frame count is available. Added 1,219 bounded codec/PCM cases, with
  generated fixtures and a separate fully instrumented decoder/FFmpeg run.
- A full UBSan run reproduced out-of-range AirPlay lifecycle enums. Internal
  events now use integer storage; raw IPC commands are checked before conversion.
  Added strict AirPlay port parsing and failed-response clearing.
- Expanded RTSP/UDP/metadata, receiver restart, concurrent runtime, IPC layout
  and account/cache isolation regressions. Final native and UBSan CTest suites
  both passed 20/20. Independent XEAPI and local/HTTPS playback also passed.
- Built all three Switch components and exercised the implementation NRO in an
  isolated virtual Eden desktop: MP3/FLAC/AAC automatic detection and natural
  advance, pause/resume/seek, Chinese text and vinyl animation. Emulator
  diagnostics and the limits of muted synthetic playback are recorded in
  `VALIDATION.md`.
- Added full UBSan validation to the GitHub Actions host job. Updated account
  recovery, background session ownership and audio seeking documentation.

## v0.2.0 release preparation (2026-10-20)

- Simplified the README around direct SD ZIP downloads, installation, account
  setup, AirPlay and background playback; retained detailed development and
  validation material in linked documentation.
- Added v0.2.0 release notes and updated the installation guide. Existing
  account data stays in `config/netease-switch/`; upgrades replace all three
  compiled components together.
- Bumped the shared project version to 0.2.0 and made both the Switch overlay
  and desktop preview display that version instead of a hardcoded label.
  Overlay object dependencies now include the version source.
- Rebuilt the three Switch components, checked both NACP versions and matching
  symbols, and passed the independent overlay service regression. Publication
  uses the version tag's required CI and verified release assets.

## v0.2.1 Tesla maintenance release (2026-10-20)

- Reviewed overlay controls, native libtesla font/render/list code, both IPC
  transports and the background operations invoked by the overlay.
- Fixed unknown-duration and overflowing forward seeks, stale disconnected
  status, title scrolling after track changes, Chinese fallback on other
  system languages, glyph bitmap lifetime and reverse focus traversal.
- Removed successful IPC polling from SD diagnostics, deduplicated repeated
  failures and bounded the per-start log to 64 KiB.
- Added actual callback and native-method regressions to CTest and therefore
  the existing Actions host/UBSan jobs. Before/after tests reproduce the old
  faults; focused ASan/UBSan/leak checks and Switch compilation passed.
- Bumped the shared version to 0.2.1, updated download/install entry points and
  added separate release notes without changing the published v0.2.0 tag.
- Corrected the offline documentation package's entry links and included the
  README icon and linked background notices at their canonical paths.
- Full validation and physical-console limits are recorded in `VALIDATION.md`.
  Release publication uses the new tag's tested, verified artifacts.

## Manual Release action (2026-10-20)

- Added a `workflow_dispatch` entry to Release with an explicit version tag.
  It builds the selected ref's event commit through the same reusable CI.
- A read-only preflight validates version notes and remote state. After all
  tests and package checks pass, only the publication job may create a missing
  tag at the tested commit; existing tags, drafts and releases are preserved.
- Manual and push-triggered releases share a per-tag concurrency group.
  The uploaded artifacts are downloaded and compared before publication, and
  the remote tag is checked again immediately before making the draft public.
- Added isolated release-target tests to CI and documented the browser steps
  and the actual installable assets in `RELEASING.md`.
- All 24 target/transport tests, Python syntax, ShellCheck and actionlint
  v1.7.12 passed locally. Tests include missing/annotated/conflicting tags,
  existing drafts, HTTP failures and concurrent creation without force updates.

## Release status policy (2026-10-20)

- Changed both manual and tag-triggered publication to full GitHub Releases,
  including 0.x versions, and explicitly mark the published version Latest.
- Updated current README and release documentation to match that policy.
  Hardware validation requirements remain documented independently of the
  GitHub release label.
- Promoted the existing v0.2.1 release by updating its metadata; its tag and
  compiled assets remain the previously verified build.

## README screenshot refresh (2026-10-20)

- Captured the v0.2.1 desktop client built from `5afe484` on an isolated
  1920 × 1080 Xvfb display, using a fresh anonymous profile, disabled AirPlay
  and dummy audio output. No personal account data was used.
- Replaced the player and discovery screenshots with unmodified window
  captures showing the current icon, Chinese text, public recommendations
  and actual playback of Tycho's instrumental track “A Walk”.
- Checked playback progress and changing vinyl frames, image dimensions,
  privacy and README image paths. Added a collapsed discovery preview and
  an explicit version, capture date and desktop-source caption.

## Background failure handling — v0.2.2 (2026-10-20)

- Replaced custom-service synchronous requests with bounded asynchronous IPC.
  Preserved pending buffer ownership, cleaned late transferred handles, checked
  active loader support and reduced disconnected overlay retry frequency.
  Final shutdown cannot return a still-owned page to an NRO loader.
- Added decoder allocation limits and bounded cache-eviction candidate storage.
  Recoverable worker allocation failures now leave the player accepting
  commands; queue file ownership also survives allocation exceptions.
- Added bounded audio-output recreation that preserves the current track and
  audible position. Persistent failures pause for manual retry. AirPlay drops
  old PCM and resumes live after recovery.
- Added production-code fault tests for IPC, resources and output. The old
  engine fails the allocation-failure regression; the repaired engine passes.
  Complete native and UBSan suites pass 23 checks each.
- Fixed the SDL thread-local leak exposed by the focused ASan/UBSan/leak run.
  A further regression reproduced and fixed AirPlay FLUSH retaining the
  temporary recovery pause instead of accepting fresh live audio.
  Kept all tests on synthetic background fixtures and retained real-device
  acceptance requirements separately.
- Advanced the build version to 0.2.2 and prepared release notes. This code
  maintenance pass does not alter the published v0.2.1 tag or release.

## Follow-up risk review (2026-10-20)

- Replaced the client test's synthetic CloudTune replies with the actual
  service parser/encoder. This exposed the async client's incompatible
  reply-type assumption; accepting both supported forms fixes the handshake.
- Distinguished valid service errors from broken transport so a rejected tool
  request does not discard the client's supported music connection.
- Reproduced four audio-recovery edge cases and fixed state after seek, the
  restored music cursor after AirPlay, final-buffer draining and watchdog
  starvation by volume controls.
- Reproduced command-producer allocation failure, then removed runtime
  command-queue growth through preallocation and a reserved, coalesced End
  slot. Added an exception boundary around the resident service dispatcher.
- Expanded native, UBSan and focused ASan/leak checks with the new regressions.
  Kept the unpublished version at 0.2.2 and documented remaining HOS-service,
  hardware memory and game-coexistence boundaries.

## Native music UI redesign (2026-10-21)

- Implemented the approved CloudTune visual system in the existing Borealis /
  C++ / NanoVG view: large cover shelves, collection rows, persistent transport,
  immersive vinyl and lyrics, account and settings panels, and centered native
  menus. Added Chinese translations for the standard widget hints.
- Unified painting and touch geometry on a 1280 × 720 reference canvas, with
  proportional 1080p scaling, contain behavior and optional 95% / 90% safe areas.
  UI preferences are stored separately in `ui.json`; existing service settings
  and account storage are unchanged.
- Added explicit sidebar, header, content and transport focus regions. Sidebar
  Right returns to content without browsing. Player expansion is a UI overlay,
  preserving the Model collection history, stable media focus and scroll offset.
  Queue actions use the existing canonical queue API; all other music and account
  actions retain their existing Model / Player entry points.
- Added focus, press, page, menu, cover, lyric and tonearm transitions, with a
  reduced-motion setting. Preserved native Borealis button repeat behavior and
  the existing AirPlay L/R handoff back to NetEase playback.
- Added layout boundary tests and production MusicView tests under Xvfb,
  including partial grid rows, focus restoration, refresh reordering, queue and
  collection song indices, and the FM header action. The full native suite passed
  25 tests; final UI-only checks passed again after the visual and input refinements.
- Built all three Switch components. Checked the desktop UI and actual NRO on
  isolated virtual displays with fresh profiles, including handheld and docked
  rendering. Desktop checks cover Chinese menus, controller equivalents, 90%
  touch coordinates and loading / empty / error states. Retained 28 native fixture screenshots plus
  emulator and safe-area evidence in `docs/design/cloudtune-nx/native/`.
- Audio, networking, Model, Player, AirPlay, IPC, background service and overlay
  implementation files were not modified. Physical Switch / Joy-Con feel, TV
  overscan and game coexistence remain hardware acceptance items. The native
  implementation report is `docs/design/cloudtune-nx/implementation.html`.

## Optional local artwork and release preparation (2026-10-21)

- Added an optional native photo gallery with four contained images, directional
  and shoulder-button selection, 220 ms crossfades, reduced-motion support and
  B return to the previous music focus. Opening it preserves the Model history,
  queue and playback; Y continues to use the existing play/pause action.
- Added a gently shaded discovery banner and optional player ambience. Original
  gallery photos retain their composition; only the panorama is a generated
  composite. Text contrast is protected by fixed scrims. The bounded texture
  cache is reused while switching images.
- Kept all personal artwork and private validation captures outside version
  control. The public CI checkout and release use the standard theme. All five
  optional artwork files must exist before the gallery is exposed; tested this
  fallback and its FM action mapping with the resources absent.
- Native production-view tests passed with and without optional resources.
  Inspected 720p / 1080p photo scenes on an isolated virtual display. Rebuilt
  all three Switch targets without personal resources for public evidence.
- Final Eden rendering was checked separately from input. Keyboard injection
  failed in the final build and the released v0.2.1 baseline in the same isolated
  environment; this remains an emulator input limitation, not a passed
  controller acceptance test. Physical Switch / Joy-Con and game coexistence
  remain unverified. No diagnostic logging was retained in application code.
- Updated the v0.2.2 release notes and README. Added Xvfb / xauth to CI so the
  production Borealis controller regression runs in both native and UBSan jobs.

## Discovery song list overlap fix (2026-10-21)

- Reproduced daily recommendations overlapping the retained discovery banner
  and section heading. FM uses the same song-list state and shared the defect.
- Kept discovery content below its header and derived song capacity from the
  available height, including focus clearance above the mini player. Painting,
  scrolling and touch continue to share the same geometry. No Model, Player,
  audio, networking or service changes were needed.
- Added a regression that failed before the fix and passed afterward, covering
  header boundaries, last-row focus, scrolling to the final song, touch indices
  and returning to the header. UI tests passed with and without personal assets.
- Reviewed native 720p and 1080p scenes in both themes and rebuilt all three
  Switch components. A fresh Eden profile loaded the actual private NRO and
  displayed an anonymous FM result below the banner after touch activation.
  Logged-in daily recommendations use production-view fixture coverage; no
  account credentials were copied into the validation profile.
- Prepared v0.2.3 with public screenshots and release notes. Personal artwork
  remains local and excluded from the public package. Evidence and remaining
  hardware limits: `docs/design/cloudtune-nx/daily-layout-v0.2.3.md`.

## Vinyl animation speed calibration (2026-10-21)

- Verified the first-party stylesheet referenced by NetEase's official mobile
  song sharing page: a linear, 20-second rotation, with paused animation state.
  Recorded the URL, content hash and scope of that evidence in
  `docs/design/cloudtune-nx/vinyl-speed-v0.2.4.md`.
- Changed only the native rotation rate from 1.8 to 20 seconds per revolution.
  Kept the playback/reduced-motion guards and angle continuity. Updated the
  design tokens, Web prototype and current UI documentation to match.
- Existing UI tests passed. Sampled the actual production view at 720p and
  1080p over 20 seconds; quarter turns and the completed revolution matched
  the intended speed within screenshot timing precision.
- Built all three Switch components and booted the actual photo NRO with a
  fresh, isolated Eden profile. Anonymous FM playback and resume both rotated
  at approximately 18 degrees/second; the paused disc remained pixel-identical
  over two seconds. Prepared v0.2.4 without changing audio, network,
  account or background-service implementation. Personal assets remain local.

## Focus, artwork and first-play repair (2026-10-21)

- Reproduced the reported third-chart focus jump before changing production
  code. Deferred collection focus initialization until the destination arrives,
  and committed the displayed snapshot together with restored focus on Back.
- Reproduced deletion of an image still referenced by the current NanoVG frame
  when the old cache reached 48 entries. Replaced filename-order eviction with
  frame-boundary LRU trimming, retried failed visible textures and started the
  artwork reveal when the texture actually becomes available.
- Added the requested first-play behavior to the existing foreground controls:
  current song list first, or the first recommended card's first song once its
  exact collection loads. Repeated requests, errors, account changes and Back
  do not start stale music. Audio, network and service implementations unchanged.
- Desktop CTest passed 25/25, including the new production-view regressions.
  Built all three Switch components. Inspected native 720p/1080p scenes and
  used an isolated Eden profile for actual NRO validation: initial Y playback,
  pause, Original chart entry/return and scrolling through all 64 chart cards.
  A 240-frame transition recording showed no return to the first card after
  Original became focused. Covers remained visible after exceeding the cache.
- Prepared v0.2.5. Details and hardware limits are recorded in
  `docs/design/cloudtune-nx/ui-refresh-v0.2.5.md`; personal photos and test captures
  remain ignored local files, excluded from the public build.

## Content ownership and selection safety (2026-10-21)

- Added a production Model / MusicView regression first; the old code failed
  12 assertions for failed-page stale content and selection between model/frame
  updates. New routes now clear their view content synchronously while keeping
  the playback queue; failed pagination preserves its loaded list.
- Input uses the displayed snapshot. Model playback checks content generation,
  page/account and song ID, resolves the canonical position and copies/publishes
  the queue under one lock. Removed index-only Model playback entry points so
  the UI cannot accidentally reintroduce the second-snapshot race.
- Extended regressions to removed songs, queue reorders, account/collection
  changes, all affected root modes, pagination failure, same-route generation
  changes and model updates after the UI activation guard.
- Desktop CTest passed 26/26; all three Switch components built. Native 720p and
  1080p inspection passed. Actual NRO in an isolated Eden profile passed the
  loaded-FM / network-failure / Charts / Play / restore-and-retry sequence.
  Touch and controller selected the displayed third and fourth songs; pause,
  options and returning to the Original chart retained expected behavior.
- Prepared v0.2.6 with public/private asset separation. See
  `docs/design/cloudtune-nx/content-safety-v0.2.6.md` for evidence and remaining
  audit items; audio/network/service implementation and IPC are unchanged.
