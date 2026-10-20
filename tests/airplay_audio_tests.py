#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate local audio and exercise the sole output owner's source handoff."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="ctnx-pcm-") as temporary:
    root = Path(temporary)
    mp3 = root / "generated.mp3"
    subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                    "-f", "lavfi", "-i", "sine=frequency=330:duration=8",
                    "-ar", "44100", "-ac", "2", "-codec:a", "libmp3lame", str(mp3)],
                   check=True, timeout=20)
    subprocess.run([sys.argv[1], str(root / "data"), str(mp3)], check=True,
                   timeout=30, env=dict(os.environ, SDL_AUDIODRIVER="dummy"))
