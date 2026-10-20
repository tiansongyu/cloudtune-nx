#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded, reproducible decoder mutations; generated audio only, no network."""
import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    environment = {
        **os.environ,
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
    }
    count = 0
    with tempfile.TemporaryDirectory(prefix="cloudtune-codec-boundaries-") as temporary:
        directory = pathlib.Path(temporary)

        def run(path, expectation, label):
            nonlocal count
            result = subprocess.run([binary, str(path), expectation],
                                    capture_output=True, text=True, timeout=5,
                                    env=environment)
            count += 1
            if result.returncode:
                raise RuntimeError(f"{label}: exit {result.returncode}\n"
                                   f"{result.stdout[-2000:]}\n{result.stderr[-6000:]}")

        run(directory, "pcm", "PCM ring capacity, chunk phase, pause and concurrent access")
        fixtures = {
            "mp3": ["-c:a", "libmp3lame", "-b:a", "128k", "-f", "mp3"],
            "mp3-vbr": ["-ar", "44100", "-ac", "1", "-c:a", "libmp3lame",
                        "-q:a", "4", "-f", "mp3"],
            "mp3-no-xing": ["-ar", "44100", "-c:a", "libmp3lame", "-b:a", "128k",
                            "-write_xing", "0", "-f", "mp3"],
            "flac": ["-c:a", "flac", "-f", "flac"],
            "adts": ["-c:a", "aac", "-b:a", "128k", "-f", "adts"],
            "m4a": ["-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart", "-f", "ipod"],
            "tail": ["-c:a", "aac", "-b:a", "128k", "-f", "ipod"],
        }
        for codec, options in fixtures.items():
            original = directory / f"{codec}.bin"
            subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                            "-f", "lavfi", "-i",
                            "sine=frequency=997:sample_rate=48000:duration=0.4", "-ac", "2",
                            *options, str(original)], check=True, timeout=10)
            data = original.read_bytes()
            run(original, "good", f"{codec}: baseline and sample-exact seeks")
            run(original, "cancel", f"{codec}: cancel blocked progressive open")
            cases = []
            cuts = sorted(set(list(range(64)) + [128, 256, 512, 1024, len(data) // 4,
                          len(data) // 2, 3 * len(data) // 4,
                          len(data) - 1, len(data) - 4, len(data) - 16, len(data) - 64]))
            cases.extend((f"cut-{size}", data[:size]) for size in cuts if size < len(data))
            indices = sorted(set(list(range(64)) +
                                 [len(data) * i // 32 for i in range(1, 32)]))
            for index in indices:
                mutated = bytearray(data)
                mutated[index] ^= 255
                cases.append((f"xor-{index}", mutated))
            if codec == "flac":
                for name, start, end in (("maximum-blocks", 8, 12),
                                         ("maximum-streaminfo", 18, 26)):
                    mutated = bytearray(data)
                    mutated[start:end] = bytes([255]) * (end - start)
                    cases.append((name, mutated))
            if codec in ("m4a", "tail"):
                for value in (0, 1, 0xFFFFFFFF):
                    mutated = bytearray(data)
                    mutated[:4] = value.to_bytes(4, "big")
                    cases.append((f"atom-size-{value}", mutated))
                for tag in (b"mdhd", b"stsz", b"stco"):
                    position = data.find(tag)
                    if position >= 0:
                        mutated = bytearray(data)
                        mutated[position + 8:position + 16] = bytes([255]) * 8
                        cases.append((f"extreme-{tag.decode()}", mutated))
            for label, mutated in cases:
                path = directory / "mutation.bin"
                path.write_bytes(mutated)
                expectation = "short" if codec == "mp3" and label == f"cut-{len(data) // 2}" else "any"
                run(path, expectation, f"{codec}: {label}")
            print(f"PASS {codec}: baseline, {len(cases)} fixed truncation/metadata/byte mutations",
                  flush=True)
    print(f"PASS {count} bounded codec/PCM cases; each child limited to 5 seconds")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
