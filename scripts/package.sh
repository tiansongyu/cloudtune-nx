#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
mode="${1:-local}"
case "$mode" in
    local|--check|--release) ;;
    --help|-h)
        echo "Usage: scripts/package.sh [--check|--release]"
        echo "Default: timestamped development archives in dist/; staging in build/package/."
        echo "--release: clean, verified build -> exactly five assets in dist/release/."
        exit 0 ;;
    *) echo "Unknown option: $mode" >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { echo "Too many arguments" >&2; exit 2; }

python3 - "$repo_root" "$mode" <<'PY'
import datetime
import fcntl
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

sys.dont_write_bytecode = True
root = pathlib.Path(sys.argv[1])
mode = sys.argv[2]
release = mode == "--release"
spec = importlib.util.spec_from_file_location("package_verifier", root / "scripts/verify-package.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)
require = verifier.require


def git(*args):
    return subprocess.check_output(["git", *args], cwd=root, text=True).strip()


def copy(source, target):
    source = pathlib.Path(source)
    require(source.is_file() and not source.is_symlink(), f"Missing/unsafe package input: {source}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


def archive(source, destination, *, prefix="", epoch):
    records = {}
    timestamp = datetime.datetime.fromtimestamp(max(epoch, 315532800), datetime.timezone.utc)
    date = (timestamp.year, timestamp.month, timestamp.day,
            timestamp.hour, timestamp.minute, timestamp.second)
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        # Explicit directory entries preserve the hidden .overlays directory.
        for path in sorted(source.rglob("*")):
            require(not path.is_symlink(), f"Symlink in staging: {path}")
            name = prefix + path.relative_to(source).as_posix()
            info = zipfile.ZipInfo(name + ("/" if path.is_dir() else ""), date)
            info.create_system = 3
            info.external_attr = ((0o40755 if path.is_dir() else 0o100644) << 16)
            if path.is_dir():
                info.external_attr |= 0x10
                z.writestr(info, b"")
            else:
                data = path.read_bytes()
                z.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
                records[name] = verifier.file_record(data)
    return records


try:
    match = re.search(r"project\(CloudTuneNX\s+VERSION\s+(\d+\.\d+\.\d+)\b",
                      (root / "CMakeLists.txt").read_text())
    require(match is not None, "Cannot read CMake project version")
    version = match.group(1)
    requested = os.environ.get("RELEASE_VERSION")
    require(requested is None or requested == version, "RELEASE_VERSION must equal CMake version")
    commit = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain"))
    if release:
        require(not dirty, "Release packaging requires a clean Git worktree and index")
        require(not (root / "dist/release").exists(),
                "dist/release already exists; preserve it elsewhere before a new release build")

    build = pathlib.Path(os.environ.get("SWITCH_BUILD_DIR", root / "build/switch"))
    background = pathlib.Path(os.environ.get("BACKGROUND_BUILD_DIR", build / "background"))
    overlay = pathlib.Path(os.environ.get("OVERLAY_BUILD_DIR", root / "build/overlay"))
    paths, components = verifier.validate_build(build, background, overlay, version)
    print("Verified NRO/OVL NACP versions, ExeFS/NPDM and three matching AArch64 ELFs.")
    codec_source, source_paths, codec_licenses = verifier.validate_third_party_inputs(root)
    print("Verified pinned FFmpeg corresponding source, upstream licenses and Shairport notices.")
    if mode == "--check":
        sys.exit(0)

    provenance_path = root / "build/validation/build-provenance.json"
    provenance = json.loads(provenance_path.read_text()) if provenance_path.is_file() else None
    if release:
        require(provenance is not None, "Missing build provenance; run scripts/docker-build.sh first")
        require(provenance.get("git_commit") == commit
                and provenance.get("version") == version
                and provenance.get("source_has_uncommitted_changes") is False,
                "Build provenance is not from this clean commit; rebuild after committing changes")
        require(provenance.get("components") == components,
                "Build artifacts changed after their provenance was recorded")

    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    prefix = f"CloudTuneNX-{version}" + ("" if release else f"-{stamp}")
    dist = root / "dist"
    dist.mkdir(exist_ok=True)
    lock = (dist / ".package.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    names = {kind: f"{prefix}-{kind}.zip" for kind in ("sd", "symbols", "docs")}
    manifest_name = f"{prefix}-manifest.json"
    local_names = list(names.values()) + [manifest_name, f"{prefix}-SHA256SUMS"]
    if release:
        require(not (dist / "release").exists(), "Refusing to overwrite existing release assets")
    else:
        require(not any((dist / name).exists() for name in local_names),
                "Timestamped package already exists; retry with a new timestamp")
    staging_parent = root / "build/package"
    staging_parent.mkdir(parents=True, exist_ok=True)
    stage = pathlib.Path(tempfile.mkdtemp(prefix=prefix + "-", dir=staging_parent))
    sd, symbols, docs, output = [stage / name for name in ("sd", "symbols", "documentation", "assets")]
    for path in (sd, symbols, docs, output):
        path.mkdir()

    for component, (binary_path, elf_path) in verifier.COMPONENT_PATHS.items():
        copy(paths[component][0], sd / binary_path)
        copy(paths[component][1], stage / elf_path)
    title = verifier.TITLE_ID
    copy(root / "background/toolbox.json", sd / f"atmosphere/contents/{title}/toolbox.json")
    flag = sd / f"atmosphere/contents/{title}/flags/boot2.flag"
    flag.parent.mkdir(parents=True)
    flag.write_bytes(b"")
    copy(root / "resources/certs/cacert.pem", sd / "config/netease-switch/cacert.pem")
    for source, target in (
        (build / "CloudTune.map", symbols / "app/CloudTune.map"),
        (overlay / "CloudTune.map", symbols / "overlay/CloudTune.map"),
        (root / "build/validation/toolchain-packages.txt", symbols / "toolchain-packages.txt"),
    ):
        if source.is_file():
            copy(source, target)

    for name in ("README.md", "LICENSE", "CONTRIBUTING.md", "SECURITY.md", "THIRD_PARTY_NOTICES.md"):
        copy(root / name, docs / name)
    for name in ("INSTALLING.md", "VALIDATION.md"):
        copy(root / "docs" / name, docs / name)
    # Include committed repository documentation, never ignored local test logs.
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode().split("\0")
    for name in tracked:
        if name.startswith("docs/"):
            copy(root / name, docs / name)
    copy(root / "overlay/vendor/libtesla/LICENSE", docs / "overlay/vendor/libtesla/LICENSE")
    copy(root / "LICENSE", docs / "licenses/CloudTuneNX-LICENSE")
    copy(root / "THIRD_PARTY_NOTICES.md", docs / "licenses/THIRD_PARTY_NOTICES.md")
    copy(root / "background/THIRD_PARTY_NOTICES.md", docs / "licenses/background-THIRD_PARTY_NOTICES.md")
    for name in tracked:
        if name.startswith(("vendor/", "resources/", "background/vendor/", "overlay/vendor/")):
            if any(word in pathlib.Path(name).name.upper() for word in ("LICENSE", "COPYING", "NOTICE")):
                copy(root / name, docs / "licenses" / name)
    for name in ("vendor/qrcodegen/qrcodegen.hpp", "background/vendor/dr_mp3.h"):
        copy(root / name, docs / "licenses" / name)
    # Rebuildable third-party sources accompany the binary assets. No runtime
    # directory, generated key or account file participates in this whitelist.
    for name, source in source_paths.items():
        copy(source, docs / "third-party-source" / name)
    copy(root / verifier.FFMPEG_MANIFEST, docs / verifier.FFMPEG_MANIFEST)
    for name in verifier.SHAIRPORT_FILES:
        copy(root / name, docs / name)
        copy(root / name, docs / "licenses" / name)
    for name, content in codec_licenses.items():
        path = docs / "licenses/ffmpeg" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        copy(root / "docs/licenses" / f"FFmpeg-{name}", docs / "docs/licenses" / f"FFmpeg-{name}")
    (docs / "third-party-source/README.md").write_text(
        "# Corresponding third-party source\n\n"
        f"CloudTune NX {version}; source commit `{commit}`.\n\n"
        f"- [Complete project source at this commit](https://github.com/tiansongyu/cloudtune-nx/tree/{commit})\n"
        f"- [Download the project source](https://github.com/tiansongyu/cloudtune-nx/archive/{commit}.tar.gz)\n"
        "- The unmodified FFmpeg release archive, exact version/SHA256 manifest, "
        "and ALAC-only build recipe are included in this directory.\n"
        "- From this directory, run `bash scripts/setup-airplay-codec.sh host` "
        "or `bash scripts/setup-airplay-codec.sh switch` with devkitA64/libnx installed. "
        "The recipe uses the bundled archive without downloading it; host builds need "
        "a C compiler, GNU Make, Python 3 and tar/xz. The main project Dockerfile "
        "and build instructions are available in the project source and packaged docs.\n"
        "- Upstream FFmpeg licenses are in `../licenses/ffmpeg/` and inside the archive.\n"
        "- Shairport's retained RSA protocol-material header and upstream notice are "
        "in `airplay/vendor/` and `../licenses/airplay/vendor/`. The published "
        "compatibility key is protocol source material, not an account credential.\n"
        "- The project's GPL license, third-party notices and the remaining vendored "
        "licenses are included in the parent documentation directory.\n",
        encoding="utf-8")

    epoch = int(git("show", "-s", "--format=%ct", "HEAD"))
    archives = {}
    for kind, source in (("sd", sd), ("symbols", symbols), ("docs", docs)):
        target = output / names[kind]
        members = archive(source, target, prefix="symbols/" if kind == "symbols" else "", epoch=epoch)
        archives[target.name] = {"kind": kind, **verifier.file_record(target.read_bytes()),
                                 "files": members}
    require(git("rev-parse", "HEAD") == commit, "Git commit changed during packaging")
    dirty = dirty or bool(git("status", "--porcelain"))
    if release:
        require(not dirty, "Source files changed while packaging; release aborted")
    manifest = {
        "format_version": 1, "product": "CloudTune NX", "version": version, "release": release,
        "packaged_utc": stamp, "git_commit": commit,
        "source_has_uncommitted_changes": dirty, "background_title_id": title,
        "build_provenance": provenance, "components": components, "archives": archives,
        "third_party_sources": {"ffmpeg": codec_source},
    }
    (output / manifest_name).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    sums = [verifier.digest((output / name).read_bytes()) + "  " + name
            for name in sorted(list(names.values()) + [manifest_name])]
    (output / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    verifier.verify_package(output, version, commit, require_release=release)
    if release:
        # One same-filesystem rename publishes a complete five-file directory.
        require(not (dist / "release").exists(), "Refusing to replace existing release assets")
        output.rename(dist / "release")
        print(f"Release assets verified and published: {dist / 'release'}")
    else:
        for source in sorted(output.iterdir()):
            destination = dist / (f"{prefix}-SHA256SUMS" if source.name == "SHA256SUMS" else source.name)
            source.rename(destination)
        print(f"Development assets verified: {dist / (prefix + '-sd.zip')}")
    print(f"Isolated staging retained for inspection: {stage}")
except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError, zipfile.BadZipFile,
        tarfile.TarError) as error:
    print(f"Packaging failed: {error}", file=sys.stderr)
    sys.exit(1)
PY
