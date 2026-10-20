# Background and overlay attribution

- `background/source/ipc_server.c` and `.h` adapt Switify's phase2 server,
  based on the sys-clk server by p-sam, natinusala and m4xw. The original
  Beer-Ware license is preserved in both files. NetEase Switch changes
  service names and log paths. Reference Switify revision:
  `e4b86610598dcf511b3682be0e047855e7445ac6`.
- `background/vendor/dr_mp3.h` is David Reid's dr_mp3 from
  <https://github.com/mackron/dr_libs>, retrieved 2026-10-20. Its dual choice of
  MIT-0 / public-domain license and embedded minimp3 attributions remain in
  the file. SHA-256:
  `997b7ee18de6e6b81e2a83f1ea9fc62aef25c62b28d48db95635f49e65de0a2f`.
- `overlay/vendor/libtesla/tesla.hpp` is the original WerWolv libtesla,
  <https://github.com/WerWolv/libtesla>, retrieved 2026-10-20. Its GPL-2.0
  license is included as `overlay/vendor/libtesla/LICENSE`. SHA-256:
  `132f0ba423392dd6f8fe0eac7a110a560e05e2bcf3c0e47058e27447c95b8661`.
- `overlay/vendor/libtesla/stb_truetype.h` is the stb TrueType renderer,
  retained from the Switify dependency tree with its embedded MIT /
  public-domain license. SHA-256:
  `161778620c6dd428af3bc640b2342c3f9829ba625446ba2892d1772773c19185`.

The new standalone overlay is GPL-2.0-only. The new shared protocol and C
client are GPL-2.0-or-later so they can be used by both the GPL-2.0 overlay
and GPL-3.0 application/service. The new playback engine is
GPL-3.0-or-later. The processes communicate through the documented C ABI.
