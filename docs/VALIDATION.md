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
