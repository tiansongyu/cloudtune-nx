#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="cloudtune-output-") as directory:
    root = Path(directory)
    flac = root / "generated.flac"
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                    "-i", "sine=frequency=440:duration=30", "-ar", "96000", "-ac", "2",
                    "-sample_fmt", "s32", "-c:a", "flac", str(flac)], check=True, timeout=15)
    oversized = root / "oversized-seektable.flac"
    header = bytearray(flac.read_bytes()[:42])
    header[4] &= 0x7F  # STREAMINFO, followed by an oversized final SEEKTABLE.
    length = (8 * 1024 * 1024 // 18) * 18
    with oversized.open("wb") as output:
        output.write(header)
        output.write(bytes([0x83]) + length.to_bytes(3, "big"))
        output.seek(length - 1, 1)
        output.write(b"\0")
    subprocess.run([sys.argv[2], str(oversized), str(flac)], check=True, timeout=15)
    environment = dict(os.environ, SDL_AUDIODRIVER="dummy")
    subprocess.run([sys.argv[1], str(root / "profile"), str(flac)],
                   check=True, timeout=40, env=environment)
