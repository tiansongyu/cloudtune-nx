#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real libcurl TLS/API boundaries using synthetic, loopback-only data."""
import base64
import datetime
import http.server
import json
import os
import pathlib
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading

from cryptography import x509
from cryptography.hazmat.primitives import hashes, padding, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.x509.oid import NameOID


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def key_response():
    state = json.dumps({
        "version": "fixture-1", "sk": "fixture-sk",
        "publicKey": base64.b64encode(bytes.fromhex(
            "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")).decode(),
    }).encode()
    pad = padding.PKCS7(128).padder()
    padded = pad.update(state) + pad.finalize()
    enc = Cipher(algorithms.AES(bytes.fromhex(
        "ab1d5a430f6bb04a3f01e81ddd72bd916d5ce591248ac128714806d7f8fb1b84")), modes.ECB()).encryptor()
    encrypted = enc.update(padded) + enc.finalize()
    return json.dumps({"code": 200, "data": {
        "encryptedData": base64.b64encode(encrypted).decode()}}).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, body, **headers):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for name, value in headers.items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_CONNECT(self):
        if self.path not in {
            "music.163.com:443", "interface.music.163.com:443",
            "interface3.music.163.com:443",
        }:
            self.reply(403, b"")
            return
        self.send_response(200, "Connection established")
        self.end_headers()
        self.connection = self.server.context.wrap_socket(self.connection, server_side=True)
        self.rfile = self.connection.makefile("rb", self.rbufsize)
        self.wfile = self.connection.makefile("wb", self.wbufsize)
        self.close_connection = False
        self.handle_one_request()

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.server.external:
            self.server.leaked = True
            self.reply(200, b'{"code":200}')
        elif self.path.endswith("/security/key/get"):
            self.reply(200, key_response())
        elif self.server.case == "redirect":
            self.server.auth_header = self.headers.get("x-music-u", "")
            self.reply(307, b"", Location=self.server.redirect)
        elif self.server.case in ("cookie-scope", "download"):
            self.server.auth_header = self.headers.get("x-music-u", "")
            self.reply(200, b'{"code":200}')
        elif self.server.case == "malformed":
            self.reply(200, b'{"code":200,"cookie":"synthetic-private-response"!')
        elif self.server.case == "code-suffix":
            self.reply(200, b'{"code":"200tail"}')
        elif self.server.case == "code-overflow":
            self.reply(200, b'{"code":4294967496}')
        else:
            self.reply(500, b"")

    def do_GET(self):
        if self.server.external:
            self.server.received_download = True
            self.server.auth_header = self.headers.get("x-music-u", "")
            self.reply(200, b"fixture-image")
        else:
            self.reply(302, b"", Location=self.server.redirect)


def certificate(directory):
    private = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "CloudTune test fixture")])
    now = datetime.datetime.utcnow()
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(private.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=1))
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .add_extension(x509.SubjectAlternativeName([
                x509.DNSName("music.163.com"), x509.DNSName("interface.music.163.com"),
                x509.DNSName("interface3.music.163.com")]), critical=False)
            .sign(private, hashes.SHA256()))
    cert_path, key_path = directory / "ca.pem", directory / "key.pem"
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(private.private_bytes(serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert_path, key_path)
    return context, cert_path


def main():
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="cloudtune-api-transport-") as temp:
        directory = pathlib.Path(temp)
        context, ca = certificate(directory)
        proxy = Server(("127.0.0.1", 0), Handler)
        external = Server(("127.0.0.1", 0), Handler)
        proxy.context, proxy.external = context, False
        external.external, external.leaked, external.received_download = True, False, False
        proxy.redirect = f"http://127.0.0.1:{external.server_port}/redirected"
        threads = [threading.Thread(target=s.serve_forever, daemon=True)
                   for s in (proxy, external)]
        for thread in threads:
            thread.start()
        try:
            for case in ("redirect", "cookie-scope", "malformed", "code-suffix",
                         "code-overflow", "download"):
                proxy.case = case
                proxy.auth_header = ""
                subprocess.run([binary, "--transport", case,
                    f"http://127.0.0.1:{proxy.server_port}", str(ca), str(directory / "image")],
                    check=True, timeout=10, capture_output=True, text=True,
                    env={**os.environ, "NO_PROXY": "127.0.0.1,localhost",
                         "no_proxy": "127.0.0.1,localhost"})
                if case in ("redirect", "cookie-scope", "download"):
                    assert proxy.auth_header == "synthetic-music-session", \
                        "Unrelated-domain cookie was used as NetEase authentication"
                assert not external.leaked, "Authenticated POST reached external redirect"
            assert external.received_download, "Public CDN redirect was not followed"
            assert not external.auth_header, "Download forwarded a stale authentication header"
        finally:
            for server in (proxy, external):
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join()
    print("API transport: redirects, cookie scope, private parse errors, status bounds and CDN downloads passed")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stderr.strip(), file=sys.stderr)
        raise SystemExit(1)
