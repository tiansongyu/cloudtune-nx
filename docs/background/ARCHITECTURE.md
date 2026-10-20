# Background playback

NetEase Switch has three independently loaded Horizon programs:

| Program | Purpose | Lifetime |
|---|---|---|
| `NeteaseSwitch.nro` | Login, discovery, library, playlist management, lyrics and full player | Until the user exits the application |
| `netease-background.nsp` | HTTPS transfer, SD cache, MP3 decoding, audio output, queue | Atmosphère system module; survives application exit |
| `netease-overlay.ovl` | Small Tesla player and controls over a running game | While opened through the overlay loader |

The background service uses program ID `42000000004E434D` and service name
`ncm:bg`. Its program ID must not be shared with another installed system module.
It allocates a fixed 12 MiB heap, opens two BSD sessions, and opens `audout:u`
only when there is a playable track. Audio is 48 kHz, signed 16-bit stereo;
44.1 kHz and mono MP3 input is linearly resampled and expanded to stereo.

The application first attempts the background IPC connection. When the service
is unavailable it can run the same `netease::playback::Engine` in its own
process. This foreground fallback stops when the application exits.
Connection probing first checks libnx's Atmosphère flag, then uses
Atmosphère's nonblocking TIPC `HasService` command (65100). It never requests a
missing custom service from SM, because that request can wait indefinitely.
Stock/emulated Horizon receives no Atmosphère extension commands.

## Protocol and queue

`include/netease/playback_protocol.h` is the shared versioned C ABI. Status is
exactly 216 bytes and fits a CMIF inline reply. It includes track ID, state,
position, duration, volume, queue position, repeat/shuffle, error code and UTF-8
track text. The IPC request handler only queues bounded control messages; it
does not download music or decode audio on the service thread.

`ncmWriteQueueFile()` publishes `queue.nsq` using a temporary file, flush, sync
and rename. POSIX replacement is atomic. Horizon rejects an existing rename
destination with `EEXIST`, so that case moves the old queue to `.bak`, publishes
the new queue, and removes the backup after success. Failed publication restores
the old queue; if restoration also fails, the backup remains readable by the
engine when the primary is absent. This also covers a crash between renames.
A queue has a 16-byte header followed by fixed-size
1,424-byte entries. The maximum queue is 512 entries, bounding the persistent
module's memory. Larger remote collections must be presented in windows by the
application. Each entry can contain a resolved HTTPS URL or only a song ID.
The background process resolves missing/expired URLs using the same API client
and the application's cookie jar. Only the application persists cookies; the
background reader reloads them without overwriting a newer login or logout.

Accepted controls: play, pause, toggle, next, previous, stop, millisecond seek,
volume from 0 to 1,000, shuffle, and repeat off/one/all. Previous restarts the
current track after three elapsed seconds. Unavailable tracks are skipped with
a bounded failure count. A malformed queue is rejected and never interpreted
as audio.

## Streaming and cache

The download worker writes a growing file under `config/netease-switch/cache/`.
The decoder begins after 32 KiB and reads only bytes the downloader has flushed.
No complete song is retained in RAM. Backward seek reads the existing cache;
forward seek can wait for the required bytes to arrive. Track changes and stop
interrupt the transfer and decoder wait.

The downloader requires HTTPS, verifies the CA chain and hostname, restricts
redirects to HTTPS, limits redirects, enforces connection/stall/overall
timeouts, and caps each response at 128 MiB. A `.complete` marker is written
only after a successful complete response. Incomplete downloads are never
mistaken for an offline cache hit. Before a new transfer, oldest cache files
are evicted to reserve room within an approximately 512 MiB cache budget.
Cache filenames include the song ID and a digest of the active login session
and MP3 quality. Logging in or changing quality therefore cannot silently reuse
an anonymous preview or a lower-quality download. Session tokens never appear
in cache filenames. Saved settings and cookies recover their `.bak` snapshot
if read during a Horizon replacement operation.

The current decoder supports MP3. Standard, higher and exhigh qualities use
the API's legacy MP3 endpoint; FLAC/AAC/lossless playback is not implemented.
Service-provided format information is checked when resolving a URL.
`settings.json` supplies `proxy` and MP3-compatible `quality`. The certificate
bundle is `config/netease-switch/cacert.pem`.

Each track owns a fresh audio output session. Seek discards the old session and
opens a new one, so queued DMA buffer tags cannot cross a seek or track change.
Pause releases the output session and retains the audible position; resume seeks
the decoder to that position and opens a new session. Repeated play/pause commands
are idempotent. Failed output calls or five seconds without a
released output buffer become a playback error rather than a permanently
advancing animation with no working audio.

For diagnostics, set `"diagnostics": true` in `settings.json` before starting
the engine. `audio.log` records sample rate, produced frames, queued duration,
position and output result codes, with a 256 KiB rotation limit. It excludes
URLs and authentication values. Remove the setting after collecting a report.

## Installation layout

```text
SD card/
├── atmosphere/contents/42000000004E434D/
│   ├── exefs.nsp                 # renamed netease-background.nsp
│   ├── toolbox.json
│   └── flags/boot2.flag          # empty file enables boot startup
├── switch/.overlays/netease-overlay.ovl
└── config/netease-switch/
    └── cacert.pem
```

Tesla or a compatible overlay loader is required to display the `.ovl`.
The overlay itself provides an explicit start/reconnect action and controls
the background process. It does not host playback.

## Validation

`background/tests/playback_smoke.cpp` exercises the actual decoder and SDL
audio sink with generated MP3 data. The test checks queue serialization,
invalid limits, playback position, pause, seek, volume, repeat, shuffle,
previous/next, automatic track advance, final queue stop and malformed input.
It also checks repeated play/pause commands and that repeat-one skips an
unavailable track instead of retrying it forever.
`https_fixture.py` serves the same generated fixture through a throttled TLS
connection to exercise progressive download rather than a preloaded file.
The HTTPS test also changes quality and session state and verifies that each
receives its own completed cache file. An optional second MP3 input exercises
automatic transitions between different sample rates.
`queue_replace.cpp` wraps `rename` to emulate Horizon's `EEXIST` behavior and
inject publication/restoration failures. It checks repeated queue replacement,
rollback, preserved recovery backups, subsequent publication and invalid input.

Desktop tests use `SDL_AUDIODRIVER=dummy` for deterministic headless execution.
They verify decoder/output control behavior, not audible sound quality.
Both the system module and overlay have been cross-compiled with devkitA64
r29.2 and libnx 4.12.0. Host testing and cross-compilation do not establish
real-hardware BGM correctness; game coexistence, suspend/resume, Bluetooth
output, dock changes and low-memory conditions require Switch testing.

On 2026-10-20, `ncm-live-playback` also resolved song ID `347230` through the
real API with an anonymous session and played its authorized MP3 preview through
the complete engine. The returned/decoded duration was 45,000 ms; the status
reported `Playing preview / 试听` and the audio clock advanced beyond five
seconds. No logged-in session, full-track entitlement or actual Switch audio
output was tested by that check.

The same day's Eden NRO tests exercised a 44.1 kHz preview followed automatically
by a 48 kHz track. Isolated capture of the emulator's audio stream contained
changing PCM samples, and the second track's position kept advancing. A stale
output-buffer failure on track change and pause/resume was reproduced and fixed
by replacing audio output sessions at those boundaries. Pause froze the vinyl
image; resume and an active seek continued the same track with changing audio
samples. These are emulator foreground results, not a claim that Atmosphère
background mixing has been validated on a Switch.

## Primary references

Retrieved 2026-10-20:

- [libnx audout reference](https://switchbrew.github.io/libnx/audout_8h.html):
  audio output functions, device format and aligned buffer contract. The
  upstream declaration is also retained locally as `libnx-audout.h`.
- [libnx audout implementation](https://github.com/switchbrew/libnx/blob/master/nx/source/services/audout.c):
  released buffers are returned one tag per API call; this implementation
  closes the output session before reusing buffer memory after a seek.
- [libnx service manager](https://github.com/switchbrew/libnx/blob/master/nx/source/services/sm.c):
  service registration and CMIF client initialization.
- [Atmosphère SM user interface](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/include/stratosphere/sm/impl/sm_user_interface.hpp):
  nonblocking service availability command 65100.
- [dr_mp3](https://github.com/mackron/dr_libs/blob/master/dr_mp3.h):
  streaming read callbacks must fill the requested size unless at EOF.
- [libtesla](https://github.com/WerWolv/libtesla): standalone overlay lifecycle.
- Local Switify phase2 source at commit
  `e4b86610598dcf511b3682be0e047855e7445ac6`: process separation, CMIF service
  loop, background service metadata, and overlay architecture.
