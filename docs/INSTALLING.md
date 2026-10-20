# Installing CloudTune NX

CloudTune NX is unofficial homebrew for a Nintendo Switch running
Atmosphère. The full app, background player and overlay are distributed
together so their IPC protocol versions match.
The canonical repository is
[tiansongyu/cloudtune-nx](https://github.com/tiansongyu/cloudtune-nx).

## SD-card layout

Download `CloudTuneNX-<version>-sd.zip` and `SHA256SUMS` from the same
[GitHub Release](https://github.com/tiansongyu/cloudtune-nx/releases).
For v0.2.1, save both files in one directory and verify:

```sh
sha256sum --check --ignore-missing SHA256SUMS
```

Confirm that `CloudTuneNX-0.2.1-sd.zip` reports `OK`. The `--ignore-missing`
option permits downloading only the SD archive; it does not ignore a checksum
mismatch in a file you downloaded. Windows users can compare
`Get-FileHash .\CloudTuneNX-0.2.1-sd.zip -Algorithm SHA256` with the matching
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
│           ├── toolbox.json
│           └── flags/
│               └── boot2.flag
└── config/
    └── netease-switch/
        └── cacert.pem
```

`exefs.nsp` is the Atmosphère background system module. It is **not** a
game-installation NSP and must not be installed with a title installer.
`boot2.flag` asks Atmosphère to start it at boot. This is the default
installation mode. Starting the service does not automatically play the saved
NetEase queue. Its independent AirPlay 1 receiver starts when enabled; the
first-use default is enabled, and later starts follow your saved preference.

## First launch

1. Install a compatible Tesla/Ultrahand overlay loader separately if you
   intend to use the in-game overlay.
2. Copy the SD archive directories to your SD card. Keep the folder
   hierarchy and allow the files from this release to replace older files.
3. Reboot into Atmosphère to start the background player.
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

## Updates

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
