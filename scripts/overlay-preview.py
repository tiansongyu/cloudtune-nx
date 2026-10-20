#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production overlay UI against an independent, local test player."""
import argparse
import array
from contextlib import contextmanager
import fcntl
import json
import math
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import time
import wave

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "overlay-preview"
BIN = BUILD / "overlay" / "preview"
GUI = BIN / "cloudtune-overlay-preview"
SERVICE = BIN / "cloudtune-preview-service"
COMMANDS = {
    "status": 1, "load": 10, "toggle": 11, "play": 12, "pause": 13,
    "next": 14, "previous": 15, "volume": 16, "seek": 17,
    "repeat": 18, "shuffle": 19, "stop": 20,
}


def rpc(directory, command=1, argument=0):
    # Matches overlay/preview/host_protocol.h; both hosts are little endian.
    request = struct.pack("<IIII", 0x484D434E, 1, command, argument)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(2)
        connection.connect(str(directory / "control.sock"))
        connection.sendall(request)
        data = bytearray()
        while len(data) < 220:
            part = connection.recv(220 - len(data))
            if not part:
                raise RuntimeError("Preview service closed an incomplete reply")
            data.extend(part)
    result = struct.unpack_from("<I", data, 216)[0]
    if result:
        raise RuntimeError(f"Preview service rejected command: 0x{result:08x}")
    values = struct.unpack_from("<IIQ8I", data)
    names = ("version", "state", "track_id", "position_ms", "duration_ms",
             "volume_permille", "queue_index", "queue_count", "repeat", "shuffle",
             "error_code")
    status = dict(zip(names, values))
    for name, start, length in (("title", 48, 64), ("artist", 112, 48),
                                ("message", 160, 56)):
        status[name] = bytes(data[start:start + length]).split(b"\0", 1)[0].decode(
            "utf-8", errors="replace")
    return status


def build():
    subprocess.run([str(ROOT / "scripts/setup-airplay-codec.sh"), "host"], check=True)
    subprocess.run(
        ["cmake", "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja",
         "-DPLATFORM_DESKTOP=ON", "-DCLOUDTUNE_BUILD_OVERLAY_PREVIEW=ON",
         "-DCMAKE_BUILD_TYPE=Debug"], check=True)
    subprocess.run(
        ["cmake", "--build", str(BUILD), "--target", "cloudtune-overlay-preview",
         "cloudtune-preview-service", "--parallel", os.environ.get("BUILD_JOBS", "8")],
        check=True)


def fixture(directory):
    """Generate original quiet chimes, then publish a private two-track queue."""
    audio = directory / "audio"
    audio.mkdir(mode=0o700, exist_ok=True)
    rate = 44100
    titles = ("云端漫游 · 试听一", "夜色回声 · 试听二")
    notes = ((261.63, 329.63, 392.00, 523.25),
             (220.00, 293.66, 349.23, 440.00))
    entries = []
    for index, sequence in enumerate(notes):
        mp3 = audio / f"demo-{index + 1}.mp3"
        if not mp3.exists():
            phrase = audio / f"phrase-{index + 1}.wav"
            samples = array.array("h")
            for frame in range(rate * 8):
                position = frame / rate
                within = position % 2.0
                frequency = sequence[min(int(position // 2), 3)]
                envelope = min(within / 0.025, 1) * math.exp(-within * 2.3)
                sample = (math.sin(2 * math.pi * frequency * within) +
                          0.25 * math.sin(4 * math.pi * frequency * within))
                samples.append(int(32767 * 0.09 * envelope * sample))
            if sys.byteorder != "little":
                samples.byteswap()
            with wave.open(str(phrase), "wb") as output:
                output.setparams((1, 2, rate, 0, "NONE", "not compressed"))
                output.writeframes(samples.tobytes())
            subprocess.run(
                ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                 "-stream_loop", "-1", "-i", str(phrase), "-t", "120", "-ac", "2",
                 "-codec:a", "libmp3lame", "-b:a", "128k", str(mp3)], check=True)
            phrase.unlink()
        url = str(mp3).encode()
        if len(url) >= 1024:
            raise RuntimeError("Fixture path exceeds the queue URL capacity")
        entries.append(struct.pack(
            "<QII128s128s128s1024s", 900001 + index, 120000, 0,
            titles[index].encode(), "本地合成演示音频".encode(),
            "CloudTune NX 覆盖层测试".encode(), url))
    temporary = directory / "queue.nsq.tmp"
    temporary.write_bytes(struct.pack("<8sII", b"NCMQUEUE", 1, len(entries)) +
                          b"".join(entries))
    temporary.replace(directory / "queue.nsq")


@contextmanager
def launcher_lock(directory):
    """Serialize process records and starts, without holding a lock over GUI wait."""
    with (directory / ".launcher.lock").open("a") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        yield


def process_start_ticks(pid):
    try:
        # comm can contain spaces and parentheses; fields after its final ')'
        # begin with state (field 3), so starttime (field 22) is index 19.
        fields = (Path("/proc") / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()
    except FileNotFoundError:
        return None
    if fields[0] in ("Z", "X"):
        return None
    return int(fields[19])


def remember_process(directory, name, process):
    start_ticks = process_start_ticks(process.pid)
    if start_ticks is None:
        raise RuntimeError(f"Preview {name} exited before its process could be recorded")
    temporary = directory / f"{name}.json.tmp"
    temporary.write_text(json.dumps(
        {"pid": process.pid, "start_ticks": start_ticks,
         "executable": str(SERVICE if name == "service" else GUI),
         "directory": str(directory)}))
    temporary.replace(directory / f"{name}.json")


def recorded_process(directory, name):
    try:
        record = json.loads((directory / f"{name}.json").read_text())
    except FileNotFoundError:
        return None
    pid = record["pid"]
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        raise RuntimeError(f"Invalid preview {name} PID record")
    start_ticks = process_start_ticks(pid)
    if start_ticks is None:
        return None
    if record.get("start_ticks", start_ticks) != start_ticks:
        raise RuntimeError(f"Refusing to use reused PID {pid}")
    expected = SERVICE if name == "service" else GUI
    proc = Path("/proc") / str(pid)
    try:
        # Rebuilding replaces the file while the old process still executes it.
        # readlink remains valid even when its target carries " (deleted)".
        executable = os.readlink(proc / "exe").removesuffix(" (deleted)")
        if Path(executable) != expected.resolve():
            raise RuntimeError(f"Refusing to use reused PID {pid}")
        environment = (proc / "environ").read_bytes().split(b"\0")
    except FileNotFoundError:
        if process_start_ticks(pid) is None:
            return None
        raise
    marker = f"CLOUDTUNE_PREVIEW_DIRECTORY={directory}".encode()
    if marker not in environment:
        raise RuntimeError(f"PID {pid} belongs to a different preview")
    current_ticks = process_start_ticks(pid)
    if current_ticks is None:
        return None
    if current_ticks != start_ticks:
        raise RuntimeError(f"Refusing to use reused PID {pid}")
    return pid, start_ticks


def stop_process(directory, name):
    path = directory / f"{name}.json"
    identity = recorded_process(directory, name)
    if identity is not None:
        pid, start_ticks = identity
        try:
            # pidfd pins the target, preventing a signal from reaching a reused
            # PID between the identity check and signal delivery.
            descriptor = os.pidfd_open(pid)
            try:
                if process_start_ticks(pid) != start_ticks:
                    raise ProcessLookupError()
                signal.pidfd_send_signal(descriptor, signal.SIGTERM)
                deadline = time.monotonic() + 5
                while process_start_ticks(pid) == start_ticks:
                    if time.monotonic() >= deadline:
                        raise RuntimeError(f"Preview {name} did not exit within 5 seconds")
                    time.sleep(0.05)
            finally:
                os.close(descriptor)
        except ProcessLookupError:
            pass
    path.unlink(missing_ok=True)


def start_service(directory):
    try:
        return rpc(directory)
    except (OSError, RuntimeError):
        pass
    fixture(directory)
    environment = os.environ.copy()
    environment["CLOUDTUNE_PREVIEW_DIRECTORY"] = str(directory)
    with (directory / "service.log").open("ab") as log:
        process = subprocess.Popen(
            [str(SERVICE), "--directory", str(directory)], env=environment,
            stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
    remember_process(directory, "service", process)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Service exited; see {directory / 'service.log'}")
        try:
            return rpc(directory)
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"Service did not become ready; see {directory / 'service.log'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=ROOT / ".runtime/overlay-preview")
    parser.add_argument("--no-build", action="store_true", help="Use an existing preview build")
    parser.add_argument("--detach", action="store_true", help="Leave the preview window running")
    parser.add_argument("--service-only", action="store_true", help="Start only the audio process")
    parser.add_argument("--stop", action="store_true", help="Close this preview and its service")
    parser.add_argument("--command", choices=COMMANDS, help="Send a command to a running preview")
    parser.add_argument("--argument", type=int, default=0, help="Volume 0–1000, seek ms, etc.")
    args = parser.parse_args()
    directory = args.directory.expanduser().resolve()
    os.umask(0o077)
    directory.mkdir(parents=True, mode=0o700, exist_ok=True)
    if directory.stat().st_uid != os.getuid():
        raise RuntimeError("Preview directory must be owned by the current user")
    directory.chmod(0o700)
    if args.stop:
        with launcher_lock(directory):
            stop_process(directory, "gui")
            stop_process(directory, "service")
        print("Preview stopped.")
        return
    if args.command:
        if not 0 <= args.argument <= 0xFFFFFFFF:
            parser.error("--argument must fit uint32")
        print(json.dumps(rpc(directory, COMMANDS[args.command], args.argument),
                         ensure_ascii=False, indent=2))
        return
    if not args.no_build:
        build()
    with launcher_lock(directory):
        status = start_service(directory)
        state = {0: "stopped", 1: "buffering", 2: "playing", 3: "paused", 4: "error"}.get(
            status["state"], "unknown")
        print(f"Player state: {state}; volume: {status['volume_permille'] / 10:g}%.")
        if args.service_only:
            print(f"Preview service ready: {directory / 'control.sock'}")
            return
        existing = recorded_process(directory, "gui")
        if existing is not None:
            print(f"CloudTune NX desktop overlay preview already open (PID {existing[0]}).")
            return
        if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
            raise RuntimeError("Set DISPLAY to the desktop where the preview should appear")
        environment = os.environ.copy()
        environment["CLOUDTUNE_PREVIEW_DIRECTORY"] = str(directory)
        with (directory / "gui.log").open("ab") as log:
            process = subprocess.Popen(
                [str(GUI)], env=environment, stdin=subprocess.DEVNULL,
                stdout=log, stderr=log, start_new_session=True)
        remember_process(directory, "gui", process)
    print("CloudTune NX desktop overlay preview opened.")
    print("Closing the window leaves the independent test player running.")
    print(f"Stop both: {sys.executable} {Path(__file__).resolve()} --directory {directory} --stop")
    if not args.detach:
        raise SystemExit(process.wait())


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"overlay-preview: {error}", file=sys.stderr)
        raise SystemExit(1)
