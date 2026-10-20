# Validation record

This document distinguishes checks actually executed from checks still
requiring application integration or a physical Nintendo Switch.

The project is now named **CloudTune NX**. Earlier artifact names below are
retained as historical evidence; current filenames and commands are documented
in [BUILDING.md](BUILDING.md). Renaming an artifact does not constitute a new
hardware or emulator validation.

## CloudTune NX publication build — 2026-10-20

The renamed build passed Docker compilation for `CloudTune.nro`,
`cloudtune-background.nsp` and `CloudTune.ovl`. Package validation checked
the NRO/NSO headers, program ID, branding metadata and all three matching
ELF build IDs. The host suite passed 5/5 CTest checks, independent XEAPI
verification and generated local/throttled HTTPS MP3 integration checks.

The actual NRO ran in Eden 0.2.1 on isolated virtual display `:121`.
Discovery, the CloudTune branding, Chinese account confirmation and quality
menus rendered correctly, including controller hints and system icons. The
anonymous test profile contained no user credentials; the process exited
normally. README images were captured separately from the same-source desktop
client on `:122`. These checks do not establish physical-console BGM behavior.

Local evidence is retained under `build/validation/`:

- `cloudtune-switch-build.log`, `cloudtune-artifacts.json`
- `cloudtune-host-tests.log`
- `cloudtune-eden-start.log`, `cloudtune-eden-tested-SHA256SUMS`
- `cloudtune-eden-start.png`, `cloudtune-eden-account-dialog.png`,
  `cloudtune-eden-quality-dialog.png`

Tested binary SHA-256:

```text
a23b2d33d8c469b36f99fb7470426b8605c9929069b93c188bc9893352f11ca4  CloudTune.nro
9a61da5b5979bd0f112583ad5fdfc8edcd4aeb396d69f26e2075c084eb37c8da  cloudtune-background.nsp
f087f944b5159639752e2b39dc03a1f5bf3e4499a4671750954903a0cd0de348  CloudTune.ovl
```

## Toolchain and emulator baseline — 2026-10-20

| Check | Result | Evidence |
| --- | --- | --- |
| Docker daemon available | Passed; Docker 28.2.2 | `docker version` |
| Pinned devkitPro image | Passed; manifest `sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282` | `docker/Dockerfile` |
| AArch64 compilation | Passed; devkitA64 r29.2, libnx 4.12.0 | `build/validation/compiler.txt`, `toolchain-packages.txt` |
| NACP and NRO packaging | Passed | `build/smoke/toolchain-smoke.nro` |
| Smoke ELF architecture | Passed; ELF64 AArch64, position independent | `build/validation/toolchain-smoke.log` |
| Eden installation | Passed; official v0.2.1 Linux amd64 clang PGO | `docs/EMULATOR.md` |
| Virtual desktop | Passed; independent Xvfb displays `:119` and `:120` | `build/validation/xvfb*.log` |
| Smoke NRO boot and rendering | Passed in Eden on `:119` | `build/validation/eden-smoke.png`, `eden-smoke.log` |
| Vulkan renderer | Passed; NVIDIA GeForce RTX 4090, driver 580.142.0.0 | `build/validation/eden-smoke.log` |
| Homebrew without firmware or keys | Passed for the smoke NRO | Eden synthesized required system archives |
| HTTPS root bundle | Downloaded and verified against curl.se SHA-256 | `resources/certs/README.md` |

Smoke binary SHA-256:

```text
31afcc9574fc57f1c72e8d62502b36a52ec3bdc111391bf9eeaf624aca2bf5c4  toolchain-smoke.elf
7aa6318690d7bc5a09e90bfa34f9f4375a2a63def9671d3c405915861949f0e2  toolchain-smoke.nro
```

Reproduce with:

```sh
./scripts/docker-build.sh --smoke
./scripts/eden-virtual-desktop.sh --cli \
  -g "$PWD/build/smoke/toolchain-smoke.nro"
```

## Complete NRO application checks — 2026-10-20

The compiled `build/switch/NeteaseSwitch.nro` was loaded in Eden 0.2.1 on
Xvfb `:119`, using Vulkan, without console keys or firmware. These checks
exercise the ARM64 application, not the native desktop executable.

| Check | Observed result | Local evidence |
| --- | --- | --- |
| Full Borealis UI startup | Passed after compatibility fixes below | `eden-app-third.png` |
| Chinese UI text | Passed; bundled CJK font rendered navigation, titles and status text | `eden-app-third.png` |
| Chinese text in standard dialogs | Passed after selecting bundled Noto as Borealis's default CJK face before widget creation; account confirmation and quality options are readable and controller icons remain intact | `font-fix-account-dialog.png`, `font-fix-quality-dialog.png` |
| Live discovery API | Passed; real public playlists and artwork appeared | `eden-app-third.png` |
| Playlist details | Passed; opened a live playlist containing 97 songs | `eden-playlist.png` |
| Touch navigation | Passed in handheld and docked emulation after coordinate fix | `eden-account-handheld.png`, `eden-regression-docked-account.png` |
| Touch scrolling | Passed; dragging a song list moved from row 1 to row 5 without playing the touched row | `eden-touch-scroll.png` |
| Login QR generation | Passed; real NetEase QR key rendered and waiting-for-scan status appeared | `eden-account-qr.png` (private, ephemeral login key; do not publish) |
| Account authorization | User subsequently authorized sign-in in the current-desktop NRO; authenticated host API readback passed | See the dated authenticated section in `API_VALIDATION.md`; no private screen is published |
| Initial online track playback | First track reached 30-second preview end and advanced to the next track | `eden-playing-01.png`, `eden-playing-03.png` |
| Animated vinyl record | Passed; successive playback frames showed different record/cover angles | `eden-playing-01.png` through `eden-playing-04.png` |
| Track metadata and lyrics | Passed; bilingual lines render separately and follow the current lyric | `eden-regression-vinyl-first.png`, `eden-regression-second-advancing.png` |
| Continuous playback across tracks | Passed after recreating the audio-output session per track; 44.1 kHz preview advanced into a 48 kHz full stream | `eden-regression-audio-first.log`, `eden-regression-audio-second.log` |
| Seek | Passed in active playback; clicking the progress bar beneath the play button seeks without toggling playback or changing the song | `eden-pause-fix-seek-under-play.png` |
| Pause/resume | Passed after recreating the output session and restoring the audible cursor; same track resumed with an advancing clock and changing PCM | `eden-pause-fix-paused-01.png`, `eden-pause-fix-resumed.png`, `eden-pause-fix-audio.log` |
| Paused vinyl | Passed; the record area was pixel-identical in two captures four seconds apart, then rotated again after resume | `eden-pause-fix-paused-01.png`, `eden-pause-fix-paused-02.png` |
| Seek while paused | Passed; seek remained paused at 0:30, then resumed from that position | `eden-pause-fix-paused-seek.png`, `eden-pause-fix-paused-seek-resumed.png` |
| Manual next track | Passed; a 48 kHz track changed to a 44.1 kHz preview and its clock advanced | `eden-pause-fix-next.png`, `eden-audio-pause-fix.log` |
| Final-build queue restoration | Passed; reopening the NRO restored the saved song list without automatically playing; selecting a restored song loaded its artwork/lyrics and played | `eden-final-restored-queue.png`, `eden-final-restored-playing.png` |

The audio checks monitor only Eden's PulseAudio sink input. The first track
produced PCM in the range −21515…20902, and the second −21694…21896;
both had nonzero sample-to-sample differences and zero crossings. This
distinguishes changing decoded audio from the constant DC output observed in
the earlier failed build. Full-song availability is determined by the
anonymous account's service entitlement; the first track was a 30-second
preview, clearly labeled in the UI.
After pause/resume, the same 48 kHz track produced PCM −21553…21504
with an RMS sample difference of 433.03, while its playback position advanced.

All filenames in the evidence column are under `build/validation/`.
`eden-passed-build.json` records the hashes of the binaries used for the
complete audio/input regression. `eden-final-build.json` records the final
build's startup, queue restoration, audio and pause/resume recheck; that run
also exited cleanly through the app's Settings page.
Screenshots containing a live QR key or account data are development evidence
and are deliberately excluded from public documentation.

Issues discovered during this run:

- Eden asserts on Borealis's optional `SetWirelessPriorityMode` IPC call.
  The application now omits that unnecessary hint.
- `smGetService("ncm:bg")` waits forever when the background module is
  absent. The client now gates Atmosphère extensions and checks service
  presence before connecting.
- Horizon filesystem rename does not overwrite an existing destination.
  Cookie/queue persistence needed a Switch-compatible replacement sequence;
  the subsequent run no longer logged overwrite failures.
- Eden's internal network logs contain `assert flags == 0` and SSL
  `PendingImpl` stub messages even while API and artwork requests succeed.
  These messages remain emulator limitations; they must not be interpreted
  as proof that all network paths fail or that all paths pass.
- The initial docked-mode touch transform selected the wrong sidebar row.
  The app's Switch touch mapping was corrected and docked regression passed.
- Reusing a stopped audio-output session left stale buffers after a track
  change or pause/resume. The engine now opens a fresh session and restores
  the audible cursor when resuming. Both cases passed a subsequent NRO run.
- Eden 0.2.1 forces certificate verification off in its SSL service
  implementation. This is an emulator limitation, not an application setting;
  see the source-linked explanation in [EMULATOR.md](EMULATOR.md). The run
  uses anonymous requests and does not establish TLS rejection behavior.

## Host validation — 2026-10-20

The final native suite completed with exit status 0. The complete log is
`build/validation/host-suite-final.log`.

| Check | Result |
| --- | --- |
| CTest | 5/5 tests passed |
| Independent XEAPI vectors | Passed |
| Generated local MP3 integration | Passed; progressive decoding, playback clock, pause/resume, seek, queue controls and track transitions |
| Throttled HTTPS MP3 integration | Passed; progressive transfer and playback over HTTPS |
| Cache isolation | Passed; different sessions and quality settings do not reuse each other's cached audio |

These are host-side engine/protocol checks. They complement the ARM64 NRO
results above; they do not replace console IPC, audio or account testing.

## Hardware checks

The checks above are not a claim that every NetEase API or Atmosphère
background playback has passed.
The final hardware run must record firmware, Atmosphère and overlay-loader
versions, installed binary hashes and matching ELFs.

- Account sign-in, expiration, sign-out and saved-library retrieval.
- Search, playlist navigation, unavailable/paid/regional tracks and errors.
- Playback, pause, seek, track changes, queue order and volume.
- Returning to HOME and a foreground game while audio continues.
- Overlay controls while the NRO is closed.
- Sleep/resume, lost Wi-Fi, reconnection and expired media URLs.
- Multi-hour playback, memory growth and audio interruption.
- Crash-report symbolication against the exact tested ELF.

## Overlay preview — 2026-10-20

The real `CloudTune.ovl` was attempted in isolated Eden 0.2.1 and failed
before rendering: Tesla's HOME/SLEEP event handles were unavailable,
followed by `0xE401`. The exact hash and failure evidence are recorded in
[OVERLAY_PREVIEW.md](OVERLAY_PREVIEW.md).

An optional Linux preview compiles the same production overlay UI with
an SDL2 adapter and connects to the actual engine in a separate process.
Generated local MP3s require no account or network.

| Check | Result |
| --- | --- |
| Native GUI and playback service builds | Passed |
| Service regression | 26 checks passed; also passed through CTest |
| Client protocol boundary checks | 9 checks passed |
| Launcher process lifecycle checks | 10 mock checks passed, including duplicate launch and cleanup after rebuilding |
| Isolated Xvfb UI checks | 13 checks passed |
| Chinese text | Visually inspected; bundled Noto rendered correctly |
| Vinyl animation | Screenshot comparison changed during playback and remained unchanged while paused |
| UI controls | Play/pause, L/R, volume, repeat, shuffle, stop, reconnect and mouse clicks affected the independent engine |
| Window resizing | Mouse hit testing passed at both 1280×720 and 1600×900 |
| UI closure | Playback position continued advancing after the window exited |

Local evidence: `build/validation/overlay-preview-ui-tests.json`,
`overlay-preview-client-tests.json`, `overlay-preview-service-regression.json`,
`overlay-preview-launcher-tests.json`,
and the `overlay-preview-playing-*.png` / `overlay-preview-paused-*.png`
screenshots. The desktop demo starts muted. Service regression uses SDL's
dummy audio device; these results verify decoding and control behavior,
not hardware sound quality, real Tesla loading, or game audio coexistence.

## AirPlay 1 receiver — 2026-10-20

The development branch adds a separate classic RAOP receiver. Tests used
generated audio, isolated data directories and loopback sockets; no account
session or existing desktop playback service was used.

| Check | Result |
| --- | --- |
| Host suite | 10/10 CTest checks passed, plus independent XEAPI and local/HTTPS MP3 integration |
| ALAC / PCM | Independent FFmpeg-encoded compressed ALAC decoded to exact expected PCM; bounded resampling and source handoff passed |
| RAOP protocol | RSA challenge, RSA-wrapped AES key, encrypted RTP, timing, retransmission, sequence wrap, FLUSH, PAUSE/RECORD and reconnect passed |
| Receiver lifecycle | Single sender ownership, malformed request rejection, occupied-port retry, prompt shutdown and persisted enable/disable passed |
| Sanitizers | ASan/UBSan with the ALAC decoder instrumented: protocol suite, 24,000 codec/crypto/mDNS mutations and 2,880 malformed UDP inputs across 12 recoverable sessions passed |
| Discovery | PTR/SRV/TXT/A, legacy unicast replies and goodbye tested; unmodified pyatv 0.16.1 discovered the actual application |
| Independent sender | pyatv completed a 45-second L16 stream to the actual Runtime/Engine; diagnostics confirmed nonzero PCM entering SDL's dummy output |
| Settings UI | Chinese dialog inspected in isolated Xvfb; disabling closed the listener and discovery; restarting retained the disabled preference |
| Player UI | Sender title/artist displayed, old music lyrics/controls suppressed, playback/pause changed the vinyl animation |
| Animation comparison | 32,448 changed pixels in the playing record region; zero changed pixels while paused |
| Switch build | Docker produced and verified the NRO, background ExeFS package, overlay and matching AArch64 ELFs |

The unmodified pyatv sender has a UTF-8 DMAP length issue in its
`string_tag` implementation: it writes character count instead of encoded
byte count. Its interoperability run therefore uses ASCII metadata.
Correctly framed Chinese metadata is checked separately by the independent
protocol fixture; the receiver does not weaken its length checks to accept
malformed sender data.
Mutation testing also found invalid UTF-8 metadata reaching UI status.
The receiver now replaces invalid bytes and controls while preserving
complete valid code points; regression and sanitizer checks passed afterward.

Local evidence is under `build/validation/airplay-*`, including
`airplay-host-validation.log`, `airplay-receiver-tests.log`,
`airplay-docker-build-final.log`, and `airplay-ui/`. Test audio, sender
debug logs and temporary runtime settings are ignored by Git and excluded
from deployment packages.

These results establish host interoperability and Switch build correctness.
They do **not** establish real iPhone/macOS compatibility, physical Switch
sound quality, long-running Wi-Fi clock stability, sleep/wake recovery or
Atmosphère reception while a game runs. Those checks remain in
[HARDWARE_TESTING.md](HARDWARE_TESTING.md).

### Actual NRO and iPhone session in Eden

A subsequent visible-desktop run used the actual Switch NRO in Eden
0.2.1, with host Avahi supplying discovery only. The NRO handled RTSP,
audio packets, decoding and output. It displayed Chinese sender metadata
and an advancing clock; the user explicitly confirmed that music cast
from the real iPhone was audible and played normally.

Bring-up fixed negative-descriptor polling and unsupported Switch send
flags. A strict Linux poll interposer now exercises the complete receiver
suite in CI; all 11 native tests, including the overlay regression, passed.
Details and reproduction steps are in [EMULATOR.md](EMULATOR.md).
This session does not establish physical-console background playback or
long-duration reliability.

## Audio formats and QR account update — 2026-10-20

This section describes the newer development branch; previous release records
above retain their original scope. No real account credentials are test fixtures.

| Check | Observed result |
| --- | --- |
| Full host script | 15/15 CTest checks, independent XEAPI verification and generated local/HTTPS MP3 integration passed |
| FLAC decoding | Generated 16/24-bit mono/stereo at 44.1/48/96/192 kHz; reference PCM, seek, native/Ogg/ID3 and misleading extensions passed |
| Multichannel | FLAC quad/5.1/7.1 and AAC 5.1/7.1 mixed to stereo against independent FFmpeg reference output |
| AAC decoding | ADTS and M4A faststart/tail-moov, 44.1-kHz mono and 48-kHz stereo; PCM tolerance at most one S16 unit, priming/tail/seek/cancellation and malformed-input checks passed |
| Progressive engine playback | Throttled local HTTPS FLAC, AAC ADTS and faststart M4A started before the completed-cache marker; pause/seek/resume and later offline-cache replay passed |
| Shared engine | MP3/FLAC/AAC mixed queues, normal controls, AirPlay handoff, synchronous account clearing and pending-load races passed |
| Account persistence | Migration, UID deduplication, restart, active/nonactive removal, failed-save rollback, stale QR response rejection, cancellation during the playback barrier and session rotation passed |
| UI | Isolated Xvfb with synthetic accounts: switching persisted the selected UID, both accounts remained saved, the queue tombstone was written; all eight quality options were reachable and the selected level persisted |
| Icon | SVG rendered to 512px PNG / 256px baseline JPEG; embedded NRO bytes matched the committed JPEG exactly |
| AAC instrumentation | ASan/UBSan/leak checks and strict devkitA64 compilation passed |
| Switch build | Clean Docker build produced the NRO, background ExeFS and overlay; versions, matching AArch64 ELFs, corresponding sources and licenses passed package verification |
| GitHub Actions | Both jobs passed for implementation commit `bb7522e`: native tests, overlay preview, Switch compilation and release-package verification |

Local evidence is retained under `build/validation/update-host-tests.log`,
`build/validation/flac/`, `build/validation/aac/` and
`build/validation/account-ui/`. These directories are ignored and are not
included in public source or release packages.
The corresponding public CI run is
[39399550532](https://github.com/tiansongyu/cloudtune-nx/actions/runs/39399550532).

### Final NRO playback in Eden

The clean implementation build was also tested in an independent Eden profile
on virtual display `:192`, using generated local audio. AirPlay was disabled,
NetEase requests were blocked with a loopback proxy and host output was muted.
No real account credentials were copied into the test profile.

The final tested NRO has SHA-256:

```text
dad47a11144b79e3908acbaf192c16bf2b6dd0f9520acf8a3841cc35e59771e5
```

It automatically identified a stereo 24-bit / 96-kHz FLAC file, played it,
and advanced naturally to stereo 44.1-kHz AAC-LC in M4A. Both formats produced
nonzero PCM and advancing playback clocks. Screenshots at 0:05 and 0:09 show
the rotating vinyl for both tracks; Chinese text and the vector-derived icon
rendered correctly. Evidence is under `build/validation/update-eden/final/`.
The earlier exploratory run is retained separately and used a different NRO.

This is actual NRO decoding and output-clock evidence, not a human listening
confirmation. Eden emitted initial shader translator `Invalid insn` diagnostics
while continuing to render; this is not a zero-warning emulator log.
The test does not establish Atmosphère service / overlay behavior or music
playing alongside a game on a physical console.

High-quality source decoding is not bit-perfect hardware output: the current
shared player outputs stereo S16 at 48 kHz with its existing linear resampler.
AAC exact seeking decodes forward from the beginning; long tracks can seek more
slowly. Tail-moov M4A waits for its trailing index. Real multi-account server
sessions, entitlements and concurrent Atmosphere/game playback still require
user/device validation. See [audio details](AUDIO_FORMATS.md) and
[account behavior](ACCOUNTS.md).

## Code review regression pass — 2026-10-20

The [code review](CODE_REVIEW.md) records the scope, findings and remaining
limits. Implementation commit `a15817c` was validated with synthetic account
data and generated audio, independently of the user's emulator profile.

| Check | Observed result |
| --- | --- |
| Complete host script | 18/18 CTest checks passed, followed by independent XEAPI verification and local/HTTPS MP3 integration |
| API transport | A loopback TLS proxy verified authenticated redirect rejection, cookie-domain selection, private parse-error handling, strict status codes and CDN redirects after authentication on the same curl handle |
| Account recovery | Malformed and empty primary/backup archives are rejected without replacing the archive/mirror; GUI navigation and playback stay guarded |
| Saved playback | Six combinations of damaged/empty archive and cold Play/PlayPause/LoadQueue refused the disk queue; repair allowed playback, and later archive damage did not interrupt already-loaded music |
| FLAC/cache | Unknown-length FLAC seeks matched PCM; orphan completion markers recovered; progressive controls, cancellation and mixed-format queues passed |
| IPC | Actual parser/client code passed host guard-page/layout/status tests and strict devkitA64 compilation; parser/client sanitizer runs also passed |
| Instrumentation | Targeted audio ASan/UBSan/leak checks and UBSan metadata arithmetic checks passed; these are not whole-program sanitizer coverage |
| Virtual desktop | The previously missed top edge of the settings account row opened the Chinese account menu; normal UI exit returned 0 and an invalid data directory returned 1 with a diagnostic |
| Switch/package inputs | Docker rebuilt NRO, background ExeFS and OVL; package checks verified versions, corresponding AArch64 ELFs and third-party sources/licenses |
| Script checks | ShellCheck, Python compilation and Git whitespace checks passed; shell/Python checks are now included in CI |

Local evidence is retained in `build/validation/review-host-tests.log`,
`build/validation/review-switch-build.log`, `build/validation/review-ui/`,
`build/validation/audio-review/`, `build/validation/airplay-review/` and
`build/validation/backend-audit/`. Cppcheck output and its limitations are
described in the review; it was not a zero-warning run. The Switch toolchain
also emits existing dependency warnings.

This pass did not repeat the real iPhone session or establish physical-console
background playback. The current user profile was not used as a fixture.
