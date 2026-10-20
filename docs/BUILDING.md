# Building CloudTune NX

The repository produces a standalone Nintendo Switch NRO, an Atmosphère
background service, and an Ultrahand/Tesla overlay. The NRO and overlay are
separate clients of the same background player.

## Reproducible Docker toolchain

Requirements: Linux, macOS or Windows with a working Docker installation,
Git, and approximately 4 GB of free space for the toolchain and build outputs.
On Windows, run the scripts in WSL2 with Docker integration enabled.

```sh
git clone https://github.com/tiansongyu/cloudtune-nx.git
cd cloudtune-nx
./scripts/docker-build.sh --image
./scripts/docker-build.sh --smoke
./scripts/docker-build.sh
```

The image is named `cloudtune-nx/toolchain:local`. Its upstream
`devkitpro/devkita64` image is pinned by digest in
[`docker/Dockerfile`](../docker/Dockerfile). The image contains the complete
official Switch portlibs set; the build does not silently upgrade packages.
To use a separately maintained image:

```sh
CLOUDTUNE_IMAGE=my-toolchain:verified BUILD_JOBS=12 ./scripts/docker-build.sh
```

`CLOUDTUNE_IMAGE` takes precedence over the compatible legacy variable
`NETEASE_SWITCH_IMAGE`. The CMake project is `CloudTuneNX`; its application
target and executable basename are `CloudTune`.

The container runs as your host UID/GID, so generated files remain editable
without `sudo`. Source code is mounted at `/workspace`; toolchains live under
`/opt/devkitpro`.

| Component | Verified version |
| --- | --- |
| devkitA64 | r29.2-1 |
| libnx | 4.12.0-1 |
| switch-cmake | 1.5.2-1 |
| switch-curl | 7.69.1-5 |
| switch-mbedtls | 2.28.10-1 |
| switch-zlib | 1.3.1-1 |
| switch-mesa | 20.1.0-5 |
| switch-glfw | 3.3.4-2 |

The complete package inventory and compiler versions are recorded on every
build in `build/validation/toolchain-packages.txt` and
`build/validation/compiler.txt`.

The API client uses mbedTLS for AES, RSA, X25519 and GCM. The official Switch
portlibs image does not provide an OpenSSL package. Do not link host
`libcrypto.so` into a Switch build.

## Outputs

The primary app outputs are:

```text
build/switch/CloudTune.nro
build/switch/CloudTune.elf
build/switch/background/cloudtune-background.nsp
build/switch/background/cloudtune-background.elf
build/overlay/CloudTune.ovl
build/overlay/CloudTune.elf
build/overlay/CloudTune.nacp
build/overlay/CloudTune.map
```

Overlay intermediate objects are isolated under `build/overlay/obj/`.
The `overlay/` source directory contains source and dependency files only.

The smoke test writes `build/smoke/toolchain-smoke.nro` and its ELF. It verifies
the compiler, libnx, NACP metadata generation and NRO packaging independently
from application dependencies.

Generate the SD deployment archive and separate debugging symbols:

```sh
./scripts/package.sh
# Fixed, versioned assets for a GitHub Release:
./scripts/package.sh --release
```

The packager validates NRO/overlay headers, the ExeFS NSP, its NSO and NPDM
program ID, the ELF architecture, and each binary's build ID against its
matching ELF before staging files under `build/package/`. The default local
mode retains timestamped `CloudTuneNX-<version>-<timestamp>` packages under
`dist/`, preserving earlier candidates. Release mode writes fixed names under
`dist/release/`:

```text
CloudTuneNX-<version>-sd.zip
CloudTuneNX-<version>-symbols.zip
CloudTuneNX-<version>-docs.zip
CloudTuneNX-<version>-manifest.json
SHA256SUMS
```

The manifest records the source revision and per-file SHA-256 hashes.
Only the `-sd.zip` contents belong on the SD card;
see [`INSTALLING.md`](INSTALLING.md).

The symbols archive separates identically named app and overlay ELFs:
`symbols/app/CloudTune.elf`, `symbols/overlay/CloudTune.elf`, and
`symbols/background/cloudtune-background.elf`. Keep this structure when
matching crash reports to the appropriate process.

Always retain the ELF corresponding to the NRO or system module tested on
hardware. An ELF from a later build cannot reliably symbolicate an earlier
Atmosphère crash report.

Build logs are written under `build/validation/`. The first environment
verification on 2026-10-20 compiled an AArch64 NRO and ran it successfully in
Eden 0.2.1 on the isolated Xvfb display `:119`; see
[`EMULATOR.md`](EMULATOR.md) for the reproduction commands and limits.

## Direct container access

```sh
docker run --rm -it \
  --user "$(id -u):$(id -g)" \
  -v "$PWD:/workspace" -w /workspace \
  cloudtune-nx/toolchain:local bash
```

Inside the container:

```sh
cmake -S . -B build/switch -G Ninja \
  -DPLATFORM_SWITCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/switch \
  --target CloudTune.nro cloudtune-background-package --parallel 8
make -C overlay -j8
```

The Borealis toolchain chooses the official devkitPro Switch CMake files.
Never reuse a desktop build directory for Switch compilation.

## Isolated host crypto dependency

For native desktop development, use installed GLFW, SDL2 and libcurl
development packages. If mbedTLS headers are absent, the following builds
the same 2.28.10 source release used by the Switch port under `.deps/mbedtls`:

```sh
./scripts/setup-host-deps.sh
cmake -S . -B build/desktop -G Ninja \
  -DPLATFORM_DESKTOP=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$PWD/.deps/mbedtls"
```

The source archive is pinned by SHA-256. This script requires a host C
compiler, CMake and Ninja; it does not run `apt`, repair existing package
state, require root, or write to `/usr`.

Run the native application in the separate virtual desktop:

```sh
cmake --build build/desktop --parallel 8
DISPLAY=:120 LP_NUM_THREADS=4 ./build/desktop/CloudTune
```

The display must already exist. For a headless machine, create it using
`Xvfb :120 -screen 0 1600x1000x24 -nolisten tcp` in a separate terminal.
Set `CLOUDTUNE_DATA` to isolate cookies, settings and audio caches; the
legacy `NETEASE_SWITCH_DATA` variable remains accepted when `CLOUDTUNE_DATA`
is unset. The default native data location and existing Eden profile retain
the `netease-switch` directory name to preserve sessions across the rename.
`SDL_AUDIODRIVER=dummy` is available for automated runs without speakers;
it does not establish audible output quality.

## Host validation

```sh
./scripts/test-host.sh
```

This configures a Debug build (assertions enabled), runs the API, library,
model and filesystem tests, checks XEAPI output with an independent Python
implementation, then exercises generated MP3 audio locally and over a
throttled local HTTPS server. The audio test covers transport controls,
natural track changes, end-of-queue behavior, malformed queues and cache
isolation by quality/session.

Additional test tools are Python 3 with `cryptography`, FFmpeg, OpenSSL and
curl. Fixtures and temporary TLS keys stay in ignored `build/validation/`.
No account sign-in, SMS or personal music collection is needed.

## GitHub Actions and releases

[CI](https://github.com/tiansongyu/cloudtune-nx/actions/workflows/ci.yml)
runs for pull requests, pushes to `main`, and manual dispatch. Release tags
reuse that workflow and publish only after host and Switch checks succeed.
The published assets are the packages from that same workflow run.
See [RELEASING.md](RELEASING.md) for tag/version checks, permissions and the
complete output layout. An Actions result is separate from emulator and
physical-console validation.

## Troubleshooting

**Docker permission denied:** configure your Docker installation so your user
can run `docker info`; the scripts do not change your group membership.

**A required package is missing:** check the pinned image and package list.
Changing the upstream digest is an explicit toolchain update and requires
repeating the smoke build and emulator test.

**CMake fails after a platform change:** use separate build directories
(`build/switch`, `build/desktop`, `build/host`) for different toolchains.

**Resources or Chinese text are absent:** check the packaged RomFS, font
resources and Borealis resource paths before attributing the issue to Eden.
Eden synthesizes some system font archives when no firmware is installed;
that does not reproduce every real Switch font.

**Background audio differs on hardware:** the overlay requires a compatible
overlay loader and the NSP requires Atmosphère. Their memory, IPC and audio
resource behavior must also be tested on a physical console.
