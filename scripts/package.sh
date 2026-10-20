#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
build_dir="${SWITCH_BUILD_DIR:-$repo_root/build/switch}"
background_dir="${BACKGROUND_BUILD_DIR:-$build_dir/background}"
overlay_dir="${OVERLAY_BUILD_DIR:-$repo_root/overlay}"
version="${RELEASE_VERSION:-$(sed -n 's/^project(NeteaseSwitch VERSION \([^ ]*\).*/\1/p' CMakeLists.txt)}"
[[ "$version" =~ ^[0-9A-Za-z][0-9A-Za-z._-]*$ ]] || {
    echo "Invalid release version: $version" >&2; exit 1;
}

app="$build_dir/NeteaseSwitch.nro"
app_elf="$build_dir/NeteaseSwitch.elf"
service="$background_dir/netease-background.nsp"
service_elf="$background_dir/netease-background.elf"
overlay="$overlay_dir/netease-overlay.ovl"
overlay_elf="$overlay_dir/netease-overlay.elf"
for artifact in "$app" "$app_elf" "$service" "$service_elf" "$overlay" "$overlay_elf" \
    resources/certs/cacert.pem background/toolbox.json; do
    [[ -s "$artifact" ]] || { echo "Missing artifact: $artifact" >&2; exit 1; }
done
python3 - "$app" "$overlay" "$service" "$app_elf" "$service_elf" "$overlay_elf" <<'PY'
import pathlib, struct, sys
for arg in sys.argv[1:3]:
    with open(arg, 'rb') as f:
        f.seek(16)
        assert f.read(4) == b'NRO0', f'Invalid NRO/overlay header: {arg}'
package = pathlib.Path(sys.argv[3]).read_bytes()
assert package[:4] == b'PFS0', 'Background service is not an ExeFS PFS0 package'
count, string_size = struct.unpack_from('<II', package, 4)
strings = 16 + count * 24
data = strings + string_size
entries = {}
for n in range(count):
    offset, size, name_offset = struct.unpack_from('<QQI', package, 16 + n * 24)
    name = package[strings+name_offset:].split(b'\0', 1)[0].decode()
    assert data + offset + size <= len(package), 'Invalid PFS0 file size'
    entries[name] = package[data+offset:data+offset+size]
assert 'main' in entries and 'main.npdm' in entries, 'Incomplete ExeFS package'
assert entries['main'][:4] == b'NSO0', 'Background main is not an NSO'
b = entries['main.npdm']
aci = struct.unpack_from('<I', b, 0x70)[0]
acid = struct.unpack_from('<I', b, 0x78)[0]
program = struct.unpack_from('<Q', b, aci+0x10)[0]
lo, hi = struct.unpack_from('<QQ', b, acid+0x210)
assert b[:4] == b'META' and b[aci:aci+4] == b'ACI0'
assert b[acid+0x200:acid+0x204] == b'ACID'
assert program == lo == hi == 0x42000000004E434D, 'Incorrect system-module program ID'
def elf_build_id(arg):
    elf = pathlib.Path(arg).read_bytes()
    assert elf[:4] == b'\x7fELF' and elf[4:6] == b'\x02\x01', 'Expected ELF64 little endian'
    assert struct.unpack_from('<H', elf, 18)[0] == 183, f'Expected AArch64 ELF: {arg}'
    section_offset = struct.unpack_from('<Q', elf, 40)[0]
    section_size, section_count = struct.unpack_from('<HH', elf, 58)
    for index in range(section_count):
        section = section_offset + index * section_size
        if struct.unpack_from('<I', elf, section+4)[0] != 7:
            continue
        offset, size = struct.unpack_from('<QQ', elf, section+24)
        end = offset + size
        while offset + 12 <= end:
            name_size, desc_size, kind = struct.unpack_from('<III', elf, offset)
            offset += 12
            name = elf[offset:offset+name_size]
            offset += (name_size + 3) & ~3
            desc = elf[offset:offset+desc_size]
            offset += (desc_size + 3) & ~3
            if name.rstrip(b'\0') == b'GNU' and kind == 3:
                return desc
    raise AssertionError(f'ELF lacks GNU build ID: {arg}')
for elf, binary in (
    (sys.argv[4], pathlib.Path(sys.argv[1]).read_bytes()),
    (sys.argv[5], entries['main']),
    (sys.argv[6], pathlib.Path(sys.argv[2]).read_bytes()),
):
    build_id = elf_build_id(elf)
    assert binary[64:96] == build_id.ljust(32, b'\0'), f'ELF does not match binary: {elf}'
print('NRO, overlay, NSO, NPDM program ID and all matching ELF build IDs verified.')
PY
[[ "${1:-}" != "--check" ]] || exit 0

# A unique directory preserves earlier release candidates and matching ELFs.
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
release_name="NeteaseSwitch-${version}-${stamp}"
release_dir="$repo_root/dist/$release_name"
mkdir -p "$release_dir"
sd="$release_dir/sd"
symbols="$release_dir/symbols"
title="42000000004E434D"
mkdir -p "$sd/switch/NeteaseSwitch" "$sd/switch/.overlays" \
    "$sd/atmosphere/contents/$title/flags" "$sd/config/netease-switch" "$symbols"
cp "$app" "$sd/switch/NeteaseSwitch/NeteaseSwitch.nro"
cp "$overlay" "$sd/switch/.overlays/NeteaseSwitch.ovl"
cp "$service" "$sd/atmosphere/contents/$title/exefs.nsp"
cp background/toolbox.json "$sd/atmosphere/contents/$title/toolbox.json"
: > "$sd/atmosphere/contents/$title/flags/boot2.flag"
cp resources/certs/cacert.pem "$sd/config/netease-switch/cacert.pem"
cp "$app_elf" "$symbols/NeteaseSwitch.elf"
cp "$service_elf" "$symbols/netease-background.elf"
cp "$overlay_elf" "$symbols/netease-overlay.elf"
[[ ! -f "$build_dir/NeteaseSwitch.map" ]] || cp "$build_dir/NeteaseSwitch.map" "$symbols/"
[[ ! -f "$overlay_dir/netease-overlay.map" ]] || cp "$overlay_dir/netease-overlay.map" "$symbols/"
[[ ! -f build/validation/toolchain-packages.txt ]] || \
    cp build/validation/toolchain-packages.txt "$symbols/"

cp docs/INSTALLING.md "$release_dir/INSTALLING.md"
cp docs/VALIDATION.md "$release_dir/VALIDATION.md"
cp README.md "$release_dir/README.md"
cp -R docs "$release_dir/docs"
mkdir -p "$release_dir/licenses"
cp resources/certs/LICENSE-MPL-2.0.txt "$release_dir/licenses/"
[[ ! -f LICENSE ]] || cp LICENSE "$release_dir/licenses/NeteaseSwitch-LICENSE"
[[ ! -f THIRD_PARTY_NOTICES.md ]] || cp THIRD_PARTY_NOTICES.md "$release_dir/licenses/"
[[ ! -f background/THIRD_PARTY_NOTICES.md ]] || \
    cp background/THIRD_PARTY_NOTICES.md "$release_dir/licenses/background-THIRD_PARTY_NOTICES.md"
[[ ! -f overlay/vendor/libtesla/LICENSE ]] || \
    cp overlay/vendor/libtesla/LICENSE "$release_dir/licenses/libtesla-LICENSE"
# Retain the actual dependency notices, including font licenses and source-
# embedded notices, rather than only a table that points back into the repo.
python3 - "$release_dir/licenses" <<'PY'
import pathlib, shutil, sys
destination = pathlib.Path(sys.argv[1])
sources = []
for root in ('vendor', 'resources', 'background/vendor', 'overlay/vendor'):
    for p in pathlib.Path(root).rglob('*'):
        if p.is_file() and any(word in p.name.upper() for word in ('LICENSE', 'COPYING', 'NOTICE')):
            sources.append(p)
sources += [pathlib.Path('vendor/qrcodegen/qrcodegen.hpp'),
            pathlib.Path('background/vendor/dr_mp3.h')]
for p in sources:
    target = destination / p
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(p, target)
PY
export NCM_PACKAGE_VERSION="$version" NCM_PACKAGE_STAMP="$stamp"
python3 - "$release_dir" <<'PY'
import hashlib, json, os, pathlib, subprocess, sys
p = pathlib.Path(sys.argv[1])
def git(*args):
    r = subprocess.run(['git', *args], text=True, capture_output=True)
    return r.stdout.strip() if r.returncode == 0 else None
manifest = {
    'product': 'Netease Switch',
    'version': os.environ['NCM_PACKAGE_VERSION'],
    'packaged_utc': os.environ['NCM_PACKAGE_STAMP'],
    'git_commit': git('rev-parse', 'HEAD'),
    'source_has_uncommitted_changes': bool(git('status', '--porcelain')),
    'background_title_id': '42000000004E434D',
    'files': {},
}
for f in sorted(p.rglob('*')):
    if f.is_file():
        manifest['files'][str(f.relative_to(p))] = {
            'bytes': f.stat().st_size,
            'sha256': hashlib.sha256(f.read_bytes()).hexdigest(),
        }
(p/'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
lines = []
for f in sorted(p.rglob('*')):
    if f.is_file():
        lines.append(hashlib.sha256(f.read_bytes()).hexdigest() + '  ' + str(f.relative_to(p)))
(p/'SHA256SUMS').write_text('\n'.join(lines) + '\n')
PY
# SD zip has the SD-card directories at its root. Symbols never go on the SD.
(cd "$sd" && zip -q -X -r "$repo_root/dist/$release_name-sd.zip" .)
(cd "$release_dir" && zip -q -X -r "$repo_root/dist/$release_name-symbols.zip" \
    symbols manifest.json SHA256SUMS)
(cd "$release_dir" && zip -q -X -r "$repo_root/dist/$release_name-docs.zip" \
    README.md INSTALLING.md VALIDATION.md docs licenses manifest.json SHA256SUMS)
(
    cd "$repo_root/dist"
    sha256sum "$release_name-sd.zip" "$release_name-symbols.zip" "$release_name-docs.zip" \
        > "$release_name-SHA256SUMS"
)
printf '\nRelease directory: %s\nSD archive: %s\nSymbols archive: %s\n' \
    "$release_dir" "$repo_root/dist/$release_name-sd.zip" \
    "$repo_root/dist/$release_name-symbols.zip"
