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

## First launch

1. Install a compatible Tesla/Ultrahand overlay loader separately if you
   intend to use the in-game overlay.
2. Copy the SD archive directories to your SD card. Keep the folder
   hierarchy and allow the files from this release to replace older files.
3. Reboot into Atmosphère. For upgrades, remove the old CloudTune `boot2.flag`
   first, as described below.
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

The supplied physical-console fatal report identified Atmosphere boot2
(`0100000000000008`) aborting after the CloudTune launch request returned
kernel `0x10801` (`LimitReached`). v0.2.9 removes the 16 MiB static playback
heap from the loadable image. The image now needs about 3.4 MiB to load; its
kernel heap starts at 2 MiB and grows on demand up to the previous 16 MiB cap.
This addresses the large load-time memory reservation. The report does not
identify which resource was exhausted or enumerate other installed modules;
it is not a guarantee that every module combination will fit.

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

The service writes `sd:/config/netease-switch/background-startup.log` with stage
names and hexadecimal results, replacing it for each attempt that mounts SD.
Failures before SD mounting are emitted to debug output only; absence of this
file does not prove that launch was never attempted. `background-ready.log` is
removed after SD mount and recreated only after IPC registration succeeds.
Report the **error code and Program ID**, firmware/Atmosphère versions and
matching `atmosphere/fatal_errors/` or `atmosphere/crash_reports/` report if present.
Do not share the entire config directory (it contains login state).

v0.2.9 also records `heap-initial`, `heap-last-result`, and
`heap-committed-bytes` (hexadecimal bytes, not an error code). Initial heap
denial is logged and prevents runtime creation. A ready marker should still
be checked against a live connection; it can remain after an abnormal exit.

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
