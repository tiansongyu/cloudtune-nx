#!/usr/bin/env bash
set -euo pipefail
# Official release asset, deliberately pinned. No firmware, keys or games.
version="0.2.1"
eden_dir="${EDEN_INSTALL_DIR:-$HOME/.local/opt/eden/$version}"
url="https://stable.eden-emu.dev/v${version}/Eden-Linux-v${version}-amd64-clang-pgo.AppImage"
mkdir -p "$eden_dir" "$HOME/.local/bin"
curl -fL --retry 3 "$url" -o "$eden_dir/Eden.AppImage"
chmod +x "$eden_dir/Eden.AppImage"
(cd "$eden_dir" && ./Eden.AppImage --appimage-extract)
printf '#!/usr/bin/env bash\nexec %q "$@"\n' "$eden_dir/squashfs-root/AppRun" \
    > "$HOME/.local/bin/eden"
printf '#!/usr/bin/env bash\nexec %q eden-cli "$@"\n' "$eden_dir/squashfs-root/AppRun" \
    > "$HOME/.local/bin/eden-cli"
chmod +x "$HOME/.local/bin/eden" "$HOME/.local/bin/eden-cli"
sha256sum "$eden_dir/Eden.AppImage"
"$HOME/.local/bin/eden-cli" --version
