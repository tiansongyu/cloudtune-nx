#!/usr/bin/env python3
"""Independent XEAPI interoperability verification, requires Python cryptography.
Usage: build/api_tests --fixture | python3 tests/verify_xeapi.py
"""
import base64
import hashlib
import hmac
import json
import sys
import urllib.parse
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives import padding

def decrypt(data, key):
    cipher = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    plain = cipher.update(data) + cipher.finalize()
    unpad = padding.PKCS7(128).unpadder()
    return unpad.update(plain) + unpad.finalize()

fixture = json.load(sys.stdin)
form = {k: base64.b64decode(v[0]) for k, v in urllib.parse.parse_qs(fixture["xeapi"]).items()}
static = bytes.fromhex("ab1d5a430f6bb04a3f01e81ddd72bd916d5ce591248ac128714806d7f8fb1b84")
assert decrypt(form["R"], static) == b"test-1|"
s = form["S"]
private = X25519PrivateKey.from_private_bytes(bytes.fromhex(
    "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"))
shared = private.exchange(X25519PublicKey.from_public_bytes(s[:32]))
prk = hmac.new(bytes(32), shared, hashlib.sha256).digest()
aes_key = hmac.new(prk, s[:32] + b"\x01", hashlib.sha256).digest()[:16]
plain_s = AESGCM(aes_key).decrypt(s[32:44], s[44:], None)
dynamic, os, sk = plain_s.split(b"|")
assert os == b"android" and sk == b"fixture-sk"
mid = decrypt(form["B"], base64.b64decode(dynamic))
random, rotated = mid[:16], mid[16:]
rotation = (random[0] & 15) % len(rotated)
b64 = rotated[-rotation:] + rotated[:-rotation] if rotation else rotated
masked = base64.b64decode(b64)
inner = bytes(c ^ random[i % 16] for i, c in enumerate(masked))
body = json.loads(decrypt(inner, static))
assert body["queryString"] == "e_r=true"
assert urllib.parse.parse_qs(base64.b64decode(body["body"]).decode()) == {"like": ["true"], "trackId": ["347230"]}
signature = base64.b64encode(hmac.new(
    b"mUHCwVNWJbunMqAHf5MImuirT6plvs6VSFW62MGHstFQxhBGdEoIhLItH3djc4+FB/OKty3+lL2rGeoFBpVe5g==",
    b"17000000000001234567890123456", hashlib.sha256).digest()).decode()
assert fixture["signature"] == signature
print("Independent XEAPI X25519 / HKDF / AES-GCM / AES-ECB / HMAC verification passed")
