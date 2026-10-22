#!/usr/bin/env python3
"""Feed generated compressed ALAC to the excluded native memory probe in Eden.

Run after launching ncm-switch-memory-probe with an empty fixture cache and a
fresh memory-probe.log. Requires system FFmpeg and Python cryptography, like
airplay_receiver_tests.py. No account or real music is used.
"""
import argparse
import base64
import json
from pathlib import Path
import socket
import time

import airplay_receiver_tests as wire


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="isolated Eden SD memory-probe.log")
    parser.add_argument("--port", type=int, default=7201)
    parser.add_argument("--seconds", type=int, default=30)
    args = parser.parse_args()
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        text = args.log.read_text() if args.log.exists() else ""
        wire.check("FAIL:" not in text, "native music probe passed before AirPlay")
        if "AAC active; awaiting AirPlay" in text:
            break
        time.sleep(0.1)
    else:
        raise TimeoutError("Native probe did not request AirPlay")

    session = wire.Rtsp(args.port)
    try:
        challenge = bytes(range(16))
        headers, _ = session.request("OPTIONS", {"Apple-Challenge": wire.b64(challenge)})
        answer = headers.get("apple-response", "")
        wire.check(bool(answer), "native receiver returns Apple-Response")
        signature = base64.b64decode(answer + "=" * (-len(answer) % 4))
        key = wire.public_key().public_numbers()
        raw = pow(int.from_bytes(signature, "big"), key.e, key.n).to_bytes(256, "big")
        preferences = json.loads((args.log.parent / "airplay.json").read_text())
        identity = bytes.fromhex(preferences["device_id"])
        wire.check(len(identity) == 6, "isolated receiver identity is six bytes")
        payload = challenge + socket.inet_aton("127.0.0.1") + identity + bytes(6)
        wire.check(raw == b"\x00\x01" + b"\xff" * 221 + b"\x00" + payload,
                   "native challenge signature matches challenge, address and receiver identity")
        preflight = session
        session = wire.Rtsp(args.port)
        try:
            session.request("OPTIONS")
            wire.check(preflight.socket.recv(1) == b"", "idle selection yields to playback connection")
        finally:
            preflight.close()
        print("PASS native Apple-Challenge and idle probe-to-playback handoff", flush=True)
        session.setup(wire.public_key())
        session.request("RECORD", {"RTP-Info": "seq=1;rtptime=100000"})
        session.request("SET_PARAMETER", {"Content-Type": "text/parameters"}, b"volume: -10.0\r\n")
        # Classic phone latency, not the old 200 ms miniature test queue.
        session.sync(100000, latency=99225)
        payload, _ = wire.compressed_alac()
        packets = int(args.seconds * 44100 / 352)
        start = time.monotonic()
        for index in range(packets):
            session.rtp(index + 1, 100000 + index * 352, payload)
            session.reply_timing()
            if index % 625 == 400:
                cover = b"\xff\xd8" + bytes(256 * 1024 - 4) + b"\xff\xd9"
                session.request("SET_PARAMETER", {"Content-Type": "image/jpeg"}, cover)
            delay = start + (index + 1) * 352 / 44100 - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        time.sleep(3)
        session.request("FLUSH", {"RTP-Info": f"seq={packets+1};rtptime={100000+packets*352}"})
        session.request("TEARDOWN")
        print(f"PASS: encrypted ALAC, 2.25 s latency, repeated 256 KiB artwork, {packets} packets, FLUSH and TEARDOWN")
    finally:
        session.close()


if __name__ == "__main__":
    main()
