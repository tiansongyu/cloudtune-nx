#!/usr/bin/env bash
set -euo pipefail
# One audited source release for host and Switch; ALAC/AAC audio only.
# The official release signature was checked against the fingerprint recorded in
# airplay/vendor/ffmpeg-source.json. That manifest is the single source pin.
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
platform="${1:-host}"
case "$platform" in
    host|switch) ;;
    *) echo "Usage: $0 [host|switch]" >&2; exit 2 ;;
esac
manifest="$repo_root/airplay/vendor/ffmpeg-source.json"
source_metadata="$(python3 - "$manifest" <<'PY'
import json
import pathlib
import re
import sys
source = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert source["format_version"] == 1 and source["name"] == "FFmpeg", "Invalid source manifest"
assert re.fullmatch(r"\d+\.\d+\.\d+", source["version"]), "Invalid FFmpeg version"
assert source["archive"] == f"ffmpeg-{source['version']}.tar.xz", "Invalid archive name"
assert source["url"] == f"https://ffmpeg.org/releases/{source['archive']}", "Invalid source URL"
assert re.fullmatch(r"[0-9a-f]{64}", source["sha256"]), "Invalid source checksum"
for name in ("version", "sha256", "archive", "url"):
    print(source[name])
PY
)"
mapfile -t source_fields <<< "$source_metadata"
version="${source_fields[0]}"
expected="${source_fields[1]}"
deps="$repo_root/.deps"
archive="$deps/downloads/${source_fields[2]}"
source_dir="$deps/src/ffmpeg-$version"
prefix="$deps/ffmpeg-$platform"
build_dir="$deps/build/ffmpeg-$platform"
mkdir -p "$deps/downloads" "$deps/src" "$build_dir"
if [[ ! -f "$archive" ]]; then
    if [[ -f "$repo_root/${source_fields[2]}" ]]; then
        # docs.zip carries this exact archive beside the standalone recipe.
        cp "$repo_root/${source_fields[2]}" "$archive.part"
    else
        curl -fL --retry 3 "${source_fields[3]}" -o "$archive.part"
    fi
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$expected" "$archive" | sha256sum --check
if [[ ! -f "$source_dir/configure" ]]; then
    tar -xJf "$archive" -C "$deps/src"
fi
flags=(
    "--prefix=$prefix" --disable-autodetect --disable-everything --enable-decoder=alac,aac
    --enable-avformat --enable-demuxer=aac,mov --enable-parser=aac
    --disable-programs --disable-doc --disable-debug --disable-network
    --disable-avdevice --disable-avfilter --disable-swscale
    --disable-swresample --disable-postproc --disable-pthreads --disable-w32threads
    --disable-os2threads --disable-asm --disable-neon --disable-runtime-cpudetect
    --enable-small --enable-static --disable-shared --enable-pic
)
compiler="${CC:-cc}"
if [[ "$platform" == switch ]]; then
    : "${DEVKITPRO:=/opt/devkitpro}"
    compiler="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc"
    flags+=(
        --enable-cross-compile --target-os=none --arch=aarch64
        "--cross-prefix=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-"
        "--cc=$compiler" --pkg-config=false
        "--extra-cflags=-Os -ffunction-sections -fdata-sections -march=armv8-a+crc+crypto -mtp=soft -fPIE -D__SWITCH__ -I$DEVKITPRO/libnx/include"
        "--extra-ldflags=-march=armv8-a+crc+crypto -mtp=soft -fPIE -specs=$DEVKITPRO/libnx/switch.specs -Wl,--gc-sections -L$DEVKITPRO/libnx/lib"
        --extra-libs=-lnx
    )
else
    flags+=("--cc=$compiler" "--extra-cflags=-Os -ffunction-sections -fdata-sections")
fi
# A relocated checkout, changed recipe or compiler deliberately invalidates the
# cache, so a stale static library cannot silently enter a release build.
recipe="$(cat "$repo_root/scripts/setup-airplay-codec.sh" "$manifest" | sha256sum | cut -d' ' -f1)"
identity="$(printf '%s\n' "$expected" "$recipe" "$prefix" "${flags[@]}" "$("$compiler" --version)" |
    sha256sum | cut -d' ' -f1)"
if [[ -f "$prefix/.cloudtune-codec-build" &&
      "$(cat "$prefix/.cloudtune-codec-build")" == "$identity" &&
      -f "$prefix/lib/libavformat.a" && -f "$prefix/lib/libavcodec.a" &&
      -f "$prefix/lib/libavutil.a" ]]; then
    printf 'ALAC/AAC FFmpeg %s (%s): cached in %s\n' "$version" "$platform" "$prefix"
    exit 0
fi
cd "$build_dir"
if [[ -f Makefile ]]; then
    make distclean
fi
"$source_dir/configure" "${flags[@]}"
make -j"${BUILD_JOBS:-8}"
make install
mkdir -p "$prefix/share/licenses/ffmpeg"
cp "$source_dir/COPYING.LGPLv2.1" "$source_dir/COPYING.LGPLv3" "$source_dir/LICENSE.md" \
    "$prefix/share/licenses/ffmpeg/"
printf '%s\n' "$identity" > "$prefix/.cloudtune-codec-build"
printf 'ALAC/AAC FFmpeg %s (%s) installed in %s\n' "$version" "$platform" "$prefix"
