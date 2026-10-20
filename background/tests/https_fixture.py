#!/usr/bin/env python3
"""Serve generated MP3 fixtures over TLS, slowly enough to test progressive play.

Usage: https_fixture.py DIRECTORY CERTIFICATE KEY [PORT]
Certificates are caller-provided test fixtures; the player must trust the
certificate explicitly through its normal CA bundle setting.
"""
import functools
import http.server
import ssl
import sys
import time


class SlowHandler(http.server.SimpleHTTPRequestHandler):
    def copyfile(self, source, destination):
        while chunk := source.read(4096):
            try:
                destination.write(chunk)
                destination.flush()
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                return
            time.sleep(0.125)


server = http.server.ThreadingHTTPServer(
    ("127.0.0.1", int(sys.argv[4]) if len(sys.argv) > 4 else 18443),
    functools.partial(SlowHandler, directory=sys.argv[1]),
)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(sys.argv[2], sys.argv[3])
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
