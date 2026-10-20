#!/usr/bin/env python3
"""Check the GUI player's account boundary using only synthetic audio."""
import os
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="cloudtune-account-player-") as temporary:
    root = pathlib.Path(temporary)
    music = root / "generated.mp3"
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                    "-i", "sine=frequency=440:duration=8", "-ar", "44100", "-ac", "2",
                    "-codec:a", "libmp3lame", "-b:a", "128k", str(music)], check=True)
    subprocess.run([sys.argv[1], str(root / "profile"), str(music)], check=True,
                   env={**os.environ, "SDL_AUDIODRIVER": "dummy"}, timeout=20)
