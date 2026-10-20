#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe actual EAPI identity and cache keys with interrupted account publication."""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import urllib.parse

from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from api_transport_tests import Handler, Server, certificate


def jar(token):
    return f"# Netscape HTTP Cookie File\n.music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\t{token}\n"


def registry(token):
    return json.dumps({"version": 1, "active_user_id": 11, "accounts": [
        {"user_id": 11, "nickname": "Synthetic account", "cookies": jar(token)}]})


class PlaybackHandler(Handler):
    def do_POST(self):
        try:
            body = self.rfile.read(int(self.headers["Content-Length"]))
            encoded = urllib.parse.parse_qs(body.decode())["params"][0]
            decryptor = Cipher(algorithms.AES(b"e82ckenh8dichen8"), modes.ECB()).decryptor()
            padded = decryptor.update(bytes.fromhex(encoded)) + decryptor.finalize()
            unpadder = padding.PKCS7(128).unpadder()
            plain = unpadder.update(padded) + unpadder.finalize()
            request = json.loads(plain.decode().split("-36cd479b6b5-")[1])
            token = request["header"].get("MUSIC_U", "")
            self.server.sessions.append(token)
            if len(self.server.sessions) == 1:
                # Emulate a later account save while the original queue remains
                # loaded. No receiver/engine is allowed to write these files.
                (self.server.profile / "accounts.json").write_text(registry("synthetic-bob"))
                (self.server.profile / "cookies.txt").write_text(jar("synthetic-staged-charlie"))
            self.reply(200, json.dumps({"code": 200, "data": [{
                "url": "https://music.163.com/fixture-audio", "time": 3000,
            }]}).encode())
        except Exception:
            self.server.failed = True
            self.reply(500, b"fixture error")

    def do_GET(self):
        self.server.downloads += 1
        self.reply(200, self.server.audio)


def main():
    with tempfile.TemporaryDirectory(prefix="cloudtune-playback-session-") as temporary:
        root = pathlib.Path(temporary)
        profile = root / "profile"
        profile.mkdir()
        music = root / "generated.mp3"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                        "-i", "sine=frequency=440:duration=3", "-ar", "48000", "-ac", "2",
                        "-codec:a", "libmp3lame", "-b:a", "128k", str(music)], check=True)
        context, ca = certificate(root)
        server = Server(("127.0.0.1", 0), PlaybackHandler)
        server.context, server.profile, server.audio = context, profile, music.read_bytes()
        server.sessions, server.downloads, server.failed = [], 0, False
        # libcurl accepts and normalizes legacy boolean/expiry spellings.
        # Cache ownership must match that actual imported authentication jar.
        alice = json.loads(registry("synthetic-alice"))
        alice["accounts"][0]["cookies"] = jar("synthetic-alice").replace(
            "\tTRUE", "\ttrue").replace("\t0\t", "\t0tail\t")
        (profile / "accounts.json").write_text(json.dumps(alice))
        (profile / "cookies.txt").write_text(jar("synthetic-staged-bob"))
        (profile / "settings.json").write_text(json.dumps({
            "quality": "standard", "proxy": f"http://127.0.0.1:{server.server_port}"}))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            subprocess.run([sys.argv[1], str(profile), str(ca)], check=True, timeout=20,
                           env={**os.environ, "SDL_AUDIODRIVER": "dummy",
                                "NO_PROXY": "127.0.0.1,localhost",
                                "no_proxy": "127.0.0.1,localhost"})
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        assert not server.failed, "Fixture could not decode an EAPI request"
        assert server.sessions == ["synthetic-alice", "synthetic-alice", "synthetic-bob"], \
            "Player used an uncommitted mirror or changed identity inside the loaded queue"
        assert server.downloads == 3, "Session-specific cache lookup did not fetch all three tracks"
        for track, token in [(710, "synthetic-alice"), (711, "synthetic-alice"),
                             (710, "synthetic-bob")]:
            digest = hashlib.md5((token + "\nstandard").encode()).hexdigest()
            cached = profile / "cache" / f"{track}-{digest}.audio"
            assert cached.read_bytes() == server.audio and pathlib.Path(str(cached)+".complete").is_file(), \
                "Cache identity differs from the session actually used by the API"
        assert (profile / "accounts.json").read_text() == registry("synthetic-bob"), \
            "Player changed the canonical account archive"
        assert (profile / "cookies.txt").read_text() == jar("synthetic-staged-charlie"), \
            "Player repaired or overwrote the compatibility mirror"
    print("PASS: observed EAPI identity and cache ownership across interrupted account saves")


if __name__ == "__main__":
    main()
