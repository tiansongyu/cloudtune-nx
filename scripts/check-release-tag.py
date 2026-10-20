#!/usr/bin/env python3
"""Fail closed when a release tag and its source/binary versions disagree."""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERSION = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"


def check(tag, cmake_path, overlay_path, nacp_paths):
    if not re.fullmatch(r"v" + VERSION, tag):
        raise ValueError("release tag must be exactly vX.Y.Z without leading zeros")
    version = tag[1:]
    cmake = cmake_path.read_text(encoding="utf-8")
    versions = re.findall(
        r"^\s*project\s*\(\s*CloudTuneNX\s+VERSION\s+(" + VERSION + r")(?=\s|\))",
        cmake,
        re.MULTILINE,
    )
    if versions != [version]:
        raise ValueError(f"CMake project version {versions!r} does not match {tag}")
    # Recognize the one-source version expression without executing Make/shell
    # code. The compiled NACP checks below establish the actual binary version.
    overlay = overlay_path.read_text(encoding="utf-8")
    matches = re.findall(r"^APP_VERSION[ \t]*:=[ \t]*(.+)$", overlay, re.MULTILINE)
    from_cmake = (
        "$(shell sed -n '/^project/s/.*VERSION //p' "
        "$(OVERLAY_ROOT)/../CMakeLists.txt | cut -d' ' -f1)"
    )
    if matches not in ([version], [from_cmake]):
        raise ValueError(f"default overlay APP_VERSION does not match {tag}")
    for path in nacp_paths:
        data = path.read_bytes()
        if len(data) != 0x4000:
            raise ValueError(f"{path}: expected a 0x4000-byte NACP")
        title = data[:0x200].split(b"\0", 1)[0].decode("utf-8")
        binary_version = data[0x3060:0x3070].split(b"\0", 1)[0].decode("utf-8")
        if title != "CloudTune NX":
            raise ValueError(f"{path}: unexpected NACP title {title!r}")
        if binary_version != version:
            raise ValueError(f"{path}: NACP version {binary_version!r} does not match {tag}")
    return version


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", help="exact release tag, e.g. v0.1.0")
    parser.add_argument("--cmake", type=pathlib.Path, default=ROOT / "CMakeLists.txt")
    parser.add_argument("--overlay", type=pathlib.Path, default=ROOT / "overlay/Makefile")
    parser.add_argument("--nacp", type=pathlib.Path, action="append", default=[])
    args = parser.parse_args()
    try:
        print(check(args.tag, args.cmake, args.overlay, args.nacp))
    except (OSError, UnicodeError, ValueError) as error:
        print(f"Release validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
