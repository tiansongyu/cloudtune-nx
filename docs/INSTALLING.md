# Installing Netease Switch

Netease Switch is unofficial homebrew for a Nintendo Switch running
Atmosphère. The full app, background player and overlay are distributed
together so their IPC protocol versions match.

## SD-card layout

Extract the release archive ending in `-sd.zip` directly to the root of your
SD card. The archive contains:

```text
sd:/
├── switch/
│   ├── NeteaseSwitch/
│   │   └── NeteaseSwitch.nro
│   └── .overlays/
│       └── NeteaseSwitch.ovl
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
installation mode: the service remains idle until playback is requested,
so starting the service does not automatically play music.

## First launch

1. Install a compatible Tesla/Ultrahand overlay loader separately if you
   intend to use the in-game overlay.
2. Copy the SD archive directories to your SD card. Keep the folder
   hierarchy and allow the files from this release to replace older files.
3. Reboot into Atmosphère to start the background player.
4. Launch **Netease Switch** from Homebrew Menu. Full application mode is
   recommended for the GUI and networking memory budget.
5. Sign in through the app and verify your account and library.
6. Start playback. Open the overlay through your loader's configured
   shortcut to control the background player.

The release contains no accounts or session cookies. Your session and player
state are stored under `sd:/config/netease-switch/`. Treat that directory as
private when sharing logs or backing up the SD card.

The CA bundle enables certificate validation for the background process.
Keep it installed even though the NRO also embeds its own bundle.

## Updates

Replace the NRO, overlay and system module from the **same** release, then
reboot. Preserve `sd:/config/netease-switch/` to retain account/player state;
replace only its `cacert.pem` with the supplied updated bundle.

Verify the downloaded archives using the release's `-SHA256SUMS` file.
The unpacked release folder also includes `manifest.json`, which records
individual binary hashes and the source revision.

## Disable or uninstall

To stop automatic background startup, remove
`sd:/atmosphere/contents/42000000004E434D/flags/boot2.flag` and reboot.
The NRO will still attempt to launch the installed service on demand.
If the service cannot start, the app labels its fallback as foreground
playback; that mode ends when the NRO exits.

To uninstall, remove the NRO folder, the `NeteaseSwitch.ovl` file and the
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

See [VALIDATION.md](VALIDATION.md) for checks actually executed and remaining
hardware checks. Emulator startup alone is not a hardware BGM certification.
