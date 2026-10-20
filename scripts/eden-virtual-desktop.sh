#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
state_dir="${EDEN_STATE_DIR:-$HOME/.local/share/netease-switch/eden}"
display="${EDEN_DISPLAY:-:119}"
mkdir -p "$state_dir"/{config,data,cache} "$repo_root/build/validation"
if ! DISPLAY="$display" xdpyinfo >/dev/null 2>&1; then
    Xvfb "$display" -screen 0 1600x1000x24 -nolisten tcp \
        > "$repo_root/build/validation/xvfb.log" 2>&1 &
    echo "$!" > "$repo_root/build/validation/xvfb.pid"
    for _ in $(seq 1 40); do
        DISPLAY="$display" xdpyinfo >/dev/null 2>&1 && break
        sleep 0.1
    done
fi
export DISPLAY="$display"
export XDG_CONFIG_HOME="$state_dir/config"
export XDG_DATA_HOME="$state_dir/data"
export XDG_CACHE_HOME="$state_dir/cache"
export QT_QPA_PLATFORM=xcb
if [[ "${1:-}" == "--cli" ]]; then
    shift
    exec "$HOME/.local/bin/eden-cli" "$@"
fi
exec "$HOME/.local/bin/eden" "$@"
