# Code review — 2026-10-20

This review follows the QR accounts and AAC/FLAC update. It covers the
application, API/model/account storage, shared playback engine, AirPlay receiver,
background IPC, overlay, tests, build scripts, packaging and GitHub Actions.
Vendored dependencies were checked at integration boundaries, including pinned
versions and license/source packaging; this is not a line-by-line audit of all
third-party code.

## Findings and changes

| Area | Finding | Change and regression coverage |
| --- | --- | --- |
| API transport | Following an authenticated POST redirect could forward the custom session header and encrypted request body to another host. | Fixed API POSTs reject redirects. A loopback TLS proxy checks rejection; public artwork GET redirects still work. |
| API cookies and errors | An unrelated domain's identically named cookie could supply the custom authentication header. JSON parse diagnostics could quote private response contents. | Select only the NetEase cookie domain and return a generic parse error; synthetic transport tests cover both cases. |
| Account recovery | Startup and navigation hid an unreadable account registry error; its cookie mirror could still be used. Empty archives were mistaken for legacy installations. | Keep the recovery error visible, reject new GUI playback/QR changes and validate the archive before the engine reads a saved queue, including cold overlay playback. Preserve the archive and mirror; do not forcibly stop already-loaded music or AirPlay. |
| Numeric input | Malformed API status codes, pagination and IDs could be partially accepted or narrowed; extreme LRC timestamps could overflow signed arithmetic. | Parse decimal fields strictly, clamp media counters and check timestamp arithmetic before addition/multiplication. Extreme-value tests include a reproduced UBSan failure. |
| FLAC seeking | Legal FLAC streams with an unknown total frame count could seek back to zero while reporting the requested position. | Rewind and decode/discard bounded blocks for exact seeking, preserving stream cancellation. PCM and engine tests cover unknown length, pause/seek, EOF and track advance. |
| Cache recovery | A completion marker without its data file prevented a replacement download. | Require both files for a cache hit. The generated HTTPS test verifies recovery from an orphan marker. |
| Decoder offsets | Malformed seek offsets could overflow before the range check. | Validate bounds before addition. Instrumented decoding and integration checks cover the affected path. |
| Background IPC | CMIF alignment padding was counted as input data; descriptor counts could point outside the TLS buffer, and rejected transferred handles were not released. | Validate the inline request layout, subtract padding and close rejected handles. Guard-page host tests exercise the actual parser; the Switch compiler checks the libnx integration. |
| Playback status | A mismatched protocol response or unterminated fixed-size field could reach the UI. | Validate the version, clear failed results and terminate strings. Client tests cover each boundary. |
| UI and shutdown | A collection-header hit region swallowed the top of the settings account row. Long UTF-8 titles could be cut inside a character. Exceptional shutdown could destroy the player before account jobs stopped. | Restrict the hit region, truncate at UTF-8 boundaries and order object destruction safely. Keep curl cleanup after handle/worker destruction and report startup exceptions. |
| Maintenance | Architecture notes still described MP3-only playback; shell/Python syntax checks were absent from CI. | Update the current architecture and add ShellCheck/Python checks to the host job. |

## Verification

Detailed results and local evidence locations are recorded in
[VALIDATION.md](VALIDATION.md). New regression cases are part of the existing
CMake/CTest suite and run in GitHub Actions. Tests use generated audio,
synthetic accounts and loopback servers; no real account archive, login cookie,
generated TLS private key or runtime profile is included in the repository or package.

Static analysis was supplementary. Cppcheck traversed the first-party host
compilation database, but reported dependency preprocessing limitations and
upstream codec/font advisories. The font warning was checked against its
initialization path. This review does not claim a warning-free Cppcheck run or
complete analysis of every platform/dependency path.

## Remaining validation boundaries

- Native IPC tests model libnx wire layouts; they do not emulate the Horizon
  kernel. Service/overlay behavior while a game runs still requires a physical
  Switch with Atmosphère.
- Unknown-length FLAC and exact AAC seeking decode forward from the start, so
  distant seeks can take longer than indexed seeks.
- A signed-URL API request can still wait for its network timeout. If the
  account-clear acknowledgement does not arrive within five seconds, switching
  fails safely and can be retried; account credentials are not switched early.
- Output remains stereo S16/48 kHz with the current linear resampler.
  High-resolution decoding is not a claim of bit-perfect output.
- Existing real iPhone/Eden and generated NRO playback evidence keeps the scope
  described in the validation report. This review does not replace physical
  console acceptance or long-duration network testing.

## Second review — account failures and playback boundaries

The second pass added fault injection, independent cross-review and sanitizer
coverage. It found additional defects despite the first pass's green tests.
The new cases use synthetic sessions, generated music and local servers.

| Area | Reproduced defect | Fix and regression evidence |
| --- | --- | --- |
| Queue identity | An interrupted account save could leave the cookie mirror newer than the committed archive. Reloading it between songs could change the API identity and cache owner within one queue. | Capture the active canonical session when loading the queue, normalize it through libcurl, and retain it until the queue is cleared or replaced. A TLS fixture independently decrypts EAPI requests and verifies Alice/Alice/Bob identity and cache ownership across playback, next track and reload. The player never rewrites account files. |
| Authentication fields | Expired cookies and cookies scoped to an unrelated path could enter custom API authentication fields. | Select unexpired root-path cookies from the official domain using the normalized cookie jar. Transport tests check the actual header and decrypted request body. libcurl's exported list can contain expired cookies; see [CURLINFO_COOKIELIST](https://curl.se/libcurl/c/CURLINFO_COOKIELIST.html). |
| First account publication | If both canonical publication and cookie rollback failed, an uncommitted mirror could be treated as a legacy login. Retrying on the same store could also discard the recovery guard. | Persist a non-secret initialization marker before changing the mirror. Refuse legacy fallback and retries until a trusted session is restored. FIFO/directory fault injection verifies both failures, the retry guard and preservation of file contents. |
| Shutdown | A pending account job could still publish a switch/removal after shutdown started. | Include the stopping state in the account-generation check. A deterministic blocked-playback-barrier test verifies cancellation. |
| MP3 seeking and truncation | Encoder-delay handling made forward seeks return the wrong samples. Early EOF with a reliable declared frame count was not marked damaged. | Seek relative to the audible cursor by bounded decoding, rewind when necessary, and compare short reads with the declared length. Sample-exact forward/backward/EOF tests and truncated Xing streams reproduce the original defects. |
| AirPlay handoff | Internal events 100–102 were cast into a public enum whose representable range ended at 31. UBSan rejected the handoff. | Store internal event numbers as integers, retain the public ABI, and validate incoming raw IPC command numbers before converting them to the enum. Both MP3 and FLAC handoff tests now run under UBSan. |
| AirPlay configuration/status | Fractional or oversized ports could truncate/wrap; failed IPC could leave a partially written AirPlay status. | Require an integer port in range and clear failed status results. Boundary, persistence-failure and partial-response tests cover both changes. |

Additional checks exercise 1,219 deterministic codec/PCM cases, fragmented and
pipelined RTSP, malformed metadata and UDP packets, receiver restarts,
concurrent runtime controls and guarded IPC layouts. These are bounded
regressions, not exhaustive fuzzing. UBSan is now part of the GitHub Actions host
job; separate ASan/UBSan/leak runs cover accounts/API and the audio decoder with
instrumented pinned FFmpeg. See [the validation report](VALIDATION.md) for results
and evidence locations.

The initialization marker prevents unsafe recovery after observed write
failures; actual SD-card power-loss behavior still requires hardware testing.
MP3 without a reliable declared frame count cannot distinguish ordinary EOF
from every truncated stream. Exact MP3 seeks may decode intervening samples and
take longer on distant targets.
