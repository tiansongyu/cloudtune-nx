#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generated FLAC fixtures: PCM fidelity, mixed playback, TLS and account barriers.

Usage: flac_tests.py DECODER_PROBE ENGINE_PROBE PLAYBACK_SMOKE AIRPLAY_HANDOFF
No account, user media, audio hardware, external service or display is required.
"""
import functools
import http.server
import math
import os
from pathlib import Path
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave


def run(*args, timeout=40):
    subprocess.run([str(value) for value in args], check=True, timeout=timeout,
                   env=dict(os.environ, SDL_AUDIODRIVER="dummy"))


def encode(*args):
    run("ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", *args)


def fixture(directory, rate, channels, bits):
    name = f"{bits}bit-{rate}-{channels}ch"
    wav = directory / f"{name}.wav"
    raw = directory / f"{name}.s16"
    flac = directory / f"{name}.flac"
    data = bytearray()
    expected = bytearray()
    scale = (1 << (bits - 1)) - 1
    for frame in range(rate * 8):
        for channel in range(channels):
            value = int(scale * (0.42 * math.sin(frame * 2 * math.pi * 440 / rate + channel)
                                 + 0.13 * math.sin(frame * 2 * math.pi * 1103 / rate)))
            data.extend(value.to_bytes(bits // 8, "little", signed=True))
            expected.extend(struct.pack("<h", value >> (bits - 16)))
    with wave.open(str(wav), "wb") as output:
        output.setparams((channels, bits // 8, rate, 0, "NONE", "not compressed"))
        output.writeframes(data)
    raw.write_bytes(expected)
    layout = {4: "quad", 6: "5.1", 8: "7.1"}.get(channels)
    encode("-i", wav, *(("-channel_layout", layout) if layout else ()),
           "-c:a", "flac", flac)
    return flac, raw, channels, rate


class SlowHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def copyfile(self, source, destination):
        length = os.fstat(source.fileno()).st_size
        # Approximately six seconds to finish any fixture. The first audible
        # PCM must arrive while its cache still lacks a completion marker.
        chunk_size = max(4096, math.ceil(length / 80))
        while chunk := source.read(chunk_size):
            try:
                destination.write(chunk)
                destination.flush()
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                return
            time.sleep(0.075)


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    decoder, engine, smoke, handoff = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="ctnx-flac-") as temporary:
        root = Path(temporary)
        fixtures = [fixture(root, 44100, 1, 16), fixture(root, 48000, 2, 16),
                    fixture(root, 96000, 2, 24), fixture(root, 192000, 2, 24)]
        for flac, raw, channels, rate in fixtures:
            run(decoder, flac, raw, channels, rate, "good")
        source, reference, channels, rate = fixtures[1]
        unknown = root / "unknown-frame-count.flac"
        unknown_content = bytearray(source.read_bytes())
        # FLAC STREAMINFO's optional 36-bit total samples field may be zero.
        properties = int.from_bytes(unknown_content[18:26], "big") & ~((1 << 36) - 1)
        unknown_content[18:26] = properties.to_bytes(8, "big")
        unknown.write_bytes(unknown_content)
        run(decoder, unknown, reference, channels, rate, "unknown")
        renamed = root / "flac-in-a-mp3-filename.mp3"
        renamed.write_bytes(source.read_bytes())
        run(decoder, renamed, reference, channels, rate, "good")
        tagged = root / "id3-prefixed.data"
        tagged.write_bytes(b"ID3\x04\x00\x00\x00\x00\x00\x10" + bytes(16) + source.read_bytes())
        run(decoder, tagged, reference, channels, rate, "good")
        ogg = root / "flac-in-ogg.data"
        encode("-i", source, "-c:a", "copy", "-f", "ogg", ogg)
        run(decoder, ogg, reference, channels, rate, "good")
        damaged = root / "truncated.flac"
        content = source.read_bytes()
        damaged.write_bytes(content[:len(content) // 4])
        run(decoder, damaged, reference, channels, rate, "truncated")
        invalid = root / "invalid.flac"
        invalid.write_bytes(b"fLaCthis is not a FLAC stream")
        run(decoder, invalid, reference, channels, rate, "reject")
        for count, left, right in [
                (4, "c0+0.70710678*c2", "c1+0.70710678*c3"),
                (6, "c0+0.70710678*c2+0.5*c3+0.70710678*c4",
                    "c1+0.70710678*c2+0.5*c3+0.70710678*c5"),
                (8, "c0+0.70710678*c2+0.5*c3+0.70710678*c4+0.70710678*c6",
                    "c1+0.70710678*c2+0.5*c3+0.70710678*c5+0.70710678*c7")]:
            surround, _, _, _ = fixture(root, 48000, count, 16)
            mixed = root / f"downmix-{count}.s16"
            # Independent FFmpeg pan implementation supplies the downmix oracle.
            encode("-i", surround, "-af", f"pan=stereo|c0<{left}|c1<{right}",
                   "-c:a", "pcm_s16le", "-f", "s16le", mixed)
            run(decoder, surround, mixed, 2, 48000, "downmix")
        mp3 = root / "mp3-in-a-flac-filename.flac"
        encode("-i", source, "-c:a", "libmp3lame", "-b:a", "128k", "-f", "mp3", mp3)
        run(smoke, root / "mixed", renamed, "", mp3, timeout=50)
        run(handoff, root / "handoff", fixtures[2][0], timeout=30)
        adts = root / "aac-in-a-mp3-filename.mp3"
        mono_adts = root / "mono-aac-in-a-flac-filename.flac"
        m4a = root / "m4a-in-a-flac-filename.flac"
        tail_m4a = root / "m4a-tail-moov.data"
        encode("-i", source, "-c:a", "aac", "-b:a", "256k", "-f", "adts", adts)
        encode("-i", source, "-ar", "44100", "-ac", "1", "-c:a", "aac",
               "-b:a", "128k", "-f", "adts", mono_adts)
        encode("-i", source, "-c:a", "aac", "-b:a", "256k",
               "-movflags", "+faststart", "-f", "ipod", m4a)
        encode("-i", source, "-c:a", "aac", "-b:a", "256k", "-f", "ipod", tail_m4a)
        for index, aac in enumerate((adts, mono_adts, m4a, tail_m4a)):
            run(smoke, root / f"aac-mixed-{index}", aac, "", renamed, timeout=50)
        run(handoff, root / "aac-handoff", m4a, timeout=30)

        cert, key = root / "cert.pem", root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-days", "1", "-keyout", str(key), "-out", str(cert),
                        "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=20)
        server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), functools.partial(SlowHandler, directory=root))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            url = f"https://localhost:{server.server_port}/{renamed.name}"
            run(engine, root / "engine", renamed, damaged, url, cert, timeout=60)
            url = f"https://localhost:{server.server_port}/{m4a.name}"
            run(engine, root / "aac-engine", m4a, damaged, url, cert, timeout=60)
            url = f"https://localhost:{server.server_port}/{adts.name}"
            run(engine, root / "adts-engine", adts, damaged, url, cert, timeout=60)
        finally:
            server.shutdown()
            server.server_close()
            worker.join()
        print("PASS FLAC: 16/24-bit PCM, mono/stereo 44.1/48/96/192 kHz, byte detection, "
              "ID3/Ogg, seeking, MP3 transitions, progressive TLS, offline cache, "
              "quad/5.1/7.1 downmix, ADTS/M4A AAC transitions, truncation, "
              "AirPlay and account switching barriers")


if __name__ == "__main__":
    main()
