#!/usr/bin/env python3
"""Feed generated compressed ALAC to the excluded native memory probe in Eden.

Run after launching ncm-switch-memory-probe with an empty fixture cache and a
fresh memory-probe.log. Requires system FFmpeg and Python cryptography, like
airplay_receiver_tests.py. No account or real music is used.
"""
import argparse
from pathlib import Path
import time

import airplay_receiver_tests as wire


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="isolated Eden SD memory-probe.log")
    parser.add_argument("--port", type=int, default=7201)
    args = parser.parse_args()
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        text = args.log.read_text() if args.log.exists() else ""
        wire.check("FAIL:" not in text, "native music probe passed before AirPlay")
        if "all formats complete; awaiting AirPlay" in text:
            break
        time.sleep(0.1)
    else:
        raise TimeoutError("Native probe did not request AirPlay")

    session = wire.Rtsp(args.port)
    try:
        session.request("OPTIONS")
        session.setup(wire.public_key())
        session.request("RECORD", {"RTP-Info": "seq=1;rtptime=100000"})
        session.request("SET_PARAMETER", {"Content-Type": "text/parameters"}, b"volume: -10.0\r\n")
        session.sync(100000)
        payload, _ = wire.compressed_alac()
        for index in range(300):
            session.rtp(index + 1, 100000 + index * 352, payload)
            session.reply_timing()
            time.sleep(352 / 44100)
        time.sleep(1)
        session.request("FLUSH", {"RTP-Info": "seq=301;rtptime=205600"})
        session.request("TEARDOWN")
        print("PASS: encrypted compressed ALAC, timing, volume, 300 packets, FLUSH and TEARDOWN")
    finally:
        session.close()


if __name__ == "__main__":
    main()
