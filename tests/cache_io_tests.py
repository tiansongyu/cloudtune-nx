#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exclusive SD handle semantics and I/O faults with real progressive codecs."""
import functools
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time


class SlowHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def copyfile(self, source, output):
        data = source.read()
        step = max(4096, len(data) // 60)
        try:
            for start in range(0, len(data), step):
                output.write(data[start:start + step])
                output.flush()
                time.sleep(0.05)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass


def main():
    with tempfile.TemporaryDirectory(prefix="cloudtune-cache-") as directory:
        root = Path(directory)
        for extension, options in (
                ("flac", ["-sample_fmt", "s32", "-c:a", "flac"]),
                ("mp3", ["-c:a", "libmp3lame", "-b:a", "320k"]),
                ("aac", ["-c:a", "aac", "-b:a", "256k", "-f", "adts"]),
                ("m4a", ["-c:a", "aac", "-b:a", "256k", "-movflags", "+faststart"])):
            subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                            "-i", "sine=frequency=440:duration=30", "-ar",
                            "96000" if extension == "flac" else "48000", "-ac", "2",
                            *options, str(root / ("source." + extension))], check=True, timeout=15)
        (root / "invalid").write_bytes(b"fLaC" + bytes(65536))
        cert, key = root / "cert.pem", root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-keyout", str(key), "-out", str(cert), "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
                    functools.partial(SlowHandler, directory=root))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        scenarios = [("stream", "source." + ext) for ext in ("flac", "mp3", "aac", "m4a")]
        scenarios += [(mode, "source.flac") for mode in ("open", "read", "write", "flush", "seek", "sync", "cancel", "corrupt-cache")]
        scenarios += [("invalid", "invalid")]
        if len(sys.argv) > 2:
            scenarios = [item for item in scenarios if item[0] == sys.argv[2]][:1]
        try:
            for index, (mode, source) in enumerate(scenarios):
                subprocess.run([sys.argv[1], str(root / f"profile-{index}"),
                                f"https://localhost:{server.server_port}/{source}",
                                str(cert), str(root / source), mode], check=True, timeout=20,
                               env=dict(os.environ, SDL_AUDIODRIVER="dummy", no_proxy="localhost,127.0.0.1"))
        finally:
            server.shutdown()
            server.server_close()
            worker.join()


if __name__ == "__main__":
    main()
