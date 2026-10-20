#!/usr/bin/env python3
"""Verify release archives without extracting or executing their contents."""
import argparse
import hashlib
import io
import json
import pathlib
import re
import stat
import struct
import sys
import tarfile
import zipfile

TITLE_ID = "42000000004E434D"
SD_FILES = {
    "switch/CloudTune/CloudTune.nro",
    "switch/.overlays/CloudTune.ovl",
    f"atmosphere/contents/{TITLE_ID}/exefs.nsp",
    f"atmosphere/contents/{TITLE_ID}/toolbox.json",
    f"atmosphere/contents/{TITLE_ID}/flags/boot2.flag",
    "config/netease-switch/cacert.pem",
}
COMPONENT_PATHS = {
    "app": ("switch/CloudTune/CloudTune.nro", "symbols/app/CloudTune.elf"),
    "background": (f"atmosphere/contents/{TITLE_ID}/exefs.nsp",
                   "symbols/background/cloudtune-background.elf"),
    "overlay": ("switch/.overlays/CloudTune.ovl", "symbols/overlay/CloudTune.elf"),
}
FORBIDDEN_PARTS = {".runtime", ".git", "cookies.txt", "cookies.txt.bak", "settings.json",
                   "queue.nsq", "queue.nsq.bak", "userdata", "credentials", ".env"}
FFMPEG_MANIFEST = "airplay/vendor/ffmpeg-source.json"
FFMPEG_LICENSES = ("COPYING.LGPLv2.1", "COPYING.LGPLv3", "LICENSE.md")
SHAIRPORT_FILES = ("airplay/vendor/SHAIRPORT-LICENSE", "airplay/vendor/shairport_rsa_key.h")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_record(data):
    return {"bytes": len(data), "sha256": digest(data)}


def ffmpeg_source(data):
    source = json.loads(data)
    require(source.get("format_version") == 1 and source.get("name") == "FFmpeg",
            "Invalid FFmpeg source manifest")
    require(re.fullmatch(r"\d+\.\d+\.\d+", source.get("version", "")), "Invalid FFmpeg version")
    require(source.get("archive") == f"ffmpeg-{source['version']}.tar.xz",
            "Invalid FFmpeg source archive name")
    require(source.get("url") == f"https://ffmpeg.org/releases/{source['archive']}",
            "FFmpeg source must use its official release URL")
    require(re.fullmatch(r"[0-9a-f]{64}", source.get("sha256", "")),
            "Invalid FFmpeg source checksum")
    require(re.fullmatch(r"[A-F0-9]{40}", source.get("signing_key_fingerprint", "")),
            "Invalid FFmpeg release signing fingerprint")
    require(source.get("license_files") == list(FFMPEG_LICENSES),
            "Missing FFmpeg upstream license files")
    return source


def ffmpeg_licenses(archive, source):
    require(len(archive) <= 64 * 1024 * 1024 and digest(archive) == source["sha256"],
            "FFmpeg corresponding-source archive does not match its pinned SHA256")
    licenses = {}
    # Read only named regular license members. Never extract or execute the
    # third-party tarball; its full contents remain intact in docs.zip.
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:xz") as tar:
        for name in FFMPEG_LICENSES:
            member = tar.getmember(f"ffmpeg-{source['version']}/{name}")
            require(member.isfile() and member.size <= 128 * 1024,
                    f"Invalid FFmpeg license member: {name}")
            licenses[name] = tar.extractfile(member).read()
    return licenses


def validate_third_party_inputs(root):
    root = pathlib.Path(root)
    manifest_path = root / FFMPEG_MANIFEST
    require(manifest_path.is_file() and not manifest_path.is_symlink(),
            "Missing FFmpeg source manifest")
    source = ffmpeg_source(manifest_path.read_bytes())
    paths = {
        FFMPEG_MANIFEST: manifest_path,
        "scripts/setup-airplay-codec.sh": root / "scripts/setup-airplay-codec.sh",
        source["archive"]: root / ".deps/downloads" / source["archive"],
        **{name: root / name for name in SHAIRPORT_FILES},
    }
    for name, path in paths.items():
        require(path.is_file() and not path.is_symlink(), f"Missing/unsafe source input: {name}")
    licenses = ffmpeg_licenses(paths[source["archive"]].read_bytes(), source)
    for name, expected in licenses.items():
        require((root / "docs/licenses" / f"FFmpeg-{name}").read_bytes() == expected,
                f"Repository FFmpeg license differs from pinned source: {name}")
    header = paths[SHAIRPORT_FILES[1]].read_bytes()
    require(b"Published RAOP compatibility key" in header and
            b"not a CloudTune user credential" in header and
            b"Permission is hereby granted" in header,
            "Missing Shairport public protocol-material copyright/permission notice")
    return source, paths, licenses


def verify_third_party_docs(archive, files):
    source = ffmpeg_source(archive.read(FFMPEG_MANIFEST))
    prefix = "third-party-source/"
    required = {
        prefix + "README.md",
        prefix + FFMPEG_MANIFEST,
        prefix + "scripts/setup-airplay-codec.sh",
        prefix + source["archive"],
        *(prefix + name for name in SHAIRPORT_FILES),
    }
    require({name for name in files if name.startswith(prefix)} == required,
            "Incomplete or unexpected third-party corresponding-source layout")
    require(archive.read(prefix + FFMPEG_MANIFEST) == archive.read(FFMPEG_MANIFEST),
            "Packaged FFmpeg source-manifest copies differ")
    licenses = ffmpeg_licenses(archive.read(prefix + source["archive"]), source)
    for name, expected in licenses.items():
        require(archive.read(f"licenses/ffmpeg/{name}") == expected and
                archive.read(f"docs/licenses/FFmpeg-{name}") == expected,
                f"Packaged FFmpeg license differs from pinned source: {name}")
    recipe = archive.read(prefix + "scripts/setup-airplay-codec.sh")
    require(b"airplay/vendor/ffmpeg-source.json" in recipe and
            b"--enable-decoder=alac" in recipe,
            "Missing ALAC build recipe or source manifest integration")
    for name in SHAIRPORT_FILES:
        require(archive.read(name) == archive.read(prefix + name) ==
                archive.read("licenses/" + name),
                f"Packaged Shairport source/notice copies differ: {name}")
    header = archive.read(SHAIRPORT_FILES[1])
    require(b"Published RAOP compatibility key" in header and
            b"not a CloudTune user credential" in header and
            b"Permission is hereby granted" in header,
            "Missing Shairport protocol-material notice")
    return source


def unpack(fmt, data, offset):
    require(offset >= 0 and offset + struct.calcsize(fmt) <= len(data),
            "Truncated binary structure")
    return struct.unpack_from(fmt, data, offset)


def elf_build_id(data):
    require(data[:6] == b"\x7fELF\x02\x01", "Expected ELF64 little endian")
    require(unpack("<H", data, 18)[0] == 183, "Expected AArch64 ELF")
    table = unpack("<Q", data, 40)[0]
    entry_size, count = unpack("<HH", data, 58)
    require(entry_size >= 64 and count > 0, "ELF lacks section table")
    require(table + entry_size * count <= len(data), "Truncated ELF section table")
    for index in range(count):
        section = table + index * entry_size
        if unpack("<I", data, section + 4)[0] != 7:  # SHT_NOTE
            continue
        offset, size = unpack("<QQ", data, section + 24)
        end = offset + size
        require(end <= len(data), "Truncated ELF note")
        while offset + 12 <= end:
            namesz, descsz, kind = unpack("<III", data, offset)
            offset += 12
            name = data[offset:offset + namesz]
            offset += (namesz + 3) & ~3
            desc = data[offset:offset + descsz]
            offset += (descsz + 3) & ~3
            require(offset <= end, "Truncated ELF note payload")
            if name.rstrip(b"\0") == b"GNU" and kind == 3:
                require(0 < len(desc) <= 32, "Unsupported GNU build ID size")
                return desc
    raise ValueError("ELF lacks GNU build ID")


def nro_version(data):
    require(data[16:20] == b"NRO0", "Invalid NRO/overlay header")
    asset = unpack("<I", data, 24)[0]
    magic, _, _, _, offset, size, _, _ = unpack("<4sIQQQQQQ", data, asset)
    require(magic == b"ASET" and size == 0x4000, "NRO lacks a complete NACP")
    require(offset >= 56 and asset + offset + size <= len(data), "Invalid NACP extent")
    version = data[asset + offset + 0x3060:asset + offset + 0x3070]
    require(b"\0" in version, "Unterminated NACP display version")
    return version.split(b"\0", 1)[0].decode("ascii")


def background_nso(package):
    require(package[:4] == b"PFS0", "Background is not an ExeFS PFS0 package")
    count, strings_size = unpack("<II", package, 4)
    require(0 < count <= 16, "Invalid PFS0 entry count")
    strings = 16 + count * 24
    data_start = strings + strings_size
    require(data_start <= len(package), "Truncated PFS0 table")
    entries = {}
    for index in range(count):
        offset, size, name_offset = unpack("<QQI", package, 16 + index * 24)
        require(name_offset < strings_size, "Invalid PFS0 filename offset")
        raw_name = package[strings + name_offset:data_start]
        require(b"\0" in raw_name, "Unterminated PFS0 filename")
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        require(name not in entries and "/" not in name, "Invalid/duplicate ExeFS name")
        require(data_start + offset + size <= len(package), "Truncated PFS0 file")
        entries[name] = package[data_start + offset:data_start + offset + size]
    require(set(entries) == {"main", "main.npdm"}, "Unexpected ExeFS contents")
    require(entries["main"][:4] == b"NSO0", "Background main is not an NSO")
    npdm = entries["main.npdm"]
    require(npdm[:4] == b"META", "Invalid NPDM header")
    aci = unpack("<I", npdm, 0x70)[0]
    acid = unpack("<I", npdm, 0x78)[0]
    require(npdm[aci:aci + 4] == b"ACI0" and npdm[acid + 0x200:acid + 0x204] == b"ACID",
            "Invalid NPDM access-control tables")
    program = unpack("<Q", npdm, aci + 0x10)[0]
    lo, hi = unpack("<QQ", npdm, acid + 0x210)
    require(program == lo == hi == int(TITLE_ID, 16), "Incorrect background program ID")
    return entries["main"]


def validate_components(binaries, symbols, version):
    result = {}
    for component in COMPONENT_PATHS:
        binary, elf = binaries[component], symbols[component]
        build_id = elf_build_id(elf)
        image = background_nso(binary) if component == "background" else binary
        if component != "background":
            require(nro_version(binary) == version,
                    f"{component} NACP version does not match {version}")
        require(image[64:96] == build_id.ljust(32, b"\0"),
                f"{component} ELF build ID does not match the packaged binary")
        result[component] = {"build_id": build_id.hex(), "version": version,
                             "binary_sha256": digest(binary), "elf_sha256": digest(elf)}
    return result


def validate_build(build_dir, background_dir, overlay_dir, version):
    build, background, overlay = map(pathlib.Path, (build_dir, background_dir, overlay_dir))
    paths = {
        "app": (build / "CloudTune.nro", build / "CloudTune.elf"),
        "background": (background / "cloudtune-background.nsp",
                       background / "cloudtune-background.elf"),
        "overlay": (overlay / "CloudTune.ovl", overlay / "CloudTune.elf"),
    }
    binaries = {key: pair[0].read_bytes() for key, pair in paths.items()}
    symbols = {key: pair[1].read_bytes() for key, pair in paths.items()}
    return paths, validate_components(binaries, symbols, version)


def zip_records(path):
    files = {}
    directories = set()
    with zipfile.ZipFile(path) as archive:
        seen = set()
        require(sum(i.file_size for i in archive.infolist()) <= 512 * 1024 * 1024,
                f"Archive expands beyond 512 MiB: {path.name}")
        for item in archive.infolist():
            name = item.filename
            pure = pathlib.PurePosixPath(name)
            require(name not in seen, f"Duplicate ZIP entry: {name}")
            seen.add(name)
            require(name and not pure.is_absolute() and "\\" not in name
                    and all(part not in ("", ".", "..") for part in name.rstrip("/").split("/")),
                    f"Unsafe ZIP path: {name}")
            require(not any(part.lower() in FORBIDDEN_PARTS for part in pure.parts),
                    f"Runtime/login file in archive: {name}")
            require(not name.lower().endswith((".log", ".sqlite", ".sqlite3", ".db")),
                    f"Private runtime/log file in archive: {name}")
            mode = item.external_attr >> 16
            require(not stat.S_ISLNK(mode), f"ZIP symlink is not allowed: {name}")
            require(not item.flag_bits & 1, f"Encrypted ZIP entry: {name}")
            if item.is_dir():
                directories.add(name.rstrip("/"))
            else:
                # read() verifies each member's CRC, including empty files.
                files[name] = file_record(archive.read(item))
        bad = archive.testzip()
        require(bad is None, f"ZIP CRC mismatch: {bad}")
    return files, directories


def verify_package(directory, version=None, commit=None, require_release=True):
    directory = pathlib.Path(directory)
    manifests = list(directory.glob("*-manifest.json"))
    require(len(manifests) == 1, "Expected exactly one release manifest")
    manifest_path = manifests[0]
    manifest = json.loads(manifest_path.read_text())
    require(manifest.get("format_version") == 1 and manifest.get("product") == "CloudTune NX",
            "Unsupported release manifest")
    actual_version = manifest.get("version", "")
    require(re.fullmatch(r"\d+\.\d+\.\d+", actual_version), "Invalid manifest version")
    require(version is None or version == actual_version, "Unexpected release version")
    actual_commit = manifest.get("git_commit", "")
    require(re.fullmatch(r"[0-9a-f]{40}", actual_commit), "Invalid release commit")
    require(commit is None or commit == actual_commit, "Unexpected release commit")
    require(type(manifest.get("source_has_uncommitted_changes")) is bool, "Missing dirty state")
    if require_release:
        require(manifest.get("release") is True, "Expected a release-mode package")
    if manifest.get("release"):
        require(manifest["source_has_uncommitted_changes"] is False, "Release source was dirty")
    prefix = manifest_path.name.removesuffix("-manifest.json")
    if manifest.get("release"):
        require(prefix == f"CloudTuneNX-{actual_version}", "Incorrect release asset names")
    else:
        require(re.fullmatch(rf"CloudTuneNX-{re.escape(actual_version)}-\d{{8}}T\d{{6}}Z", prefix),
                "Incorrect local package names")
    names = {kind: f"{prefix}-{kind}.zip" for kind in ("sd", "symbols", "docs")}
    expected_assets = set(names.values()) | {manifest_path.name, "SHA256SUMS"}
    require({p.name for p in directory.iterdir()} == expected_assets,
            "Release directory must contain exactly three ZIPs, manifest and SHA256SUMS")
    require(all(p.is_file() and not p.is_symlink() for p in directory.iterdir()),
            "Release assets must be regular files")
    sums = {}
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/\\]+)", line)
        require(match is not None, "Invalid SHA256SUMS line")
        sha, name = match.groups()
        require(name not in sums, "Duplicate SHA256SUMS entry")
        sums[name] = sha
    require(set(sums) == expected_assets - {"SHA256SUMS"}, "Incomplete top-level checksums")
    for name, sha in sums.items():
        require(digest((directory / name).read_bytes()) == sha, f"Asset hash mismatch: {name}")
    require(set(manifest.get("archives", {})) == set(names.values()), "Manifest archive mismatch")
    archive_files = {}
    for kind, name in names.items():
        path = directory / name
        record = manifest["archives"][name]
        require(record.get("kind") == kind, "Manifest archive kind mismatch")
        require(record.get("bytes") == path.stat().st_size
                and record.get("sha256") == sums[name], f"Manifest asset hash mismatch: {name}")
        files, directories = zip_records(path)
        require(files == record.get("files"), f"Manifest member hashes differ: {name}")
        archive_files[kind] = files
        if kind == "sd":
            require(set(files) == SD_FILES, "Incorrect SD layout or unexpected SD files")
            ancestors = {str(parent) for file in SD_FILES
                         for parent in pathlib.PurePosixPath(file).parents if str(parent) != "."}
            require(directories == ancestors, "Incorrect SD directories; hidden .overlays required")
        elif kind == "symbols":
            required = {entry[1] for entry in COMPONENT_PATHS.values()}
            optional = {"symbols/app/CloudTune.map", "symbols/overlay/CloudTune.map",
                        "symbols/toolchain-packages.txt"}
            require(required <= set(files) and set(files) <= required | optional,
                    "Incorrect symbols archive layout")
        else:
            required = {"README.md", "INSTALLING.md", "VALIDATION.md", "LICENSE",
                        "CONTRIBUTING.md", "SECURITY.md", "THIRD_PARTY_NOTICES.md",
                        "overlay/vendor/libtesla/LICENSE", FFMPEG_MANIFEST,
                        *SHAIRPORT_FILES}
            require(required <= set(files), "Missing release documentation or license")
            require(all(name in required or name.startswith(("docs/", "licenses/", "third-party-source/"))
                        for name in files), "Unexpected documentation archive file")
            with zipfile.ZipFile(path) as docs:
                source = verify_third_party_docs(docs, files)
            require(manifest.get("third_party_sources") == {"ffmpeg": source},
                    "Manifest FFmpeg corresponding-source metadata differs")
    binaries, symbols = {}, {}
    with zipfile.ZipFile(directory / names["sd"]) as sd, \
            zipfile.ZipFile(directory / names["symbols"]) as debug:
        for key, (binary_path, elf_path) in COMPONENT_PATHS.items():
            binaries[key], symbols[key] = sd.read(binary_path), debug.read(elf_path)
        require(sd.read(f"atmosphere/contents/{TITLE_ID}/flags/boot2.flag") == b"",
                "boot2.flag must be empty")
        toolbox = json.loads(sd.read(f"atmosphere/contents/{TITLE_ID}/toolbox.json"))
        require(toolbox.get("name") == "CloudTune NX" and toolbox.get("tid") == TITLE_ID,
                "Unexpected toolbox product/program ID")
        ca = sd.read("config/netease-switch/cacert.pem")
        require(b"BEGIN CERTIFICATE" in ca and b"PRIVATE KEY" not in ca, "Invalid CA bundle")
    components = validate_components(binaries, symbols, actual_version)
    require(components == manifest.get("components"), "Manifest component IDs/hashes differ")
    require(manifest.get("background_title_id") == TITLE_ID, "Manifest program ID mismatch")
    if manifest.get("release"):
        provenance = manifest.get("build_provenance", {})
        require(provenance.get("git_commit") == actual_commit
                and provenance.get("source_has_uncommitted_changes") is False,
                "Build provenance does not match clean release commit")
        require(provenance.get("components") == components, "Build provenance components differ")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", help="directory containing the five release assets")
    parser.add_argument("--version", help="expected CMake/tag version, without a v prefix")
    parser.add_argument("--commit", help="expected complete Git commit SHA")
    parser.add_argument("--build-dir", help="validate unarchived Switch build artifacts instead")
    parser.add_argument("--background-dir")
    parser.add_argument("--overlay-dir", default="build/overlay")
    args = parser.parse_args()
    try:
        if args.build_dir:
            require(args.version is not None, "--build-dir requires --version")
            background = args.background_dir or str(pathlib.Path(args.build_dir) / "background")
            _, components = validate_build(args.build_dir, background, args.overlay_dir, args.version)
            print(json.dumps({"version": args.version, "components": components}, indent=2))
        else:
            require(args.directory is not None, "A release directory is required")
            manifest = verify_package(args.directory, args.version, args.commit)
            print(f"Verified CloudTune NX {manifest['version']} at {manifest['git_commit']}: "
                  "5 assets, SHA256, ZIP CRC, exact SD layout, NACP versions, matching ELFs "
                  "and pinned third-party source/licenses.")
    except (OSError, ValueError, KeyError, TypeError, UnicodeError, zipfile.BadZipFile,
            tarfile.TarError) as error:
        print(f"Package verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
