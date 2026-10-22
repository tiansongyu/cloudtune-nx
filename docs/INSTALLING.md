# Installing CloudTune NX

CloudTune NX is unofficial homebrew for a Nintendo Switch running
Atmosphère. The full app, background player and overlay are distributed
together so their IPC protocol versions match.
The canonical repository is
[tiansongyu/cloudtune-nx](https://github.com/tiansongyu/cloudtune-nx).

## SD-card layout

Download `CloudTuneNX-<version>-sd.zip` and `SHA256SUMS` from the same
[GitHub Release](https://github.com/tiansongyu/cloudtune-nx/releases).
For v0.2.8, save both files in one directory and verify:

```sh
sha256sum --check --ignore-missing SHA256SUMS
```

Confirm that `CloudTuneNX-0.2.8-sd.zip` reports `OK`. The `--ignore-missing`
option permits downloading only the SD archive; it does not ignore a checksum
mismatch in a file you downloaded. Windows users can compare
`Get-FileHash .\CloudTuneNX-0.2.8-sd.zip -Algorithm SHA256` with the matching
line in `SHA256SUMS`.

Extract **only the SD archive** directly to the root of your SD card.
The symbols and documentation archives remain on your computer; GitHub's
automatic Source code archives are not installation packages.
Local `./scripts/package.sh` builds additionally support timestamped
`CloudTuneNX-<version>-<timestamp>-sd.zip` candidates with the same SD layout:

```text
sd:/
├── switch/
│   ├── CloudTune/
│   │   └── CloudTune.nro
│   └── .overlays/
│       └── CloudTune.ovl
├── atmosphere/
│   └── contents/
│       └── 42000000004E434D/
│           ├── exefs.nsp
│           └── toolbox.json
└── config/
    └── netease-switch/
        └── cacert.pem
```

`exefs.nsp` is the Atmosphère background system module. It is **not** a
game-installation NSP and must not be installed with a title installer.
Since v0.2.7, the package does **not** include `boot2.flag`. CloudTune launches
the installed service on demand when you open the NRO. This keeps a custom
module out of the system boot sequence. Starting the service does not play the saved
NetEase queue. Its independent AirPlay 1 receiver starts when enabled; the
first-use default is enabled, and later starts follow your saved preference.

## Connection and low-memory troubleshooting (v0.2.15)

Update all three matching components: the NRO, `exefs.nsp` and `CloudTune.ovl`.

If the overlay connects but the main application reports `0x10801`, update
the **NRO as well as the NSP**. v0.2.15 fixes a second `sm:` session allocation
that exceeds application mode's one-session creation quota. This is distinct
from background RAM exhaustion; closing more system modules cannot change
that application quota. The client now borrows libnx's existing SM connection.
For another failure, preserve `ipc-client-app.log`, `ipc-client-overlay.log`,
`background-startup.log` and `ipc.log` from `config/netease-switch/`. Client logs
contain the failed stage and result without account or music URL data.
Preserve `config/netease-switch/` and any local photo resources. A local
autostart repair bundle includes an empty
`atmosphere/contents/42000000004E434D/flags/boot2.flag`; public downloads keep
autostart opt-in. `boot.flag` is not the Atmosphere boot2 marker.

The foreground and overlay now reuse an existing process and retry a delayed
connection. The GUI distinguishes **后台连接中** from **前台播放**. An absent
service is not necessarily an IPC fault: check the current boot's
`background-startup.log`. `runtime`, `ipc-register` and `ready` should all
report zero. A failed `boot-resource-gate` followed by `startup-exit` means
initialization stopped before registration. v0.2.12 clears the previous
`ipc.log` at startup, so an old success line cannot stand in for this boot.

A physical v0.2.11 log showed only **168 KiB** free; that is insufficient even
for the initial 2 MiB heap. After unused modules were disabled, v0.2.13 still
had only **5.18–5.41 MiB** free with CloudTune and Tesla/Ultrahand enabled. Its
10.25 MiB policy threshold blocked startup before IPC registration.

v0.2.14 can select **compact mode** after five seconds with at least 5 MiB free:
it limits the playback heap to 2 MiB and checks a 3 MiB system margin before
allocation/new work. With at least 10.25 MiB free it retains normal mode (8 MiB
margin, up to 16 MiB heap). The startup log records the selected mode. These are
policy margins, not guaranteed reservations; demanding tracks/queues may exceed
the compact budget and report an allocation error.

Keep the Tesla/Ultrahand loader's `boot2.flag` enabled for in-game controls.
To test with more headroom, fully power off, temporarily rename only unused
third-party modules' `boot2.flag` files, then reboot. Record fresh resource
counters and test frontend/overlay controls. Restore the renamed markers one
at a time, rebooting between trials. Do not assume a module's file size equals
its runtime memory usage. Reducing a compatible loader's heap can free space,
but merely changing Ultrahand settings does not upgrade an old loader.

## First launch

1. Install a compatible Tesla/Ultrahand overlay loader separately if you
   intend to use the in-game overlay.
2. Copy the SD archive directories to your SD card. Keep the folder
   hierarchy and allow the files from this release to replace older files.
3. Reboot into Atmosphère. Standard packages launch on demand. If you have
   deliberately enabled the current service at boot, keep its `boot2.flag`;
   disable it only when using the boot-failure recovery steps below.
4. Hold **R** while launching a game to open Homebrew Menu in full application
   mode, then launch **CloudTune NX**. Use this mode for the GUI and networking
   memory budget.
5. Open **我的账号**, generate a QR code, and scan and confirm it with the
   official NetEase Cloud Music phone app. QR scanning is the only login method.
   Verify your account and library afterward.
6. Start playback. Open the overlay through your loader's configured
   shortcut to control the background player.

The release contains no accounts or session cookies. Your session and player
state are stored under `sd:/config/netease-switch/`. Treat that directory as
private when sharing logs or backing up the SD card.
This original directory name is intentional: CloudTune NX preserves it to
reuse already authorized sessions, queues and settings without a migration.
The IPC service `ncm:bg` and program ID `42000000004E434D` also remain unchanged.

Add, switch or remove saved accounts in **设置 → 账号管理**. Switching first
clears the previous NetEase playback queue; an existing AirPlay session can
continue independently. See [ACCOUNTS.md](ACCOUNTS.md) for session recovery.

For phone audio, connect both devices to the same local network and open
**设置 → AirPlay 1 接收**. Confirm the receiver is ready before selecting it
on the sender. Turn reception off there if you do not want it listening.
With the matching background service, reception remains available after
closing the NRO; foreground fallback reception stops with the app.
See [AIRPLAY.md](AIRPLAY.md) for network requirements and validation limits.

The CA bundle enables certificate validation for the background process.
Keep it installed even though the NRO also embeds its own bundle.

## Recover a console that crashes during boot

The first physical report identified boot2 (`0100000000000008`) aborting on
kernel `0x10801` (`LimitReached`). v0.2.9 removed the 16 MiB static heap, but
the follow-up still showed AM (`0100000000000023`) with `2001-0132`, while
CloudTune's log reached `ready` with a 2 MiB heap. A successful service start
therefore did not establish that the system had enough resources to finish
booting. The exact exhausted resource remains unconfirmed.

v0.2.10 waits for Atmosphere to record HOME's creation before committing the
playback heap or starting networking/workers. The waiting path writes SD
logs without malloc. It preserves automatic startup through `boot2.flag`,
with no manual application launch required. Query failures or an approximately 120-second
timeout stop startup safely. The static module still needs about 3.4 MiB;
this is not a guarantee that every module combination will fit.

Update `exefs.nsp`, not just the NRO, before retesting autostart. Once the
new module is installed, an empty
`atmosphere/contents/42000000004E434D/flags/boot2.flag` opts into boot startup.
The standard public archive still leaves that choice to the user.

1. Power the console off and access its SD card from a computer.
2. Rename `sd:/atmosphere/contents/42000000004E434D/flags/boot2.flag` to
   `boot2.flag.disabled`. This disables only CloudTune automatic startup.
3. Try booting Atmosphère. If it still fails, move the **CloudTune-only** folder
   `42000000004E434D` off the SD card and preserve the crash report. Do not remove
   other modules or `sd:/config/netease-switch/`.
4. Install v0.2.7 or later, keeping that old boot flag disabled, then open the
   NRO in full application mode. Confirm it reports background playback before
   testing playback and the overlay. If the module cannot be launched, the NRO can
   use foreground playback. If a launched process has not published IPC, the NRO
   reports that the background service is not ready to avoid two audio owners.

Copying a new ZIP over an old installation does **not** delete the old
`boot2.flag`. Disable that file explicitly before rebooting. A fresh installation
of v0.2.7 does not enable boot startup. AirPlay/background control becomes
available after the NRO first starts the service in each boot session.

The service replaces `sd:/config/netease-switch/background-startup.log` on
each attempt that can open the log through native FS IPC. v0.2.10 writes its
version, `before-home` / `waiting-home` / `after-home` snapshots and the
`boot-home-menu` result. Every snapshot lists shared resource **used / limit /
free** values for memory (bytes), threads, events, transfer memories and
sessions (counts). `heap-committed-bytes` is hexadecimal bytes, not an error
code; it should be zero while waiting and normally `0x00200000` at ready.
Failures before FS access are emitted to debug output only.

The old ready marker is removed when diagnostic SD access opens and recreated
only after IPC registration. A marker can survive an abnormal exit; verify a
live overlay/NRO connection. If another module crashes after CloudTune is
ready, provide the startup log rather than assuming that `ready` proves the
whole console is healthy. Also provide the error code, Program ID,
firmware/Atmosphere versions and the matching fatal/crash report when present.
Do not share the entire config directory: it contains login state.

## Recover foreground audio cache errors

For `Cannot open audio cache` or repeated format errors at playback start,
update to v0.2.8 or later. It uses one shared cache file handle for progressive
download and decoding, instead of opening the same growing SD file twice.
Replace `switch/CloudTune/CloudTune.nro`; if the background module is installed,
replace the matching full SD package and reboot to unload the old service.
Keep the CloudTune boot flag disabled as described above.

Existing valid caches remain usable. A cached file rejected at decoder open
is automatically invalidated and downloaded once more. There is no need to
delete accounts, settings or the entire config directory. Start a song in
foreground mode, then check pause/resume, seeking and changing tracks.

If playback still reports `SD cache ...: errno=... fs=...`, retain that message
and `sd:/config/netease-switch/cache-errors.log` when present. The log contains
only the failed I/O operation and numeric error codes, and is capped at roughly
64 KiB. Failure to write this log can itself indicate an unwritable SD card.
Report the application version, firmware/Atmosphère versions, launch mode,
free SD space and whether the card is FAT32 or exFAT. A genuine unsupported
source or damaged downloaded audio can still produce a decoder error; this
update does not claim to add support for every format or validate the SD card.

## Updates

When upgrading from v0.2.6 or earlier, disable the old CloudTune
`flags/boot2.flag` as above; the new archive cannot remove it for you.


Replace the NRO, overlay and system module from the **same** release, then
reboot. Preserve `sd:/config/netease-switch/` to retain account/player state;
replace only its `cacert.pem` with the supplied updated bundle.

When upgrading from the pre-rename preview, the new application is
`sd:/switch/CloudTune/CloudTune.nro` and the overlay is
`sd:/switch/.overlays/CloudTune.ovl`. Remove only the superseded
`sd:/switch/NeteaseSwitch/` application folder and the old
`sd:/switch/.overlays/NeteaseSwitch.ovl` or `netease-overlay.ovl` files, if
present, to avoid duplicate menu entries.
Keep `sd:/config/netease-switch/` and update the existing
`42000000004E434D` module in place; do not install a second background module.

Verify downloaded Release assets with their `SHA256SUMS` file.
`CloudTuneNX-<version>-manifest.json` records individual binary hashes and
the source revision; keep it with the matching archives.
Local timestamped packages retain their `<package>-SHA256SUMS` name.
Published version tags and their attachments are not replaced by later builds;
install a newer version when a fix is released.

## Disable or uninstall

Automatic startup is opt-in. After verifying on-demand startup and playback on
your console, you can enable it through a compatible sysmodule manager or create
an empty `flags/boot2.flag` in the CloudTune module folder. Keep it disabled while
diagnosing a boot crash.

To stop automatic background startup, remove
`sd:/atmosphere/contents/42000000004E434D/flags/boot2.flag` and reboot.
The NRO will still attempt to launch the installed service on demand.
If the service cannot start, the app labels its fallback as foreground
playback; that mode ends when the NRO exits.

To uninstall, remove the NRO folder, the `CloudTune.ovl` file and the
`42000000004E434D` system-module folder, then reboot. Remove
`sd:/config/netease-switch/` only if you also want to delete the saved
session and settings.

## Reporting a problem

Include the release version, binary SHA-256, console firmware, Atmosphère
version, overlay loader/version and the exact steps that reproduce the
problem. Remove cookies, tokens and personal account details before sharing
logs.

Keep the matching `-symbols.zip` on your computer. Its ELFs are needed to
interpret crash reports, and do not belong on the SD card.
The app and overlay symbols occupy separate `symbols/app/` and
`symbols/overlay/` directories because both use the basename `CloudTune.elf`;
the service symbols are in `symbols/background/`.

See [VALIDATION.md](VALIDATION.md) for checks actually executed and remaining
hardware checks. Emulator startup alone is not a hardware BGM certification.
