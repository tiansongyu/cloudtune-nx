# Development reference snapshots

Downloaded on **2026-10-20** from the upstream projects listed below.
These files are reference material, not locally installed SDK headers.
The build uses the versions inside the pinned Docker image.

| Local file | Upstream and revision |
| --- | --- |
| `libnx/index.html` | [libnx API reference](https://switchbrew.github.io/libnx/), retrieval-date snapshot |
| `libnx/audout.h` | [Audio output service](https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/include/switch/services/audout.h) |
| `libnx/applet.h` | [Applet lifecycle API](https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/include/switch/services/applet.h) |
| `libnx/service.h` | [Service and IPC API](https://github.com/switchbrew/libnx/blob/b156b4774fb2df7b93402169509266da7547fc4c/nx/include/switch/sf/service.h) |
| `libnx/LICENSE.md` | libnx ISC license at the same revision |
| `switch-examples/application.Makefile` | [Official homebrew application template](https://github.com/switchbrew/switch-examples/blob/669786898205b7beb25ff1731e72982e6d0397d3/templates/application/Makefile) |
| `switch-examples/README.upstream.md` | [switch-examples README](https://github.com/switchbrew/switch-examples/blob/669786898205b7beb25ff1731e72982e6d0397d3/README.md), examples described as public domain |
| `devkitpro-devkita64.Dockerfile` | [Official devkitPro Docker source](https://github.com/devkitPro/docker/blob/master/devkita64/Dockerfile), retrieval-date snapshot |
| `devkitpro-LICENSE` | License distributed with devkitPro/docker |
| `eden-release-0.2.1.json` | [Official release metadata API](https://git.eden-emu.dev/api/v1/repos/eden-emu/eden/releases/latest), resolved to v0.2.1 at retrieval |

The copied libnx header revision is
`b156b4774fb2df7b93402169509266da7547fc4c`. The example revision is
`669786898205b7beb25ff1731e72982e6d0397d3`.
[`SHA256SUMS.json`](SHA256SUMS.json) records downloaded file hashes.

Useful live references:

- [libnx source and releases](https://github.com/switchbrew/libnx)
- [devkitPro toolchain images](https://github.com/devkitPro/docker)
- [Switch homebrew examples](https://github.com/switchbrew/switch-examples)
- [Atmosphère source and releases](https://github.com/Atmosphere-NX/Atmosphere)
- [libtesla](https://github.com/WerWolv/libtesla)
- [libultrahand](https://github.com/ppkantorski/libultrahand)

Third-party licenses and copyright notices are retained with their files.
