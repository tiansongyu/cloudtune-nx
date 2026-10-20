#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
[[ -f .deps/mbedtls/include/mbedtls/aes.h ]] || ./scripts/setup-host-deps.sh
./scripts/setup-airplay-codec.sh host
cmake -S . -B build/desktop -G Ninja -DPLATFORM_DESKTOP=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/desktop --parallel "${BUILD_JOBS:-8}"
ctest --test-dir build/desktop --output-on-failure
build/desktop/ncm-api-tests --fixture | python3 tests/verify_xeapi.py

# Generated data only. No user music or login is required by these tests.
run_dir="$repo_root/build/validation/audio-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$run_dir"
ffmpeg -hide_banner -loglevel error -f lavfi -i 'sine=frequency=440:duration=8' \
    -ar 44100 -ac 2 -codec:a libmp3lame -b:a 128k "$run_dir/test.mp3"
SDL_AUDIODRIVER=dummy build/desktop/background/ncm-playback-smoke \
    "$run_dir/local" "$run_dir/test.mp3" | tee "$run_dir/local.log"

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$run_dir/key.pem" -out "$run_dir/cert.pem" \
    -subj '/CN=localhost' -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
    > "$run_dir/certificate.log" 2>&1
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
python3 background/tests/https_fixture.py "$run_dir" "$run_dir/cert.pem" \
    "$run_dir/key.pem" "$port" > "$run_dir/server.log" 2>&1 &
server_pid="$!"
trap 'kill "$server_pid" 2>/dev/null || true' EXIT
for _ in $(seq 1 40); do
    curl --silent --fail --head --cacert "$run_dir/cert.pem" \
        "https://localhost:$port/test.mp3" >/dev/null 2>&1 && break
    sleep 0.1
done
SDL_AUDIODRIVER=dummy build/desktop/background/ncm-playback-smoke \
    "$run_dir/https" "https://localhost:$port/test.mp3" "$run_dir/cert.pem" \
    | tee "$run_dir/https.log"
printf 'Host validation evidence: %s\n' "$run_dir"
