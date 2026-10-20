#!/usr/bin/env python3
"""Independent, loopback-only RAOP wire oracle; no audio device or account."""
import base64
import json
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = Path(__file__).resolve().parents[1]
DEVICE = bytes.fromhex("0243544e5801")
AES_KEY = bytes(range(16))
AES_IV = bytes(range(16, 32))
SAMPLES = [-32768, 32767, -1, 1, 0, 0, 1234, -2345,
           12000, -12000, 42, -42, 256, -256, 30000, -30000]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def b64(data):
    return base64.b64encode(data).decode().rstrip("=")


def ntp():
    value = time.time() + 2208988800
    seconds = int(value)
    return struct.pack("!II", seconds & 0xFFFFFFFF, int((value - seconds) * 2**32))


def alac(samples, frames=None):
    fields = [(1, 3), (0, 4), (0, 12), (1, 1), (0, 2), (1, 1),
              (len(samples) // 2 if frames is None else frames, 32)]
    fields.extend((sample & 0xFFFF, 16) for sample in samples)
    fields.append((7, 3))
    bits = "".join(f"{value:0{width}b}" for value, width in fields)
    bits += "0" * (-len(bits) % 8)
    return int(bits, 2).to_bytes(len(bits) // 8, "big")


def encrypt(payload):
    size = len(payload) & ~15
    encoder = Cipher(algorithms.AES(AES_KEY), modes.CBC(AES_IV)).encryptor()
    return encoder.update(payload[:size]) + encoder.finalize() + payload[size:]


def public_key():
    # The published protocol compatibility key is source data, not a test
    # secret. Derive only its public part; encryption uses cryptography.
    text = (ROOT / "airplay/vendor/shairport_rsa_key.h").read_text()
    fragments = re.findall(r'"((?:[^"\\]|\\.)*)"', text)
    pem = "".join(bytes(fragment, "utf-8").decode("unicode_escape") for fragment in fragments)
    start = pem.index("-----BEGIN")
    return serialization.load_pem_private_key(pem[start:].encode(), None).public_key()


class Probe:
    def __init__(self, binary, port=None, retry=False):
        if port is None:
            temporary = socket.socket()
            temporary.bind(("127.0.0.1", 0))
            port = temporary.getsockname()[1]
            temporary.close()
        self.port = port
        arguments = [binary, str(self.port)] + (["--retry"] if retry else [])
        self.process = subprocess.Popen(arguments, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True, bufsize=1)
        check(self.read().get("ready") == (not retry),
              "receiver reports expected initial bind result")

    def read(self):
        check(select.select([self.process.stdout], [], [], 6)[0], "probe responds within deadline")
        line = self.process.stdout.readline()
        check(line, "probe remains alive")
        return json.loads(line)

    def command(self, command="status"):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return self.read()

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.write("stop\n")
            self.process.stdin.flush()
            check(self.read().get("stopped"), "receiver clean shutdown")
            check(self.process.wait(timeout=5) == 0, "probe exits successfully")
        self.process.stdin.close()
        self.process.stdout.close()


class Rtsp:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.buffer = b""
        self.cseq = 0
        self.control = self.timing = self.audio = None
        self.remote = {}

    def response(self):
        while b"\r\n\r\n" not in self.buffer:
            chunk = self.socket.recv(4096)
            check(chunk, "RTSP connection remains open for response")
            self.buffer += chunk
        header, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        lines = header.decode().split("\r\n")
        code = int(lines[0].split()[1])
        headers = {}
        for line in lines[1:]:
            key, value = line.split(":", 1)
            headers[key.lower()] = value.strip()
        length = int(headers.get("content-length", 0))
        while len(self.buffer) < length:
            self.buffer += self.socket.recv(4096)
        body, self.buffer = self.buffer[:length], self.buffer[length:]
        return code, headers, body

    def request(self, method, headers=None, body=b"", expect=200):
        self.cseq += 1
        fields = {"CSeq": str(self.cseq), "Content-Length": str(len(body))}
        fields.update(headers or {})
        request = f"{method} rtsp://127.0.0.1/test RTSP/1.0\r\n"
        request += "".join(f"{key}: {value}\r\n" for key, value in fields.items()) + "\r\n"
        self.socket.sendall(request.encode() + body)
        code, returned, data = self.response()
        check(code == expect, f"{method}: expected {expect}, got {code}")
        check(returned.get("cseq") == str(self.cseq), "RTSP echoes CSeq")
        return returned, data

    def setup(self, public, pcm=False, encrypted=True, frames=352):
        key = public.encrypt(AES_KEY, padding.OAEP(mgf=padding.MGF1(hashes.SHA1()),
                                                   algorithm=hashes.SHA1(), label=None))
        sdp = "v=0\r\nm=audio 0 RTP/AVP 96\r\n"
        if pcm:
            sdp += "a=rtpmap:96 L16/44100/2\r\n"
        else:
            sdp += "a=rtpmap:96 AppleLossless\r\n"
            sdp += f"a=fmtp:96 {frames} 0 16 40 10 14 2 255 0 0 44100\r\n"
        if encrypted:
            sdp += f"a=rsaaeskey:{b64(key)}\r\na=aesiv:{b64(AES_IV)}\r\n"
        self.request("ANNOUNCE", {"Content-Type": "application/sdp"}, sdp.encode())
        self.control, self.timing, self.audio = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                                                for _ in range(3)]
        for stream in [self.control, self.timing, self.audio]:
            stream.bind(("127.0.0.1", 0))
            stream.setblocking(False)
        transport = ("RTP/AVP/UDP;unicast;mode=record;"
                     f"control_port={self.control.getsockname()[1]};"
                     f"timing_port={self.timing.getsockname()[1]}")
        headers, _ = self.request("SETUP", {"Transport": transport})
        self.remote = {key: int(value) for key, value in
                       re.findall(r"(server_port|control_port|timing_port)=(\d+)", headers["transport"])}
        check(len(self.remote) == 3, "SETUP supplies three UDP ports")
        check(select.select([self.timing], [], [], 2)[0], "receiver sends timing request")
        self.reply_timing()

    def reply_timing(self):
        replies = 0
        while select.select([self.timing], [], [], 0)[0]:
            request, source = self.timing.recvfrom(1500)
            check(len(request) == 32 and request[1] == 0xD2, "RAOP timing request shape")
            response = bytes([0x80, 0xD3, 0, 7]) + bytes(4) + request[24:32] + ntp() + ntp()
            self.timing.sendto(response, source)
            replies += 1
        return replies

    def sync(self, timestamp, latency=8820):
        message = struct.pack("!BBHI", 0x80, 0xD4, 0, (timestamp - latency) & 0xFFFFFFFF)
        message += ntp() + struct.pack("!I", timestamp)
        self.control.sendto(message, ("127.0.0.1", self.remote["control_port"]))
        time.sleep(0.02)

    def rtp(self, sequence, timestamp, payload, encrypted=True, resent=False):
        packet = struct.pack("!BBHII", 0x80, 0x60, sequence & 65535,
                             timestamp & 0xFFFFFFFF, 1)
        packet += encrypt(payload) if encrypted else payload
        if resent:
            packet = b"\x80\xd6\x00\x01" + packet
            self.control.sendto(packet, ("127.0.0.1", self.remote["control_port"]))
        else:
            self.audio.sendto(packet, ("127.0.0.1", self.remote["server_port"]))

    def close(self):
        self.socket.close()
        for stream in [self.control, self.timing, self.audio]:
            if stream:
                stream.close()


def wait(probe, session, predicate, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if session and session.timing:
            session.reply_timing()
        status = probe.command()
        if predicate(status):
            return status
        time.sleep(0.02)
    raise AssertionError(f"receiver condition timed out; status={status}")


def expected(samples):
    return zlib.crc32(struct.pack("<" + "h" * len(samples), *samples))


def compressed_alac():
    """Encode generated integer PCM with the independent system FFmpeg binary."""
    samples = [sample for frame in range(352) for sample in
               ((frame % 128) * 100 - 6400, 3200 - (frame % 64) * 100)]
    pcm = struct.pack("<" + "h" * len(samples), *samples)
    with tempfile.TemporaryDirectory(prefix="cloudtune-alac-") as temporary:
        output = str(Path(temporary) / "synthetic.m4a")
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error",
                        "-f", "s16le", "-ar", "44100", "-ac", "2", "-i", "pipe:0",
                        "-c:a", "alac", "-frames:a", "1", "-y", output],
                       input=pcm, check=True, timeout=10)
        encoded = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error",
                                  "-i", output, "-map", "0:a:0", "-c:a", "copy",
                                  "-f", "data", "pipe:1"],
                                 check=True, stdout=subprocess.PIPE, timeout=10).stdout
    check(4 < len(encoded) < len(pcm), "independent encoder compresses the synthetic PCM")
    check(not (encoded[2] & 2), "ALAC fixture uses the compressed channel-pair branch")
    return encoded, samples


def retry_bind(binary):
    occupied = socket.socket()
    occupied.bind(("127.0.0.1", 0))
    occupied.listen(1)
    probe = Probe(binary, occupied.getsockname()[1], retry=True)
    try:
        state = probe.command()
        check(state["enabled"] == 1 and state["state"] == 5,
              "occupied port is an enabled, retryable network error")
        occupied.close()
        wait(probe, None, lambda row: row["state"] == 2, timeout=3)
        session = Rtsp(probe.port)
        session.request("OPTIONS")
        session.close()
    finally:
        occupied.close()
        probe.close()
    # Stopping during the retry delay must not wait for the next bind attempt.
    occupied = socket.socket()
    occupied.bind(("127.0.0.1", 0))
    occupied.listen(1)
    probe = Probe(binary, occupied.getsockname()[1], retry=True)
    try:
        started = time.monotonic()
        probe.close()
        check(time.monotonic() - started < 0.8, "retry wait is promptly cancellable")
    finally:
        occupied.close()
        if probe.process.poll() is None:
            probe.close()
    print("PASS occupied port automatically recovers, retry shutdown remains prompt")


def boundary_cases(binary, public):
    probe = Probe(binary)
    session = None
    try:
        initial_fds = len(list(Path(f"/proc/{probe.process.pid}/fd").iterdir()))
        session = Rtsp(probe.port)
        # Sixteen pipelined messages cross the receiver's eight-message budget.
        requests = [f"OPTIONS * RTSP/1.0\r\nCSeq: {index}\r\n\r\n".encode()
                    for index in range(1, 17)]
        session.socket.sendall(b"".join(requests))
        for index in range(1, 17):
            code, headers, _ = session.response()
            check(code == 200 and headers["cseq"] == str(index),
                  "bounded RTSP pipeline preserves order and drains all messages")
        session.cseq = 16
        # Exercise a header and body arriving in small independent writes.
        fragmented = (b"SET_PARAMETER * RTSP/1.0\r\nCSeq: 17\r\n"
                      b"Content-Type: text/parameters\r\nContent-Length: 12\r\n\r\nvolume: -5\r\n")
        for offset in range(0, len(fragmented), 3):
            session.socket.sendall(fragmented[offset:offset + 3])
        check(session.response()[0] == 200, "fragmented RTSP body is parsed by byte count")
        session.cseq = 17
        session.setup(public, encrypted=False)
        session.request("RECORD")

        def dmap(tag, body):
            return tag + struct.pack("!I", len(body)) + body

        session.request("SET_PARAMETER", {"Content-Type": "application/x-dmap-tagged"},
                        dmap(b"minm", b"retained-title"))
        malformed_dmap = [b"minm" + struct.pack("!I", 0xFFFFFFFF) + b"x",
                          b"minm" + struct.pack("!I", 10) + b"x",
                          b"min", b"minm\x00\x00\x00"]
        nested = dmap(b"minm", b"too-deep")
        for _ in range(64):
            nested = dmap(b"mlit", nested)
        malformed_dmap.append(nested)
        for data in malformed_dmap:
            session.request("SET_PARAMETER", {"Content-Type": "application/x-dmap-tagged"}, data)
            check(probe.command()["title"] == "retained-title",
                  "truncated lengths or excessive DMAP nesting mutated metadata")

        base = struct.pack("!BBHII", 0x80, 0x60, 900, 900000, 1)
        malformed_rtp = [base[:length] for length in range(12)]
        malformed_rtp += [
            bytes([0x8F]) + base[1:] + b"x",  # missing CSRC entries
            bytes([0x90]) + base[1:] + b"\x00\x00\xff\xff",  # excessive extension
            bytes([0x90]) + base[1:] + b"\x00",  # truncated extension
            bytes([0xA0]) + base[1:] + b"\x00",  # zero padding
            bytes([0xA0]) + base[1:] + b"\xff",  # padding exceeds payload
            b"\x80\xd6\x00\x01" + base[:10],  # short retransmission envelope
        ]
        raw = alac(SAMPLES)
        malformed_rtp.extend(base + raw[:cut] for cut in range(1, len(raw) - 6))
        for packet in malformed_rtp:
            session.audio.sendto(packet, ("127.0.0.1", session.remote["server_port"]))
        for length in range(20):
            session.control.sendto((b"\x80\xd4" + bytes(18))[:length],
                                   ("127.0.0.1", session.remote["control_port"]))
        for length in range(32):
            session.timing.sendto((b"\x80\xd3" + bytes(30))[:length],
                                  ("127.0.0.1", session.remote["timing_port"]))
        time.sleep(0.05)
        check(probe.command()["frames"] == 0, "malformed packets reached the audio callback")
        session.sync(900000)
        session.rtp(900, 900000, raw, encrypted=False)
        status = wait(probe, session, lambda row: row["frames"] == 8)
        check(status["crc"] == expected(SAMPLES), "valid audio did not recover after malformed packets")
        session.close()
        session = None
        wait(probe, None, lambda row: row["state"] == 2)
        # Disconnect while requests/bodies and audio are pending. Each RECORD
        # must release ownership exactly once, and listener restarts stay bounded.
        for iteration in range(12):
            session = Rtsp(probe.port)
            session.setup(public, pcm=True, encrypted=False)
            session.request("RECORD")
            session.socket.sendall(b"SET_PARAMETER * RTSP/1.0\r\nCSeq: 50\r\n"
                                   b"Content-Length: 524288\r\n\r\npartial")
            started = time.monotonic()
            if iteration % 3 == 0:
                check(probe.command("restart")["restarted"], "receiver restart failed")
            else:
                check(probe.command("disconnect")["disconnected"], "partial request disconnect failed")
            check(time.monotonic() - started < 1, "pending body delayed receiver shutdown")
            session.close()
            session = None
            status = wait(probe, None, lambda row: row["state"] == 2)
            check(status["begins"] == status["ends"], "disconnect leaked or duplicated stream ownership")
        check(len(list(Path(f"/proc/{probe.process.pid}/fd").iterdir())) == initial_fds,
              "repeated sessions/restarts leaked socket descriptors")
        print(f"PASS boundary corpus: 16 pipelined RTSP, fragmented body, "
              f"{len(malformed_dmap)} DMAP, {len(malformed_rtp) + 52} UDP, "
              "12 interrupted sessions / 4 restarts; exact PCM recovery and no FD leak")
    finally:
        if session:
            session.close()
        probe.close()


def run(binary):
    public = public_key()
    probe = Probe(binary)
    session = None
    try:
        session = Rtsp(probe.port)
        challenge = bytes(range(16))
        headers, _ = session.request("OPTIONS", {"Apple-Challenge": b64(challenge)})
        signature = base64.b64decode(headers["apple-response"] + "==")
        numbers = public.public_numbers()
        recovered = pow(int.from_bytes(signature, "big"), numbers.e, numbers.n).to_bytes(256, "big")
        payload = challenge + socket.inet_aton("127.0.0.1") + DEVICE + bytes(6)
        check(recovered == b"\x00\x01" + b"\xff" * (256 - len(payload) - 3) + b"\x00" + payload,
              "Apple-Challenge independently verifies raw RSA PKCS#1 v1.5")
        busy = Rtsp(probe.port)
        check(busy.response()[0] == 453, "second RTSP client cannot take over")
        busy.close()
        session.setup(public)
        probe.command("deny")
        session.request("RECORD", expect=453)
        probe.command("allow")
        session.request("RECORD")
        session.sync(100000)
        session.request("SET_PARAMETER", {"Content-Type": "text/parameters"}, b"volume: -12.5\r\n")

        def metadata(title_bytes, artist_bytes):
            tags = (b"minm" + struct.pack("!I", len(title_bytes)) + title_bytes +
                    b"asar" + struct.pack("!I", len(artist_bytes)) + artist_bytes)
            session.request("SET_PARAMETER", {"Content-Type": "application/x-dmap-tagged"},
                            b"mlit" + struct.pack("!I", len(tags)) + tags)

        # Network metadata is byte-counted UTF-8. Invalid encodings must never
        # reach the JSON/status/UI consumer, and capacity limits preserve whole
        # Unicode code points rather than splitting their byte sequences.
        malformed_title = b"\xff\xc0\x80\xed\xa0\x80\xf4\x90\x80\x80\x00\r\n" + "合法".encode()
        metadata(malformed_title, b"\x80artist\x7f")
        status = probe.command()
        check(status["title"] == "?" * 10 + "   合法" and status["artist"] == "?artist ",
              "invalid/overlong/surrogate UTF-8 and control bytes are sanitized")
        metadata(("x" * 43 + "🎵尾").encode(), ("歌" * 7 + "🎵").encode())
        status = probe.command()
        check(status["title"] == "x" * 43 + "🎵" and status["artist"] == "歌" * 7,
              "bounded metadata preserves complete emoji/Chinese code points")
        metadata(("x" * 45 + "中文").encode(), b"artist")
        check(probe.command()["title"] == "x" * 45,
              "metadata stops before a partially fitting UTF-8 code point")
        title, artist = "测试音乐 · 中文", "网易云测试歌手"
        title_bytes, artist_bytes = title.encode("utf-8"), artist.encode("utf-8")
        metadata(title_bytes, artist_bytes)
        payload = alac(SAMPLES)
        check(len(payload) % 16 != 0, "fixture exercises cleartext AES tail")
        session.rtp(100, 100000, payload)
        status = wait(probe, session, lambda row: row["frames"] == 8)
        check(status["crc"] == expected(SAMPLES) and status["peak"] == 32768,
              "RSA-OAEP/AES-CBC/ALAC end-to-end exact PCM")
        check(status["volume"] == -12.5 and status["title"] == title and status["artist"] == artist,
              "volume and UTF-8 DMAP metadata use byte lengths, preserving Chinese text")
        session.request("FLUSH", {"RTP-Info": "seq=200;rtptime=200000"})
        probe.command("reset")
        session.sync(200000)
        session.rtp(99, 99999, payload)
        session.rtp(200, 200000, payload)
        status = wait(probe, session, lambda row: row["frames"] == 8)
        check(status["crc"] == expected(SAMPLES) and status["flushes"] >= 1,
              "FLUSH rejects stale packets and resumes")

        # A deliberately missing middle packet must be requested before playout,
        # then delivered over the retransmission/control socket without silence.
        session.request("FLUSH", {"RTP-Info": "seq=300;rtptime=300000"})
        probe.command("reset")
        session.sync(300000, latency=22050)
        samples352 = (SAMPLES * 44)[:704]
        payload352 = alac(samples352)
        session.rtp(300, 300000, payload352)
        session.rtp(302, 300704, payload352)
        check(select.select([session.control], [], [], 0.4)[0], "missing RTP packet requests resend")
        resend, _ = session.control.recvfrom(1500)
        check(len(resend) == 8 and resend[1] == 0xD5 and struct.unpack("!H", resend[4:6])[0] == 301,
              "resend identifies the missing sequence")
        session.rtp(301, 300352, payload352, resent=True)
        status = wait(probe, session, lambda row: row["frames"] >= 1056)
        check(status["frames"] == 1056 and status["crc"] == expected(samples352 * 3),
              "retransmitted packets play in order with exact PCM")
        session.request("PAUSE")
        probe.command("reset")
        session.rtp(303, 301056, payload352)
        time.sleep(0.25)
        check(probe.command()["frames"] == 0, "PAUSE drops incoming audio")
        session.request("RECORD", {"RTP-Info": "seq=65535;rtptime=500000"})
        session.sync(500000)
        session.rtp(65535, 500000, payload352)
        session.rtp(0, 500352, payload352)
        status = wait(probe, session, lambda row: row["frames"] >= 704)
        check(status["frames"] == 704 and status["crc"] == expected(samples352 * 2),
              "RECORD resumes and 16-bit RTP sequence wraps without loss")
        session.request("TEARDOWN")
        session.close()
        session = None
        wait(probe, None, lambda row: row["ends"] >= 1 and row["state"] == 2)
        print("PASS encrypted ALAC, challenge, timing, metadata, owner, FLUSH, resend, PAUSE and sequence wrap")

        encoded, compressed_samples = compressed_alac()
        session = Rtsp(probe.port)
        session.setup(public, frames=4096)
        probe.command("reset")
        session.request("RECORD")
        session.sync(600000)
        session.rtp(600, 600000, encoded)
        status = wait(probe, session, lambda row: row["frames"] >= 352)
        check(status["frames"] == 352 and status["crc"] == expected(compressed_samples),
              "independent FFmpeg compressed ALAC preserves exact PCM through encrypted RTP")
        session.request("TEARDOWN")
        session.close()
        session = None
        wait(probe, None, lambda row: row["state"] == 2)
        print("PASS independently encoded compressed ALAC, encrypted RTP and exact PCM")

        session = Rtsp(probe.port)
        session.setup(public, pcm=True, encrypted=False)
        probe.command("reset")
        session.request("RECORD")
        session.sync(400000)
        session.rtp(400, 400000, struct.pack("!" + "h" * len(SAMPLES), *SAMPLES), encrypted=False)
        status = wait(probe, session, lambda row: row["frames"] == 8)
        check(status["crc"] == expected(SAMPLES), "L16 network-endian exact PCM")
        session.close()
        session = None
        wait(probe, None, lambda row: row["ends"] >= 2 and row["state"] == 2)
        print("PASS L16, disconnect releases owner, reconnect")

        invalid_requests = [
            b"OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: -1\r\n\r\n",
            b"OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 4294967296\r\n\r\n",
            b"OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 524289\r\n\r\n",
            b"OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nCSeq: 2\r\n\r\n",
            b"OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nbroken\r\n\r\n",
            b"OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nX-Large: " + b"x" * 17000,
        ]
        for request in invalid_requests:
            session = Rtsp(probe.port)
            session.socket.sendall(request)
            check(session.response()[0] in (400, 413), "bounded malformed RTSP rejection")
            session.close()
            session = None
            wait(probe, None, lambda row: row["state"] == 2)
        for fmtp in ["96 4097 0 16 40 10 14 2 255 0 0 44100",
                     "96 352 0 24 40 10 14 2 255 0 0 44100",
                     "96 352 0 16 40 10 14 2 255 0 0 192000"]:
            session = Rtsp(probe.port)
            body = ("v=0\r\na=rtpmap:96 AppleLossless\r\na=fmtp:" + fmtp + "\r\n").encode()
            session.request("ANNOUNCE", {"Content-Type": "application/sdp"}, body, expect=456)
            session.close()
            session = None
            wait(probe, None, lambda row: row["state"] == 2)
        session = Rtsp(probe.port)
        session.request("OPTIONS")
        check(probe.command("disconnect")["disconnected"], "explicit disconnect acknowledged")
        session.close()
        session = None
        print("PASS malformed headers/content lengths/fmtp, explicit disconnect, receiver remains healthy")
    finally:
        if session:
            session.close()
        probe.close()
    retry_bind(binary)
    boundary_cases(binary, public)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: airplay_receiver_tests.py PATH_TO_RECEIVER_PROBE")
    run(sys.argv[1])
