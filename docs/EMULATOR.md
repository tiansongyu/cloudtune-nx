# Eden development and verification

Eden is installed locally for testing this project's self-built homebrew.
The installer downloads only the emulator; it does not download console keys,
firmware, commercial games or any other Nintendo content.

## Installed release

| Item | Value |
| --- | --- |
| Release | Eden v0.2.1, official stable release |
| Published | 2026-06-01 |
| Build | Linux amd64 clang PGO |
| Installation | `~/.local/opt/eden/0.2.1/` |
| Launchers | `~/.local/bin/eden`, `~/.local/bin/eden-cli` |
| AppImage SHA-256 | `7a28bf988b0648831989722bdbaa90ab31371b403808199813e0ea7c8b25ba6d` |

The official [release page](https://git.eden-emu.dev/eden-emu/eden/releases/tag/v0.2.1)
lists the [downloaded asset](https://stable.eden-emu.dev/v0.2.1/Eden-Linux-v0.2.1-amd64-clang-pgo.AppImage).
A copy of the official release API response is retained under
`docs/reference/eden-release-0.2.1.json`.

```sh
./scripts/install-eden.sh
~/.local/bin/eden-cli --version
```

The installer extracts the AppImage, avoiding a dependency on FUSE mounting.
The amd64 build requires an x86-64-v3-capable CPU; the verification machine
uses an AMD Ryzen 9 9950X.

## Isolated virtual display

The host requires Xvfb and the X11 utility `xdpyinfo`. The launcher stores
Eden configuration, emulated SD data and caches under
`~/.local/share/netease-switch/eden/`, separate from an existing Eden profile.

```sh
./scripts/eden-virtual-desktop.sh
./scripts/eden-virtual-desktop.sh --cli \
  -g "$PWD/build/smoke/toolchain-smoke.nro"
```

The default display is `:119`, at 1600 × 1000. Set `EDEN_DISPLAY` to use a
different display. The initial GUI can show a missing-decryption-keys
prompt; choose **No** for unencrypted homebrew. The smoke NRO tested here
runs without imported keys or firmware.

To inspect the isolated display with ImageMagick:

```sh
DISPLAY=:119 import -window root build/validation/eden-screen.png
```

To run the application:

```sh
./scripts/eden-virtual-desktop.sh --cli \
  -g "$PWD/build/switch/NeteaseSwitch.nro" \
  > build/validation/eden-app.log 2>&1
```

To use the currently visible desktop instead of the isolated display:

```sh
EDEN_DISPLAY="$DISPLAY" ./scripts/eden-virtual-desktop.sh \
  -g "$PWD/build/switch/NeteaseSwitch.nro"
```

This opens the Qt interface with the same isolated emulator data. If the
“Derivation Components Missing” dialog asks to install decryption keys,
choose **No** for this self-built NRO. No console keys are needed for this
application. Review the SSL limitation below before authorizing an account.

The separate display `:120` is reserved for native desktop Borealis testing
on the development machine. Emulator and desktop processes can run side by
side without stealing focus from the user's main desktop.

The complete application was tested in both handheld and docked emulation.
Docked touch coordinates correctly select the visible sidebar row after the
Borealis input mapping fix. The CLI profile is
`config/eden/sdl2-config.ini` under the isolated state directory; change
`use_docked_mode` only while the CLI is stopped.

## Verified environment baseline

On 2026-10-20:

1. The pinned Docker toolchain produced a valid AArch64 ELF and NRO.
2. Eden CLI identified itself as `Eden v0.2.1 v0.2.1`.
3. The NRO loaded on Xvfb `:119`, initialized libnx and rendered the
   `NETEASE SWITCH TOOLCHAIN SMOKE TEST` screen.
4. Eden used Vulkan on an NVIDIA GeForce RTX 4090 (driver 580.142.0.0).
5. No console firmware or keys were imported.

Evidence: `build/validation/toolchain-smoke.log`,
`build/validation/eden-smoke.log`, and `build/validation/eden-smoke.png`.
Those generated logs are local validation artifacts; they are not source
dependencies.

The subsequent full NRO run rendered live playlist artwork and Chinese text,
generated a login QR code, and played anonymous API-provided MP3 streams.
It also exposed an audio-output lifecycle issue that the simple console
smoke could not detect. See [VALIDATION.md](VALIDATION.md) for the application
checks and their individual results.

## Scope of emulator results

A successful standalone NRO test establishes that the application can boot
and render in Eden. It does not establish Atmosphère sysmodule startup,
Tesla/Ultrahand overlay loading, audio coexistence with retail games, HOME
menu transitions, sleep/resume, or long-running networking on a physical
console. Keep those checks in the hardware test record.

Eden may log unimplemented services or synthesized system archives during
homebrew startup. Preserve the complete log and actual screen result
together: an isolated warning is not sufficient to establish either
success or failure.

Eden 0.2.1's
[`SetVerifyOptionImpl`](https://git.eden-emu.dev/eden-emu/eden/src/tag/v0.2.1/src/core/hle/service/ssl/ssl.cpp)
forces SSL verification options to zero. Its OpenSSL backend consequently
logs `skip_verification=true`, even though this application requests peer and
hostname validation. Successful networking in this emulator therefore does
**not** validate the certificate-checking path. Keep emulator testing to
anonymous discovery/playback and unapproved QR generation; authorize a real
account on the native desktop build or the console. The application's TLS
verification settings remain enabled.

## Compatibility fixes found during application bring-up

The initial complete NRO exposed two issues which the console-only smoke
test does not exercise:

- Borealis requested the optional `SetWirelessPriorityMode` IPC operation.
  Eden 0.2.1 asserted on this unimplemented operation before drawing the UI.
  The app now leaves Horizon's wireless priority unchanged.
- Requesting `ncm:bg` before a background service exists causes the service
  manager to wait indefinitely. The client now checks for Atmosphère and
  service presence before requesting a connection, allowing the standalone
  app to continue when the background service is unavailable.

The captured bring-up logs are
`build/validation/eden-app-first-boot-internal.log` and
`build/validation/eden-app-second-internal.log`. These are diagnostic
failures from earlier builds, not evidence of a successful final app run.
