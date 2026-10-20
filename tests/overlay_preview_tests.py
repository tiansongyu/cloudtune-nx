#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the real overlay preview service with local audio and no GUI/account.

Usage: python3 tests/overlay_preview_tests.py --service PATH_TO_EXECUTABLE
Requires ffmpeg unless --fixture supplies an existing MP3. SDL's dummy driver
still consumes the production engine's decoded PCM and advances its audio clock.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

MAGIC = 0x484D434E
VERSION = 1
SHUTDOWN = 0xFFFF0001


class ServiceTest:
    def __init__(self, executable, directory, log):
        self.executable = str(executable)
        self.directory = directory
        self.socket_path = str(directory / "control.sock")
        self.log = log
        self.environment = dict(os.environ, SDL_AUDIODRIVER="dummy")
        self.processes = []
        self.checks = []

    def start(self):
        process = subprocess.Popen(
            [self.executable, "--directory", str(self.directory)],
            env=self.environment,
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )
        self.processes.append(process)
        return process

    def close(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)

    def rpc(self, command=1, argument=0, *, magic=MAGIC, version=VERSION, split=False):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(2)
            client.connect(self.socket_path)
            request = struct.pack("<4I", magic, version, command, argument)
            if split:
                client.sendall(request[:5])
                time.sleep(0.04)
                client.sendall(request[5:])
            else:
                client.sendall(request)
            data = bytearray()
            while len(data) < 220:
                chunk = client.recv(220 - len(data))
                if not chunk:
                    break
                data.extend(chunk)
        if len(data) != 220:
            raise AssertionError(f"Wrong response size: {len(data)}")
        version, state, track, *fields = struct.unpack_from("<IIQ8I", data)
        names = ("position", "duration", "volume", "index", "count", "repeat", "shuffle", "error")
        return dict(
            zip(names, fields),
            version=version, state=state, track=track,
            result=struct.unpack_from("<I", data, 216)[0],
        )

    def wait(self, predicate, reason, seconds=5):
        deadline = time.monotonic() + seconds
        latest = None
        while time.monotonic() < deadline:
            try:
                latest = self.rpc()
                if predicate(latest):
                    return latest
            except (OSError, AssertionError):
                pass
            time.sleep(0.025)
        raise AssertionError(f"{reason}; last status={latest}")

    def check(self, name, condition=True):
        if not condition:
            raise AssertionError(name)
        self.checks.append(name)

    def run(self):
        process = self.start()
        initial = self.wait(
            lambda s: s["state"] == 3 and s["volume"] == 0, "Initial paused preload failed"
        )
        self.check("preload decodes local MP3 with two tracks",
                   initial["track"] == 100 and initial["count"] == 2 and initial["duration"] > 3000)
        time.sleep(0.15)
        self.check("preload remains at zero until play", self.rpc()["position"] == 0)
        self.check("directory permission 0700", stat.S_IMODE(self.directory.stat().st_mode) == 0o700)
        self.check("socket permission 0600", stat.S_IMODE(os.stat(self.socket_path).st_mode) == 0o600)
        self.check("version and ping", self.rpc(0)["version"] == VERSION and self.rpc(2)["result"] == 0)
        self.check("split frame is read completely", self.rpc(split=True)["result"] == 0)
        self.check("bad magic rejected", self.rpc(magic=0)["result"] == 1)
        self.check("bad version rejected", self.rpc(version=2)["result"] == 1)
        self.check("unknown command rejected", self.rpc(9)["result"] == 2)
        self.check("invalid volume rejected", self.rpc(16, 1001)["result"] == 3)
        self.check("invalid repeat rejected", self.rpc(18, 3)["result"] == 3)

        duplicate = subprocess.run(
            [self.executable, "--directory", str(self.directory)],
            env=self.environment, capture_output=True, timeout=3,
        )
        self.check("single instance preserves original socket",
                   duplicate.returncode == 1 and self.rpc()["result"] == 0)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.connect(self.socket_path)
            client.sendall(b"x")
        self.check("reconnect after truncated frame", self.rpc()["result"] == 0)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as slow:
            slow.connect(self.socket_path)
            slow.sendall(b"x")
            started = time.monotonic()
            self.check("partial frame deadline frees service",
                       self.rpc()["result"] == 0 and time.monotonic() - started < 1.7)

        # Send Play then disconnect without reading its reply. The independent
        # service must keep decoding and consuming audio with no GUI connected.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.connect(self.socket_path)
            client.sendall(struct.pack("<4I", MAGIC, VERSION, 12, 0))
        first = self.wait(lambda s: s["state"] == 2 and s["position"] >= 200,
                          "Play did not progress")
        time.sleep(0.2)
        self.check("client exit preserves advancing playback",
                   self.rpc()["position"] > first["position"])
        self.rpc(13)
        paused = self.wait(lambda s: s["state"] == 3, "Pause failed")
        time.sleep(0.15)
        self.check("pause freezes audio clock", self.rpc()["position"] == paused["position"])
        self.rpc(17, 2000)
        self.wait(lambda s: s["position"] == 2000, "Paused seek failed")
        self.rpc(16, 250)
        self.rpc(18, 2)
        self.rpc(19, 1)
        self.rpc(12)
        self.wait(lambda s: s["position"] > 2100 and s["volume"] == 250
                  and s["repeat"] == 2 and s["shuffle"] == 1, "Resume/controls failed")
        self.check("seek volume repeat shuffle resume affect real engine")
        self.rpc(14)
        self.wait(lambda s: s["track"] == 101 and s["state"] == 2 and s["position"] >= 150,
                  "Next track did not decode")
        self.check("next track advances decoded audio")
        self.rpc(15)
        self.wait(lambda s: s["track"] == 100 and s["state"] == 2, "Previous failed")
        self.check("previous track")
        self.rpc(20)
        self.wait(lambda s: s["state"] == 0, "Stop failed")
        self.rpc(10, 1)
        self.wait(lambda s: s["track"] == 101 and s["state"] == 2, "Queue index load failed")
        self.check("stop and queue index load")
        self.rpc(11)
        self.wait(lambda s: s["state"] == 3, "Play/pause failed")
        self.check("play/pause toggles")

        original_queue = (self.directory / "queue.nsq").read_bytes()
        remote_queue = bytearray(original_queue)
        remote_url = b"https://invalid.example/never-requested.mp3"
        remote_queue[16 + 400:16 + 1424] = remote_url.ljust(1024, b"\0")
        try:
            (self.directory / "queue.nsq").write_bytes(remote_queue)
            self.check("nonlocal queue rejected before network use", self.rpc(10)["result"] == 3)
        finally:
            (self.directory / "queue.nsq").write_bytes(original_queue)

        self.check("shutdown acknowledged", self.rpc(SHUTDOWN)["result"] == 0)
        self.check("shutdown cleans socket",
                   process.wait(timeout=5) == 0 and not os.path.exists(self.socket_path))
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stale:
            stale.bind(self.socket_path)
        process = self.start()
        self.wait(lambda s: s["state"] == 3 and s["volume"] == 0, "Stale socket restart failed")
        self.check("stale socket recovered and initial volume reset")
        process.terminate()
        self.check("SIGTERM cleans socket",
                   process.wait(timeout=5) == 0 and not os.path.exists(self.socket_path))
        return self.checks


def make_queue(directory, fixture):
    paths = []
    for index, rate in enumerate((44100, 48000)):
        if fixture:
            path = fixture
        else:
            path = directory / f"generated-{rate}.mp3"
            subprocess.run(
                ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                 f"sine=frequency={330 + index * 110}:duration=8:sample_rate={rate}",
                 "-ac", "2", "-c:a", "libmp3lame", "-b:a", "128k", "-y", str(path)],
                check=True, timeout=20,
            )
        paths.append(path)
    queue = struct.pack("<8sII", b"NCMQUEUE", VERSION, len(paths))
    for index, path in enumerate(paths):
        encoded_path = os.fsencode(path)
        if len(encoded_path) >= 1024:
            raise ValueError("Fixture path exceeds queue URL field")
        queue += struct.pack(
            "<QII128s128s128s1024s", 100 + index, 8000, 0,
            f"Generated fixture {index}".encode(), b"CloudTune test", b"Local only", encoded_path,
        )
    (directory / "queue.nsq").write_bytes(queue)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--service", type=Path, required=True, help="Preview service executable")
    parser.add_argument("--fixture", type=Path, help="Optional local MP3 at least four seconds long")
    args = parser.parse_args()
    service = args.service.resolve()
    fixture = args.fixture.resolve() if args.fixture else None
    if not service.is_file() or not os.access(service, os.X_OK):
        parser.error("--service must name an executable")
    if fixture and not fixture.is_file():
        parser.error("--fixture must name a local MP3")
    if not fixture and not shutil.which("ffmpeg"):
        parser.error("ffmpeg is required unless --fixture supplies a local MP3")
    # Short /tmp path remains below AF_UNIX's 108-byte sun_path bound even if
    # the checkout itself has a long absolute path.
    with tempfile.TemporaryDirectory(prefix="cloudtune-ovl-", dir="/tmp") as temporary:
        directory = Path(temporary)
        make_queue(directory, fixture)
        with tempfile.TemporaryFile(mode="w+") as log:
            test = ServiceTest(service, directory, log)
            try:
                checks = test.run()
                print(json.dumps({"passed": len(checks), "audio_driver": "SDL dummy",
                                  "checks": checks}, indent=2))
            except Exception:
                log.seek(0)
                print(log.read(), end="", file=sys.stderr)
                raise
            finally:
                test.close()


if __name__ == "__main__":
    main()
