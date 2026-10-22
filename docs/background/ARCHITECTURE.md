# Background playback

CloudTune NX has three independently loaded Horizon programs:

| Program | Purpose | Lifetime |
|---|---|---|
| `CloudTune.nro` | Login, discovery, library, playlist management, lyrics and full player | Until the user exits the application |
| `cloudtune-background.nsp` | HTTPS transfer, SD cache, MP3/AAC/FLAC decoding, AirPlay reception, audio output, queue | Atmosphère system module; survives application exit |
| `CloudTune.ovl` | Small Tesla player and controls over a running game | While opened through the overlay loader |

The background service uses program ID `42000000004E434D` and service name
`ncm:bg`. Its program ID must not be shared with another installed system module.
These identifiers and `config/netease-switch/` deliberately remain stable
across the CloudTune NX rename to preserve installed services and sessions.
Since v0.2.9 the playback heap is no longer embedded in BSS: loading the
module needs about 3.4 MiB instead of 19.1 MiB. v0.2.10 additionally reserves
only the heap address at entry (`svcSetHeapSize(0)`). Before HOME is recorded
as created, startup uses allocation-free SM/FS IPC and flushes diagnostics
through a native `FsFile`, without mounting fsdev, initializing network/time,
creating workers or running C++ global constructors.

Startup polls Atmosphere `pm:info` command 65001 for HOME program
`0100000000001000`. The loader retains that record after HOME is replaced,
so on-demand launch while a game is running also works. This is a process
creation boundary, not proof that HOME has finished rendering. Polling stops
after about 120 seconds; query errors/timeouts release the process without starting
the runtime. Do not use `pm:shell` GetBootFinishedEventHandle: Atmosphere 1.9.4
aborts that command outside recovery boot. Do not call NotifyBootFinished.

Since v0.2.11, after the HOME boundary startup requires 20 consecutive 250 ms
observations with sufficient memory, 16 threads, 16 events, four transfer
memories and 16 sessions free. v0.2.14 selects one of two memory budgets before
any playback heap allocation:

| Mode | Free memory required at startup | Playback heap cap | System policy margin |
|---|---|---|---|
| Normal | 10.25 MiB | 16 MiB, allocated on demand | 8 MiB |
| Compact | 5 MiB | 2 MiB, hard cap | 3 MiB |

Normal mode includes the measured 2 MiB initial heap plus 256 KiB allowance.
Compact mode budgets its entire maximum heap at startup. The physical v0.2.13
log had 5.18–5.41 MiB free with the overlay loader enabled; the old 10.25 MiB
threshold could never pass. The smaller margin is an explicit policy tradeoff,
not a Nintendo guarantee or a reservation against other processes.

A shortage or a mode change resets the five-second observation window, including
changes detected after the final sleep. The selected mode is frozen before
newlib/worker initialization and cannot be changed while allocations are live.
The resource wait ends after about 60 seconds. Failed queries/timeouts exit
before C++ global constructors, releasing acquired platform resources. The log
records `memory-policy`, `memory-reserve-bytes` and `heap-limit-bytes`.

Newlib's first allocation commits 2 MiB. Normal mode may grow in 2 MiB steps;
compact mode rejects further growth even if another process later frees memory.
Every physical growth checks the selected system margin. Failed checks/kernel
requests return `ENOMEM` without changing live allocations. Starting workers,
download threads, listeners or audio output also checks the selected memory
margin and the unchanged thread/event/transfer/session margins. Reads/checks
are not atomic with other processes' allocations and cannot reserve resources.
Foreground players do not use this system-module policy.

When newlib lowers its break, the allocator attempts to release whole 2 MiB
tail chunks. Horizon validates that those pages have normal writable memory
state and no locked/alias-mapped attributes; a rejected shrink retains the
physical pages. Closing tracks or disabling AirPlay requests `malloc_trim(0)`.
Fragmented heaps can still retain pages: this is not a promise to return to
the initial footprint. The emergency heap is now 16 KiB and never runs the
runtime. Static sections are still loaded before our code can check quotas;
insufficient resources at process creation can still make Atmosphere boot2
abort. Stage snapshots record all five shared quotas in the startup log.

It opens three BSD sessions and opens `audout:u`
only when there is a playable track. Audio is 48 kHz, signed 16-bit stereo;
other source rates are linearly resampled, mono is expanded and multichannel
input is mixed to stereo. High-resolution source support does not imply
bit-perfect output; see [audio formats](../AUDIO_FORMATS.md).

The application first attempts the background IPC connection. When the service
is unavailable it can run the same `netease::playback::Runtime` in its own
process. This foreground fallback stops when the application exits.
The runtime owns both the music engine and the independent AirPlay receiver.
Connection probing accepts libnx's Atmosphère flag or a successful nonzero
read-only Mesosphere kernel-version query (`svcGetInfo`, type 65000, sub-ID 0).
Legacy overlay loaders can omit HBABI's optional Atmosphere hint. Neither a
failed query nor zero output is accepted. After this check the client uses
Atmosphère's TIPC `HasService` command (65100). Acquisition uses a private SM
session and standard TLS/CMIF on one dedicated worker. This follows the
ordinary libnx session path used by TriPlayer while keeping bounded caller
waits: handshake/status/control requests allow 150 ms each, and the acknowledged
account-clear barrier allows 5.5 s. The worker has a 16 KiB stack and a fixed
256-byte mailbox; no SVC 0x23 or user-buffer mapping is needed. PM process-ID
lookup precedes launch, so an existing process behind the boot gate is not
started again. Unknown PM state prevents a second local player. Foreground
and overlay clients reconnect at most once every two seconds after startup.
Stock/emulated Horizon that rejects the kernel query receives no Atmosphere
SM extension commands in production. Once both the service and PM process
lookup explicitly report absence, a waiting frontend releases stale background
ownership and starts its local runtime; unknown PM state still prevents this.

Only one request can be pending. A timeout logically disconnects the client
without sending a synchronous CMIF Close and never automatically replays a
command whose outcome is unknown. The worker retains its session handle and
memory until its system call returns, preventing handle reuse while the
worker is descheduled. Late transferred handles are closed before reuse. A permanently
blocked worker causes final shutdown to exit the process rather than return
to a loader which might recycle its stack. Otherwise shutdown joins the idle
worker and releases its resources. This does not restart a hung service.
The reply parser accepts type 0 and the existing server's type-4 framing,
while checking the CMIF header, data bounds and transferred handles. A valid
service error remains distinguishable from a transport failure, allowing
supported-command probes on the same session.

## Protocol and queue

`include/netease/playback_protocol.h` is the shared versioned C ABI. Status is
exactly 216 bytes and fits a CMIF inline reply. It includes track ID, state,
position, duration, volume, queue position, repeat/shuffle, error code and UTF-8
track text. The IPC request handler queues bounded control messages; it does
not download music or decode audio on the service thread. Account changes use
the acknowledged `ClearQueue` command: its bounded wait ensures old music and
pending loads are cleared before publishing a different account session.
The engine reserves space for 32 pending commands and one final AirPlay End
event at construction. Consecutive End events coalesce, so command producers
do not allocate during playback. The service dispatch boundary converts
allocation and other exceptions into error replies.

`ncmWriteQueueFile()` publishes `queue.nsq` using a temporary file, flush, sync
and rename. POSIX replacement is atomic. Horizon rejects an existing rename
destination with `EEXIST`, so that case moves the old queue to `.bak`, publishes
the new queue, and removes the backup after success. Failed publication restores
the old queue; if restoration also fails, the backup remains readable by the
engine when the primary is absent. This also covers a crash between renames.
A queue has a 16-byte header followed by fixed-size
1,424-byte entries. The engine reads these records individually and retains
only playback fields and actual string lengths; the wire/file ABI is unchanged.
The maximum queue is 512 entries, bounding the persistent module's memory. Larger remote collections must be presented in windows by the
application. Each entry can contain a resolved HTTPS URL or only a song ID.
The background process resolves missing/expired URLs using the same API client
and a session snapshot captured when it loads the queue. It reads the active
session from the committed `accounts.json`, recovering its `.bak` when needed,
then imports and exports the jar through libcurl to normalize cookie syntax.
Both API authentication and cache identity use that same normalized snapshot.
Next/previous, repeat, and resuming music after AirPlay preserve it; an
acknowledged `ClearQueue` discards it before an account change. Only a newly
loaded queue captures another session. The background reader never repairs
the compatibility mirror or persists cookies.

`cookies.txt` is a legacy fallback only when neither `accounts.json` nor its
backup exists and no `accounts.json.pending` marker or marker backup exists.
A committed signed-out record always selects an anonymous session, regardless
of stale mirror contents. Empty or damaged account archives reject new queue
loads, including cold play from the overlay. An existing loaded queue and
AirPlay are not stopped merely because a file later becomes unreadable.

First-time account publication writes a non-sensitive `.pending` marker before
changing the legacy mirror. Successful commit or verified restoration of the
old mirror removes it. If both publication and rollback fail, the marker
prevents restart or a retry on the same store object from accepting the staged
mirror as a legacy session. A valid committed archive takes precedence over a
residual marker. Recovery must restore the committed archive, or restore the
previous legacy session before removing the marker; see
[account recovery](../ACCOUNTS.md).

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

Remote downloads use a single `w+b` stream shared by the downloader and decoder.
A mutex protects each seek/read/write/flush operation; the read cursor and
download offset are independent. Waiting for network bytes happens outside
the mutex. The worker joins the downloader before closing or reopening the
stream, including signed-URL retries. This avoids conflicting SD opens and
re-truncation under an active reader. Completed/local media uses a read-only
stream. Cache I/O errors stop the transfer and are reported separately from
network and decoder errors, with `errno` and, on Switch, the native FS Result.
The bounded `cache-errors.log` is written even when periodic audio diagnostics
are disabled, if the config directory remains writable.

The downloader requires HTTPS, verifies the CA chain and hostname, restricts
redirects to HTTPS, limits redirects, enforces connection/stall/overall
timeouts, and caps each response at 512 MiB. A `.complete` marker is written
only after a successful complete response. Incomplete downloads are never
mistaken for an offline cache hit. Before a new transfer, oldest cache files
are evicted to retain at most 512 MiB of other tracks, plus at most 512 MiB
for the current download (approximately 1 GiB total).
The marker is published before the transfer's done flag. Decoder rejection
joins the downloader before invalidating the marker, so cancellation cannot
recreate it afterward. A completed cache rejected while opening the decoder
is invalidated and downloaded once more; failures are bounded, and explicit
local files are never invalidated. This does not redownload a stream that was
opened successfully and only later detected as truncated.
Cache filenames include the song ID and a digest of the loaded queue's
authenticated session identity and requested quality. Loading a queue after
login or changing quality therefore cannot silently reuse an anonymous preview
or a lower-quality download. Session tokens never appear in cache filenames.
Saved settings and account archives recover their `.bak` snapshot if read
during a Horizon replacement operation.

The shared decoder selects MP3, FLAC or AAC from actual content, independently
of URL suffix and service format labels. AAC supports ADTS and M4A; FLAC also
supports Ogg and ID3-prefixed streams. Standard, higher and exhigh qualities
use the legacy endpoint; higher source levels use `songs_url_v1`.
`settings.json` supplies `proxy` and the requested `quality`; account rights
and the track determine the returned source. New caches use `.audio`, while
complete legacy `.mp3` caches remain readable. The certificate bundle is
`config/netease-switch/cacert.pem`.

MP3/FLAC decoder allocations share a per-decoder 4 MiB aggregate budget,
including allocation headers. AAC/ALAC configure FFmpeg's process-wide
single-allocation ceiling once at 2 MiB. AAC also limits probing, stream count
and generic index storage; unusually large container metadata can be rejected.
M4A with complete AAC header parameters skips redundant stream-info probing;
headerless ADTS retains probing. FFmpeg allocation failures are reported as
resource limits instead of damaged audio. Completed download threads are joined
before decoder allocation. If opening a decoder runs out of memory while a
download worker still exists, the engine completes that same download, joins
the worker and retries decoding once. The buffering wait remains cancellable
by stop/next/shutdown; persistent failure is reported without deleting a valid
cache. Normal streaming continues when memory permits.
These limits do not change the requested music quality or sample-rate support,
and the FFmpeg ceiling is not a total-process memory limit. Cache eviction
keeps only 256 candidates at a time. A C++ allocation failure releases the
active track and queue file, reports an error and leaves the worker available
for another selection rather than terminating it. The selected 2/16 MiB service heap cap can reject demanding files or queues;
real peak resident memory and game coexistence still require hardware measurements.

Each track owns a fresh audio output session. Seek discards the old session and
opens a new one, so queued DMA buffer tags cannot cross a seek or track change.
Pause releases the output session and retains the audible position; resume seeks
the decoder to that position and opens a new session. Repeated play/pause commands
are idempotent. Failed output calls, five seconds without released buffers, or
a gap of more than two seconds between playback iterations start recovery.
Known decoding/seeking work resets that gap timer to avoid repeated recovery
on a slow seek. Recovery recreates the output session and restores the saved
audible position. At most three reopen attempts are allowed until output has
worked for two seconds; persistent failures pause the same track with a retry
message instead of skipping songs. User playback controls can retry or cancel
recovery. AirPlay drops buffered/live PCM during recovery and resumes at the
live edge. These are recovery mechanisms, not evidence that every firmware's
sleep or output-route transition has been tested.
Final queued audio at EOF has its own five-second drain watchdog even when
some output slots are free. Volume, repeat and shuffle changes do not reset
stall timers. Reopening and stopping the HOS audio service still use libnx's
synchronous system-service calls; recovery assumes those calls return.

For diagnostics, set `"diagnostics": true` in `settings.json` before starting
the engine. `audio.log` records sample rate, produced frames, queued duration,
position and output result codes, with a 256 KiB rotation limit. It excludes
URLs and authentication values. Remove the setting after collecting a report.

The system module's `ipc.log` records service lifecycle events and changed IPC
errors. Successful polling replies are omitted, repeated identical errors are
suppressed, and writes stop at 64 KiB until the service restarts. Each start
replaces that diagnostic file. Logging failures disable the log without changing
request handling or session cleanup.

## Installation layout

```text
SD card/
├── atmosphere/contents/42000000004E434D/
│   ├── exefs.nsp                 # renamed cloudtune-background.nsp
│   └── toolbox.json             # boot2.flag is opt-in; not included since v0.2.7
├── switch/.overlays/CloudTune.ovl
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
- [libnx kernel calls](https://github.com/switchbrew/libnx/blob/v4.12.0/nx/include/switch/kernel/svc.h):
  ordinary TLS SendSyncRequest, thread and condition-variable primitives.
- [Atmosphère server session](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_server_session.cpp):
  request completion and client-close behavior. Closing a client does not
  immediately cancel a request already received by the server.
- [dr_mp3](https://github.com/mackron/dr_libs/blob/master/dr_mp3.h):
  streaming read callbacks must fill the requested size unless at EOF.
- [libtesla](https://github.com/WerWolv/libtesla): standalone overlay lifecycle.
- Local Switify phase2 source at commit
  `e4b86610598dcf511b3682be0e047855e7445ac6`: process separation, CMIF service
  loop, background service metadata, and overlay architecture.

### TriPlayer communication review (2026-10-22)

Reviewed the local TriPlayer checkout at commit
`737adfdfcea89924286b1cd13ec47a9ebaffda96` (MIT).
`Common/source/ipc/TriPlayer.cpp` uses HasService, normal libnx sessions and
CMIF commands; `Application/source/Sysmodule.cpp` serializes requests and
performs a version handshake. `Application/source/utils/NX.cpp` checks the
process ID before launch. `Sysmodule/source/ipc/Server.cpp` handles ordinary
sessions, while `Sysmodule/source/Service.cpp` separates audio work from IPC.
CloudTune adopts those communication patterns with independently written
code, retaining bounded waits, its existing protocol and single runtime owner.
TriPlayer's fixed heap does not solve a shared system-memory shortage and was
not copied. Its playback/network implementation was not substituted.
