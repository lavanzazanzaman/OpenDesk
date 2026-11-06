#!/usr/bin/env python3
"""Cross-checks the C++ protocol primitives (odproto-tool) against the verified Python reference (engine/opendesk_proto.py).

Usage: crosscheck.py path/to/odproto-tool [path/to/dcgen.dll]
"""
import base64
import os
import random
import subprocess
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "engine"))
import opendesk_proto as od  # noqa: E402

TOOL = sys.argv[1]
DCGEN = sys.argv[2] if len(sys.argv) > 2 else None
failures = 0


def run(*args, stdin=""):
    r = subprocess.run([TOOL, *args], input=stdin, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{args[0]} failed: {r.stderr}")
    return r.stdout.strip()


def check(name, ok, detail=""):
    global failures
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'' if ok else '  ' + detail}")
    failures += not ok


key, iv = os.urandom(32), os.urandom(16)
payloads = [b"", b"x", os.urandom(15), os.urandom(16), os.urandom(127), os.urandom(128), os.urandom(5000),
            os.urandom(300000)]

# 1. C++ writer -> Python reader (CBC chained across chunks)
chunks = run("aes-encode", key.hex(), iv.hex(), stdin="\n".join(p.hex() for p in payloads) + "\n").splitlines()
dec = od.AesChunkDecoder(key, iv).chunks(b"".join(bytes.fromhex(c) for c in chunks))
check("C++ AesStream writer -> Python reader", dec == payloads, f"{len(dec)} chunks")

# 2. Python writer -> C++ reader, fed in awkward piece sizes
w = od.AesStreamWriter(key, iv)
stream = b"".join(w.encode(p) for p in payloads)
for step in (1, 7, 16, 1000, len(stream)):
    out = run("aes-decode", key.hex(), iv.hex(), str(step), stdin=stream.hex() + "\n")
    check(f"Python AesStream writer -> C++ reader (pieces of {step})", bytes.fromhex(out) == b"".join(payloads))

# 3. discovery request parsing
priv = rsa.generate_private_key(public_exponent=65537, key_size=1024)
nums = priv.public_key().public_numbers()
b64 = lambda n: base64.b64encode(n.to_bytes((n.bit_length() + 7) // 8, "big")).decode()  # noqa: E731
rsa_xml = f"<RSAKeyValue><Modulus>{b64(nums.n)}</Modulus><Exponent>{b64(nums.e)}</Exponent></RSAKeyValue>"
bw = od.AesStreamWriter(od.BROADCAST_KEY, od.BROADCAST_IV)
request = bw.encode(rsa_xml.encode()) + bw.encode(b"\0") + bw.encode(bytes([1])) + bw.encode(b"sksnskskss")
check("parse discovery request", run("parse-request", stdin=request.hex() + "\n") == "1 sksnskskss")

# 4. discovery reply: RSA-OAEP-SHA1 unwrap + AesStream XML, then the real DataContractSerializer
reply = bytes.fromhex(run("discovery-reply", stdin=request.hex() + "\n"))
key_iv = priv.decrypt(reply[:128], padding.OAEP(mgf=padding.MGF1(hashes.SHA1()), algorithm=hashes.SHA1(), label=None))
xml = b"".join(od.AesChunkDecoder(key_iv[:32], key_iv[32:48]).chunks(reply[128:]))
check("discovery reply decrypts (RSA-OAEP-SHA1 + AesStream)", xml.startswith(b"<Computer "))

# 5. XML identical to the Python reference for the same computer
ref = od.ComputerInfo(
    id="e32d4ce0-ff56-474b-a840-97889932304c", name="CachyOS & <test>", description="Linux streamer",
    adapters=[od.NetworkAdapter("192.168.178.101", "74563CB94066", False, True, False),
              od.NetworkAdapter("fe80::1", "", False, True, True)],
    connection_id=bytes(range(16)), key=bytes(range(0x40, 0x60)), iv=bytes(range(0x80, 0x90)))
check("Computer XML byte-identical to Python", xml == ref.to_xml(), f"\n{xml}\n{ref.to_xml()}")

if DCGEN:
    r = subprocess.run(["dotnet", DCGEN, "--check"], input=xml, capture_output=True)
    out = r.stdout.decode()
    check("real DataContractSerializer parses it", "Name=CachyOS & <test>" in out and "StreamerVersion=1.34.23.0" in out,
          out + r.stderr.decode())

# 6. random chunk-size fuzz of the incremental reader
random.seed(1)
for _ in range(20):
    ps = [os.urandom(random.randint(0, 3000)) for _ in range(random.randint(1, 30))]
    w = od.AesStreamWriter(key, iv)
    s = b"".join(w.encode(p) for p in ps)
    step = random.randint(1, 4096)
    ok = bytes.fromhex(run("aes-decode", key.hex(), iv.hex(), str(step), stdin=s.hex() + "\n")) == b"".join(ps)
    if not ok:
        check(f"fuzz step {step}", False)
check("incremental reader fuzz (20 random streams)", True)

print("ALL PASS" if not failures else f"{failures} FAILURE(S)")
sys.exit(1 if failures else 0)
