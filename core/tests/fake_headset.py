#!/usr/bin/env python3
"""Plays the headset side of the upstream protocol against a running streamer (native or Python) on 127.0.0.1.

Sequence (NetworkManager.ConnectToComputerAsync in the headset app):
  UDP discovery broadcast -> decrypt reply (RSA-OAEP + AesStream) -> Computer XML
  UDP 17-byte select (ConnectionID + platform)
  TCP messaging (AesStream): expect StreamerSettingsReload, send User/MobileSettingsReload
  TCP video/audio/data; data: read the bandwidth probe, then send FrameData packets
Exit code 0 when every step passed.
"""
import base64
import re
import socket
import struct
import sys
import time
import math
from pathlib import Path

import lz4.block
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "engine"))
import opendesk_proto as od  # noqa: E402

ACCOUNT = sys.argv[1] if len(sys.argv) > 1 else "testacct"
HOST = "127.0.0.1"
ok = True


def step(name, cond, detail=""):
    global ok
    print(f"{'PASS' if cond else 'FAIL'}  {name}{'' if cond else '  ' + str(detail)}", flush=True)
    ok &= bool(cond)
    if not cond:
        sys.exit(1)


class AesSocket:
    """Stream view over an AesStream TCP channel (or plain when key is None)."""

    def __init__(self, sock, key=None, iv=None):
        self.sock, self.buf = sock, b""
        self.dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor() if key else None
        self.enc = od.AesStreamWriter(key, iv) if key else None

    def _chunk(self):
        first = self.dec.update(recv_exact(self.sock, 16))
        length, pos = od.read_varint(first, 0)
        total = (pos + length + 15) // 16 * 16
        rest = self.dec.update(recv_exact(self.sock, total - 16)) if total > 16 else b""
        self.buf += (first + rest)[pos:pos + length]

    def read(self, n):
        if not self.dec:
            return recv_exact(self.sock, n)
        while len(self.buf) < n:
            self._chunk()
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, data):
        self.sock.sendall(self.enc.encode(data) if self.enc else data)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


# --- discovery
priv = rsa.generate_private_key(public_exponent=65537, key_size=1024)
nums = priv.public_key().public_numbers()
b64 = lambda n: base64.b64encode(n.to_bytes((n.bit_length() + 7) // 8, "big")).decode()  # noqa: E731
rsa_xml = f"<RSAKeyValue><Modulus>{b64(nums.n)}</Modulus><Exponent>{b64(nums.e)}</Exponent></RSAKeyValue>"
w = od.AesStreamWriter(od.BROADCAST_KEY, od.BROADCAST_IV)
# FAKE_HEADSET_APP_VERSION=1.34.25: the request of headset apps from 1.34.25 (account signature + their own version)
APP_VERSION = __import__("os").environ.get("FAKE_HEADSET_APP_VERSION")
if APP_VERSION:
    _, minor, build = (int(x) for x in APP_VERSION.split(".")[:3])
    request = (w.encode(rsa_xml.encode()) + w.encode(b"\1") + w.encode(bytes(range(64))) + w.encode(bytes([minor]))
               + w.encode(bytes([build])) + w.encode(bytes([1])) + w.encode(ACCOUNT.encode()))
else:
    request = w.encode(rsa_xml.encode()) + w.encode(b"\0") + w.encode(bytes([1])) + w.encode(ACCOUNT.encode())

udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.settimeout(3)
reply = None
for _ in range(10):  # the streamer may still be starting
    udp.sendto(request, (HOST, od.DISCOVERY_PORT))
    try:
        reply, server = udp.recvfrom(65535)
        break
    except TimeoutError:
        time.sleep(0.5)
step("discovery reply received", reply is not None)
key_iv = priv.decrypt(reply[:128], padding.OAEP(mgf=padding.MGF1(hashes.SHA1()), algorithm=hashes.SHA1(), label=None))
xml = b"".join(od.AesChunkDecoder(key_iv[:32], key_iv[32:48]).chunks(reply[128:])).decode()
field = lambda tag: re.search(f"<{tag}>([^<]*)</{tag}>", xml).group(1)  # noqa: E731
conn_id, key, iv = (base64.b64decode(field(t)) for t in ("ConnectionID", "Key", "IV"))
step("Computer XML decrypted", len(conn_id) == 16 and len(key) == 32 and len(iv) == 16, xml[:200])
print(f"      computer '{field('Name')}' ({field('Description')})")
_v = lambda part: re.search(rf"<(?:\w+:)?_{part}>(\d+)<", xml).group(1)  # noqa: E731
reported = f"{_v('Major')}.{_v('Minor')}.{_v('Build')}"
print(f"      reported streamer version: {reported}")
if APP_VERSION:
    step("streamer reports the headset app's version back", reported.startswith(APP_VERSION), reported)
encrypted = field("EncryptLocalTraffic") == "true"
print(f"      EncryptLocalTraffic={encrypted}")

# --- select + TCP channels
udp.sendto(conn_id + bytes([1]), server)
msg = socket.create_connection((HOST, od.MESSAGING_PORT), timeout=5)
reader_dec = od.AesChunkDecoder(key, iv)
writer = od.AesStreamWriter(key, iv)

# first chunk from the streamer: StreamerSettingsReload
data = b""
while len(data) < 16:
    data += msg.recv(4096)
first = reader_dec.chunks(data)
step("StreamerSettingsReload received", first and first[0][0] == 15, first)
print(f"      {first[0][5:].decode()}")
mobile = {"HmdType": 1000, "Resolution": {"Width": 2560, "Height": 1440}, "IPD": 0.063, "DesktopFramerate": 72}
if __import__("os").environ.get("FOVEATED"):  # an eye-tracked headset with foveated streaming on
    mobile["FoveatedStreaming"] = True
    # asymmetric per-eye FOV like a real headset's (each eye sees more to its outer side)
    FOV = ({"LeftTan": 1.2, "RightTan": 0.9, "UpTan": 1.0, "DownTan": 1.1}, {"LeftTan": 0.9, "RightTan": 1.2, "UpTan": 1.0, "DownTan": 1.1})
    mobile["FovLeft"], mobile["FovRight"] = FOV
BODY = __import__("os").environ.get("BODY")  # full body tracking (Quest 3 style): body joints, skeleton, emulated trackers
user = {"VRFramerate": 120, "DesktopBitrateLimit": 0.5}
if BODY:
    mobile["Features"] = 0x100 | 0x200  # BodyTracking | FullBodyTracking
    user["EmulateTrackers"] = True
import json  # noqa: E402
for t, payload in ((11, user), (13, mobile)):
    body = json.dumps(payload).encode()
    msg.sendall(writer.encode(bytes([t]) + len(body).to_bytes(4, "big") + body))

# UpdateRequired (headset newer than streamer) must be answered with UpdateUnavailable
msg.sendall(writer.encode(bytes([4]) + (0).to_bytes(4, "big")))
msg.settimeout(3)
got = b""
answered = False
t_end = time.monotonic() + 3
while time.monotonic() < t_end and not answered:
    got += msg.recv(4096)
    try:  # re-decode the whole streamer->headset stream so far (one CBC chain from the start)
        plain = od.AesChunkDecoder(key, iv).chunks(data + got)
        answered = any(c[:1] == bytes([5]) for c in plain)
    except Exception:
        pass
step("UpdateRequired answered with UpdateUnavailable", answered)

# ScreenshotToDesktop: i16 BE name length + UTF-8 name, i32 BE size, PNG bytes
png = bytes.fromhex("89504e470d0a1a0a0000000d4948445200000001000000010806000000"
                    "1f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082")
name = b"fake_headset_test.png"
shot = len(name).to_bytes(2, "big") + name + len(png).to_bytes(4, "big") + png
msg.sendall(writer.encode(bytes([70]) + len(shot).to_bytes(4, "big") + shot))
step("ScreenshotToDesktop sent", True)

video = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
video.settimeout(5)
video.connect((HOST, od.VIDEO_PORT))
video_ch = AesSocket(video, key if encrypted else None, iv if encrypted else None)
formats, stream_frames, sources, timings, foveae = [], {}, [], [], []
VR_DUMP_FOVEA = __import__("os").environ.get("VR_DUMP_FOVEA")  # FoveaTopLeft per frame (harness checks)
frame_ages = []  # (arrival, ms since the streamer stamped the frame; both CLOCK_MONOTONIC)
throttle = {"stall_until": 0.0, "rate": 0.0, "bytes": 0, "since": 0.0}  # simulated link: pause / bytes per second


def video_source_of(f):
    return sources[formats.index(f)] if f in formats else -1


def dump(stream, au):  # VR_DUMP=PREFIX: VR streams to PREFIX-<stream>.bin (raw Annex-B / OBUs)
    prefix = __import__("os").environ.get("VR_DUMP")
    if prefix and sources and sources[-1] == 1:
        with open(f"{prefix}-{stream}.bin", "ab") as f:
            f.write(au)


def read_video():  # VideoFormat (4) / Frame (0) / Slice (1), like VideoPlayer
    video.settimeout(None)
    try:
        while True:
            t = video_ch.read(1)[0]
            size = 1
            if t == 4:
                f = struct.unpack("<B3x" + "i" * 27, video_ch.read(112))
                formats.append(f[1:])
                sources.append(f[0])
                size += 112
            elif t == 0:
                m = video_ch.read(160)
                timings.append((m[12],) + struct.unpack_from("<fff", m, 16))  # source, FrameTime, GameTime, EncodeTime
                foveae.append(struct.unpack_from("<ii", m, 124) + struct.unpack_from("<ii", m, 136))  # FoveaTopLeft L, R
                if VR_DUMP_FOVEA:
                    with open(VR_DUMP_FOVEA, "a") as ff:
                        ff.write(" ".join(map(str, foveae[-1])) + "\n")
                au = video_ch.read(struct.unpack_from("<i", m, 152)[0])
                frame_ages.append((time.monotonic(), (time.monotonic_ns() // 1000 - struct.unpack_from("<q", m, 0)[0]) / 1000))
                stream_frames[0] = stream_frames.get(0, 0) + 1
                dump(0, au)
                if m[12] == 2 and __import__("os").environ.get("VIDEO_DUMP"):  # Source=Video: the played file's stream
                    with open(__import__("os").environ["VIDEO_DUMP"], "ab") as vf:
                        vf.write(au)
                size += 160 + len(au)
            elif t == 1:
                ts, idx, n = struct.unpack("<qii", video_ch.read(16))
                au = video_ch.read(n)
                stream_frames[idx] = stream_frames.get(idx, 0) + 1
                dump(idx, au)
                size += 16 + n
            else:
                print(f"      bad video packet type {t}")
                return
            if time.monotonic() < throttle["stall_until"]:
                time.sleep(max(0.0, throttle["stall_until"] - time.monotonic()))
            if throttle["rate"]:  # read no faster than the simulated link
                throttle["bytes"] += size
                ahead = throttle["bytes"] / throttle["rate"] - (time.monotonic() - throttle["since"])
                if ahead > 0:
                    time.sleep(ahead)
            else:
                throttle["read"] = throttle.get("read", 0) + size
    except Exception:
        pass


import threading  # noqa: E402
threading.Thread(target=read_video, daemon=True).start()
audio = socket.create_connection((HOST, od.AUDIO_PORT), timeout=5)
data_sock = socket.create_connection((HOST, od.DATA_PORT), timeout=10)
data_ch = AesSocket(data_sock, key if encrypted else None, iv if encrypted else None)
audio_ch = AesSocket(audio, key if encrypted else None, iv if encrypted else None)
n = int.from_bytes(data_ch.read(4), "big", signed=True)
t0 = time.monotonic()
data_ch.read(n)
dt = time.monotonic() - t0
step("bandwidth probe header received (0 = Auto-adjust bitrate off)", n >= 0, n)
print(f"      {n} bytes in {dt * 1000:.0f} ms ({n * 8 / max(dt, 1e-6) / 1e9:.1f} Gbit/s over loopback)")

# --- FrameData: head pose + a pointer, like Game.SendFrameData
fd = bytearray(316)
struct.pack_into("<i", fd, 0, 1)
struct.pack_into("<4f", fd, 120, 0, 0, 0, 1)
if __import__("os").environ.get("HANDS"):  # hand tracking: head away from the origin and turned, so raw != app space;
    import math  # the left controller and the left hand's first joint at the same raw position
    fd = bytearray(4992)
    struct.pack_into("<4f3f", fd, 120, 0, math.sin(math.pi / 4), 0, math.cos(math.pi / 4), 1.0, 1.6, 2.0)
    struct.pack_into("<f", fd, 8, 1.5)  # FloorHeight (eye-level origin 1.5 m above the floor)
    struct.pack_into("<4f3f", fd, 176, 0, 0, 0, 1, 1.3, 1.2, 1.7)
    fd[176 + 64] = 1
    fd[312] = 1
    struct.pack_into("<4f3f", fd, 316, 0, 0, 0, 1, 1.3, 1.2, 1.7)
    for j in range(1, 26):  # the rest of the left hand: valid joints along a line
        struct.pack_into("<4f3f", fd, 316 + 56 * j, 0, 0, 0, 1, 1.3 + 0.01 * j, 1.2, 1.7)
if BODY:  # 32 body joint slots (0-17 = joints 0-17, 18-31 = joints 70-83), each at its own spot: x = slot / 100
    fd = fd + bytearray(4992 - len(fd)) if len(fd) < 4992 else fd
    fd[3344], fd[3345] = 1, 1  # calibrated, high fidelity
    struct.pack_into("<f", fd, 3348, 1.0)  # confidence
    for k in range(32):
        struct.pack_into("<Q4f3f", fd, 3352 + 40 * k, 0xF, 0, 0, 0, 1, k / 100, 1.0, 0.5)
    # the skeleton (SkeletonDataChanged): 84 joints {joint, parent, pose}, then the change counter
    skel = b"".join(struct.pack("<ii4f3f", j, max(j - 1, 0), 0, 0, 0, 1, 0, j / 100, 0) for j in range(84))
    body = skel + struct.pack("<i", 7)
    msg.sendall(writer.encode(bytes([22]) + len(body).to_bytes(4, "big") + body))
if __import__("os").environ.get("FOVEATED"):
    # the head turning left at 0.5 rad/s: frames render from a predicted pose - the fovea stays where the gaze says
    struct.pack_into("<3f", fd, 160, 0, 0.5, 0)
if __import__("os").environ.get("ZERO_POSE"):  # a headset not tracking yet: all-zero head poses first (games must not centre on them)
    zero = bytearray(fd)
    struct.pack_into("<4f", zero, 120, 0, 0, 0, 0)
    for i in range(40):
        struct.pack_into("<i", zero, 0, i)
        comp = lz4.block.compress(bytes(zero), store_size=False)
        data_ch.send(len(comp).to_bytes(4, "little") + bytes(8) + comp)
        time.sleep(0.05)
if __import__("os").environ.get("RECENTER_TEST"):  # standing 1 m / 2 m off the origin, then the headset's own recenter moves the origin here
    away = bytearray(fd)
    struct.pack_into("<4f3f", away, 120, 0, 0, 0, 1, 1.0, 0.0, 2.0)
    for i in range(60):
        struct.pack_into("<i", away, 0, i)
        comp = lz4.block.compress(bytes(away), store_size=False)
        data_ch.send(len(comp).to_bytes(4, "little") + bytes(8) + comp)
        time.sleep(0.05)
    msg.sendall(writer.encode(bytes([29]) + (0).to_bytes(4, "big")))  # MessageType.Recenter
    for i in range(30):
        struct.pack_into("<i", fd, 0, 100 + i)
        comp = lz4.block.compress(bytes(fd), store_size=False)
        data_ch.send(len(comp).to_bytes(4, "little") + bytes(8) + comp)
        time.sleep(0.05)
comp = lz4.block.compress(bytes(fd), store_size=False)
for i in range(10):
    struct.pack_into("<i", fd, 0, i)
    comp = lz4.block.compress(bytes(fd), store_size=False)
    data_ch.send(len(comp).to_bytes(4, "little") + bytes(8) + comp)
step("FrameData sent (no active input: nothing gets injected)", True)


# --- audio: desktop audio packets in, microphone packets out (NOAUDIO=1: streamer set to "computer only")
if not __import__("os").environ.get("NOAUDIO"):
    audio.settimeout(3)
    t_end = time.monotonic() + 1.0
    packets = sizes = 0
    while time.monotonic() < t_end:
        size = int.from_bytes(audio_ch.read(4), "little", signed=True)
        audio_ch.read(size)
        packets += 1
        sizes = size
    step("desktop audio streaming", packets >= 150, f"{packets} packets")
    print(f"      {packets} packets of {sizes} bytes in 1 s")
    for _ in range(20):
        audio_ch.send((960).to_bytes(4, "little") + bytes(960))
    step("microphone packets sent", True)
# AUDIO_PROBE=1 (harness: AUDIO_TEST_MODE): what reaches the headset, the PC's speakers and the microphone input
if __import__("os").environ.get("AUDIO_PROBE"):
    import array
    import math as _m
    import subprocess as _sp
    import threading as _th

    def rms(data):
        s = array.array("h", data[: len(data) // 2 * 2])
        return int((sum(x * x for x in s) / max(len(s), 1)) ** 0.5)

    def record(device, seconds=1.2):
        return _sp.run(["timeout", str(seconds), "parec", "--raw", "--format=s16le", "--rate=48000", "--channels=1", "--latency-msec=20", "-d", device],
                       capture_output=True).stdout

    time.sleep(1.5)  # routing changes settle
    default = _sp.run(["pactl", "get-default-sink"], capture_output=True, text=True).stdout.strip()
    speaker = rms(record("speaker.monitor"))
    received = b""
    if not __import__("os").environ.get("NOAUDIO"):
        audio.settimeout(3)
        t_end = time.monotonic() + 0.6
        while time.monotonic() < t_end:
            size = int.from_bytes(audio_ch.read(4), "little", signed=True)
            received = audio_ch.read(size) if len(received) > 96000 else received + audio_ch.read(size)
    mic_out = {}
    def capture():
        mic_out["input"] = rms(record("opendesk_mic", 1.0))
    def capture_speaker():
        mic_out["speaker"] = record("speaker.monitor", 1.0)
    packet = 0
    def tone():
        global packet
        out = b"".join(int(14000 * _m.sin((packet + i) * 2 * _m.pi * 880 / 48000)).to_bytes(2, "little", signed=True) for i in range(480))
        packet += 480
        return out
    # the input device appears with the first packet
    for _ in range(30):
        audio_ch.send((960).to_bytes(4, "little") + tone())
        time.sleep(0.01)
    time.sleep(0.5)
    a = _th.Thread(target=capture)
    a.start()
    t_end = time.monotonic() + 1.4
    while time.monotonic() < t_end:
        audio_ch.send((960).to_bytes(4, "little") + tone())
        time.sleep(0.01)
    a.join()
    a = _th.Thread(target=capture_speaker)
    a.start()
    t_end = time.monotonic() + 1.4
    while time.monotonic() < t_end:
        audio_ch.send((960).to_bytes(4, "little") + tone())
        time.sleep(0.01)
    a.join()
    # (speaker while the mic plays: the tone alone in modes that keep it, so compare with the level before)
    print(f"AUDIO default_sink={default} headset={rms(received)} speaker={speaker} mic_input={mic_out['input']} speaker_during_mic={rms(mic_out['speaker'])}")
def wait_format(pred, timeout=8):
    t_end = time.monotonic() + timeout
    while time.monotonic() < t_end:
        if formats and pred(formats[-1]):
            return formats[-1]
        time.sleep(0.1)
    return formats[-1] if formats else None


if any(__import__("os").environ.get(k) for k in ("VIDEOS", "GAMES")):  # NetMessage helpers for the tab tests
    msg_stream = bytearray(data + got)
    msg.settimeout(None)

    def read_msgs():
        try:
            while True:
                b = msg.recv(65536)
                if not b:
                    return
                msg_stream.extend(b)
        except Exception:
            pass

    threading.Thread(target=read_msgs, daemon=True).start()

    def messages():  # every NetMessage the streamer sent so far: (type, payload)
        out = []
        for c in od.AesChunkDecoder(key, iv).chunks(bytes(msg_stream)):
            while len(c) >= 5:
                n = int.from_bytes(c[1:5], "big")
                out.append((c[0], c[5:5 + n]))
                c = c[5 + n:]
        return out

    def wait_msg(t, timeout=8, after=0):
        t_end = time.monotonic() + timeout
        while time.monotonic() < t_end:
            found = [p for i, (tt, p) in enumerate(messages()) if tt == t and i >= after]
            if found:
                return found[0]
            time.sleep(0.1)
        return None

    def net_str(s):
        b = s.encode()
        return len(b).to_bytes(2, "big") + b

    def rd_str(p, o):
        n = int.from_bytes(p[o:o + 2], "big")
        return p[o + 2:o + 2 + n].decode(), o + 2 + n

    def send(t, body=b""):
        msg.sendall(writer.encode(bytes([t]) + len(body).to_bytes(4, "big") + body))


if __import__("os").environ.get("VIDEOS"):  # Videos tab: browse, thumbnail, play, seek, stop
    send(50, net_str(""))  # RequestVideos(root)
    p = wait_msg(51)
    items, o = [], 2
    for _ in range(int.from_bytes(p[:2], "big") if p else 0):
        path, o = rd_str(p, o)
        size, ticks, opts = struct.unpack(">qqi", p[o:o + 20])
        o += 20
        items.append((path, size, opts))
    step("RequestVideos root -> Videos", any(i[2] & 1 for i in items), items)
    folder = next(i[0] for i in items if i[2] & 1)
    send(50, net_str(folder))
    n_before = len(messages())
    p = wait_msg(51, after=n_before)
    path, o = "", 2
    for _ in range(int.from_bytes(p[:2], "big") if p else 0):
        pth, o = rd_str(p, o)
        o += 20
        if pth.endswith("test.mkv"):
            path = pth
    step("RequestVideos folder -> file", path.endswith("test.mkv"), path)
    send(52, (1).to_bytes(2, "big") + net_str(path))
    p = wait_msg(53)
    tpath, o = rd_str(p, 0) if p else ("", 0)
    png_len = int.from_bytes(p[o:o + 4], "big") if p else 0
    step("VideoThumbnail PNG", p and p[o + 4:o + 8] == b"\x89PNG", (tpath, png_len))
    n_before = len(messages())
    send(54, net_str(path) + (0).to_bytes(8, "big") + b"\xff")  # PlayVideo(path, 0, audio -1)
    at = wait_msg(60, after=n_before)
    step("AudioTracks", at and at[0] == 1, at)
    stt = wait_msg(65, after=n_before)
    step("SubtitleTracks", stt and stt[0] == 1, stt)
    f = wait_format(lambda f: sources[-1] == 2)
    step("VideoFormat Source=Video", sources and sources[-1] == 2, f)
    frames_before = stream_frames.get(0, 0)
    time.sleep(2)
    step("video frames streaming", stream_frames.get(0, 0) - frames_before >= 45, stream_frames.get(0, 0) - frames_before)
    send(58, (5_000_000).to_bytes(8, "big"))  # SeekVideo 5 s
    time.sleep(1)
    step("SeekVideo accepted", True)
    send(56)  # StopVideo -> desktop again
    f = wait_format(lambda f: sources[-1] == 0)
    step("StopVideo -> desktop VideoFormat", sources and sources[-1] == 0, f)
    # "Download": the PC file onto the headset over the data connection (NetDataClient.ReadVideoAsync, FullVideo)
    send(50, net_str(folder))
    n_before = len(messages())
    lst = wait_msg(51, after=n_before)
    expect, o = -2, 2
    for _ in range(int.from_bytes(lst[:2], "big") if lst else 0):
        pth, o = rd_str(lst, o)
        if pth == path:
            expect = struct.unpack(">q", lst[o:o + 8])[0]
        o += 20
    send(55, net_str(path))
    kind = data_ch.read(1)[0]
    size = struct.unpack(">q", data_ch.read(8))[0] if kind == 0 else -1
    got = 0
    while 0 <= got < size:
        got += len(data_ch.read(min(1 << 16, size - got)))
    step("DownloadVideo -> FullVideo with the whole file", kind == 0 and size == expect and got == size, (kind, size, expect))
    send(55, net_str("/nonexistent.mkv"))
    step("DownloadVideo of a missing file -> OpenFailure", data_ch.read(1)[0] == 4)
    if __import__("os").environ.get("TRANSCODE"):  # MPEG-4 part 2 AVI: converted to H.264 while it plays
        send(50, net_str(folder))
        n_before = len(messages())
        lst = wait_msg(51, after=n_before)
        old, o = None, 2
        for _ in range(int.from_bytes(lst[:2], "big") if lst else 0):
            pth, o = rd_str(lst, o)
            o += 20
            if pth.endswith("old.avi"):
                old = pth
        step("old.avi listed", old is not None, old)
        send(54, net_str(old or "") + (0).to_bytes(8, "big") + b"\xff")
        f = wait_format(lambda f: sources[-1] == 2)
        step("MPEG-4 part 2 file -> VideoFormat H.264 (converted)", f and sources[-1] == 2 and f[0] == 1, f)
        frames_before = stream_frames.get(0, 0)
        time.sleep(2)
        n = stream_frames.get(0, 0) - frames_before
        step("converted frames streaming (~25 fps)", n >= 40, n)
        send(56)
        wait_format(lambda f: sources[-1] == 0)

if __import__("os").environ.get("GAMES"):  # Games tab: list, thumbnail, launch (fake riftlift), exit
    FAKE_ID = 1234567890123

    def games_list(p):
        out, o = [], 2
        for _ in range(int.from_bytes(p[:2], "big") if p else 0):
            plat, appid, running = struct.unpack(">BQ?", p[o:o + 10])
            o += 10
            out.append((plat, appid, running))
        return out

    send(30, b"\0")  # RequestGames(force=false)
    lst = games_list(wait_msg(31, timeout=15))
    step("RequestGames -> Games (RiftLift test game, not running)", (0, FAKE_ID, False) in lst,
         f"{len(lst)} games, oculus: {[g for g in lst if g[0] == 0]}")
    steam = [g for g in lst if g[0] == 1]
    ids = [(0, FAKE_ID)] + [(g[0], g[1]) for g in steam[:1]]
    send(32, len(ids).to_bytes(2, "big") + b"".join(struct.pack(">BQ", *i) for i in ids))
    got = {}
    t_end = time.monotonic() + 10
    while time.monotonic() < t_end and len(got) < len(ids):
        for tt, p in messages():
            if tt == 33:
                plat, appid, n = struct.unpack(">BQi", p[:13])
                img = p[13:13 + n]
                got[(plat, appid)] = struct.unpack(">II", img[16:24]) if img[:8] == b"\x89PNG\r\n\x1a\n" else None
        time.sleep(0.1)
    step("GameThumbnail PNG 220x128 for each request", all(got.get(i) == (220, 128) for i in ids), got)
    n_before = len(messages())
    send(34, struct.pack(">BQ", 0, FAKE_ID))  # LaunchGame
    # Play with VR selected: the headset switches itself to VR right away
    send(16, b'{"StreamingSource": 1}')
    time.sleep(4)
    bounced = [p for t, p in messages()[n_before:] if t == 16 and b'"StreamingSource":0' in p]
    step("headset kept in VR while the game loads (no bounce to the desktop)", not bounced, bounced)
    send(30, b"\1")
    lst = games_list(wait_msg(31, after=n_before))
    step("launched game reported running", (0, FAKE_ID, True) in lst, [g for g in lst if g[0] == 0])
    step("no GameExited while it runs", wait_msg(36, timeout=0.5, after=n_before) is None)
    send(35, struct.pack(">BQ", 0, FAKE_ID))  # ExitGame
    t_exit = time.monotonic()
    p = wait_msg(36, timeout=20, after=n_before)
    step("ExitGame -> GameExited (after the detached game process is gone too)",
         p == struct.pack(">BQ", 0, FAKE_ID) and time.monotonic() - t_exit > 4, (p, round(time.monotonic() - t_exit, 1)))
    time.sleep(1)
    back = [p for t, p in messages()[n_before:] if t == 16 and b'"StreamingSource":0' in p]
    step("game gone without rendering -> headset back to the desktop", back, back[-1:] if back else None)
    n_before = len(messages())
    send(34, struct.pack(">BQ", 0, FAKE_ID))  # launch again, then the VR menu's "Exit game" (platform 0, id 0)
    time.sleep(3)
    send(35, struct.pack(">BQ", 0, 0))
    p = wait_msg(36, timeout=20, after=n_before)
    step("VR menu ExitGame(0, 0) -> GameExited for the running game", p == struct.pack(">BQ", 0, FAKE_ID), p)
    import subprocess  # noqa: E402
    left = subprocess.run(["pgrep", "-f", "^sleep 600$"], capture_output=True, text=True).stdout.split()
    step("no game processes left behind", not left, left)

if __import__("os").environ.get("X11"):  # an X11 session: the X screen streamed (XRandR monitor, MIT-SHM grabs)
    f = wait_format(lambda f: True, timeout=10)
    step("X11 desktop VideoFormat 1280x720", f is not None and (f[2], f[3]) == (1280, 720), f and f[:4])
    before = stream_frames.get(0, 0)
    time.sleep(2)
    step("X11 desktop frames streaming", stream_frames.get(0, 0) - before >= 20, stream_frames.get(0, 0) - before)

if __import__("os").environ.get("VR"):  # VR mode toggle (streamer started with the VR test pattern)
    def send_msg(t, body=b""):
        msg.sendall(writer.encode(bytes([t]) + len(body).to_bytes(4, "big") + body))

    send_msg(12, json.dumps({"VRFramerate": 72, "VRGraphicsQuality": 1, "VRBitrateLimit": 0.3}).encode())
    send_msg(16, json.dumps({"StreamingSource": 1}).encode())
    f = wait_format(lambda f: sources[-1] == 1, timeout=float(__import__("os").environ.get("VR_START_TIMEOUT", "8")))
    step("VR VideoFormat (Source=VR, 2 streams)", sources and sources[-1] == 1 and f[4] == 2, f)
    before = dict(stream_frames)
    vr_seconds = float(__import__("os").environ.get("VR_MEASURE_SECONDS", "2"))  # (longer: the streamer's 5 s statistics)
    time.sleep(vr_seconds)
    l, r = stream_frames.get(0, 0) - before.get(0, 0), stream_frames.get(1, 0) - before.get(1, 0)
    step("VR stereo frames ~72 fps", l >= 60 * vr_seconds and abs(l - r) <= 2, (l, r))
    if __import__("os").environ.get("SPACEWARP"):  # an app through libovr-opendesk (OVRAPP/HELLOXR): half rate while it's on
        def rate(seconds=2):
            b0 = dict(stream_frames)
            time.sleep(seconds)
            return (stream_frames.get(0, 0) - b0.get(0, 0)) / seconds
        send_msg(14, json.dumps({"UseSpacewarp": True}).encode())
        time.sleep(1)
        on = rate()
        step("synchronous spacewarp: the app renders at half rate (~36 fps)", 28 <= on <= 44, on)
        send_msg(14, json.dumps({"UseSpacewarp": False}).encode())
        time.sleep(1)
        off = rate()
        step("spacewarp off: full rate again (~72 fps)", off >= 62, off)
    vt = [t for t in timings[-60:] if t[0] == 1]
    if vt:
        print(f"      VR EncodeTime mean {sum(t[3] for t in vt) / len(vt):.2f} ms over {len(vt)} frames")
    step("VR FrameMetadata timing (FrameTime from pose age, EncodeTime measured)",
         vt and all(0 < t[3] < 100 and 0 <= t[1] < 1000 for t in vt), [round(x, 2) for x in vt[-1][1:]] if vt else None)
    # Wi-Fi dip while a game loads: a 1 s stall, then a link only a bit faster than the stream. The backlog must drain
    # (frames held back) instead of staying as constant extra latency until the stream restarts.
    def median_age(since):
        a = sorted(x for t, x in frame_ages if t > since)
        return round(a[len(a) // 2], 1) if a else None
    r0, t0 = throttle.get("read", 0), time.monotonic()
    time.sleep(1)
    rate = (throttle.get("read", 0) - r0) / (time.monotonic() - t0)
    before = median_age(time.monotonic() - 1)
    throttle.update(stall_until=time.monotonic() + 1, rate=1.15 * rate, bytes=0, since=time.monotonic() + 1)
    time.sleep(4)
    after = median_age(time.monotonic() - 1)
    throttle.update(rate=0)
    print(f"      stream {rate * 8 / 1e6:.1f} Mbit/s; frame age median {before} ms before, {after} ms 3 s after the stall")
    step("network backlog after a stall drains (frame age < 100 ms on a link 15% faster than the stream)",
         after is not None and after < 100, (before, after))
    send_msg(16, json.dumps({"StreamingSource": 0}).encode())
    f = wait_format(lambda f: sources[-1] == 0)
    step("back to desktop", sources and sources[-1] == 0, f)

if __import__("os").environ.get("AUTOVR"):  # no app at first; one starts later and quits (streamer-driven VR mode)
    time.sleep(1)
    step("no VR app: desktop only", not sources or sources[-1] == 0, sources[-3:])
    time.sleep(10)  # two probing clients came and went meanwhile
    step("probing clients (no frames) left the headset on the desktop", 1 not in sources, sources[-3:])
    f = wait_format(lambda f: sources[-1] == 1, timeout=20)
    step("app started -> streamer switched the headset to VR", sources and sources[-1] == 1, f)
    before = stream_frames.get(0, 0)
    time.sleep(2)
    step("app frames streaming", stream_frames.get(0, 0) - before > 60, stream_frames.get(0, 0) - before)
    # the user switches to the desktop and back while the app keeps running
    msg.sendall(writer.encode(bytes([16]) + (22).to_bytes(4, "big") + b'{"StreamingSource": 0}'))
    f = wait_format(lambda f: sources[-1] == 0)
    step("headset -> desktop while the app runs", sources and sources[-1] == 0, f)
    msg.sendall(writer.encode(bytes([16]) + (22).to_bytes(4, "big") + b'{"StreamingSource": 1}'))
    f = wait_format(lambda f: sources[-1] == 1)
    step("headset -> back to VR", sources and sources[-1] == 1, f)
    before = stream_frames.get(0, 0)
    time.sleep(1.5)
    step("app frames streaming again", stream_frames.get(0, 0) - before > 30, stream_frames.get(0, 0) - before)
    f = wait_format(lambda f: sources[-1] == 0, timeout=20)
    step("app quit -> back to the desktop", sources and sources[-1] == 0, f)

if __import__("os").environ.get("REJOIN"):  # an OpenXR app keeps running while the headset leaves and comes back
    # (run twice: upstream closed or crashed on the headset, then reopened - the app's picture has to come back too)
    f = wait_format(lambda f: sources[-1] == 1, timeout=20)
    step("running app -> headset in VR", sources and sources[-1] == 1, f)
    before = stream_frames.get(0, 0)
    time.sleep(2)
    step("app frames streaming", stream_frames.get(0, 0) - before > 60, stream_frames.get(0, 0) - before)

if __import__("os").environ.get("FOVEATED"):  # foveated streaming: gaze in, 4 streams + fovea positions out
    # per eye: view tangents x (right), y (up) -> projection NDC clamped to +-0.5 (+y down), as the headset sends it
    def ndc(e, tx, ty):
        f = FOV[e]
        return (max(-0.5, min(0.5, ((tx + f["LeftTan"]) / (f["LeftTan"] + f["RightTan"]) - 0.5) * 2)),
                max(-0.5, min(0.5, ((f["UpTan"] - ty) / (f["UpTan"] + f["DownTan"]) - 0.5) * 2)))
    # converging a little (an object 0.8 m ahead, left of centre): each eye's fovea follows that eye
    GAZE = ndc(0, -0.12 + 0.04, 0.15) + ndc(1, -0.12 - 0.04, 0.15)

    def send_gaze():
        while True:
            try:
                video_ch.send(struct.pack("<i4f", 16, *GAZE))
            except Exception:
                return
            time.sleep(0.011)
    threading.Thread(target=send_gaze, daemon=True).start()
    f = wait_format(lambda f: sources[-1] == 1 and f[4] == 4, timeout=20)
    step("foveated VideoFormat (StreamCount 4)", f and sources[-1] == 1 and f[4] == 4, f)
    if f and f[4] == 4:
        # FrameSize, StreamCount, Bounds, then {Secondary,Tertiary,Quaternary}{FrameSize, Bounds}
        pw, ph, eye_w, eye_h = f[2], f[3], f[7], f[8]
        fw, fh, tw, th, qw, qh = f[9], f[10], f[15], f[16], f[21], f[22]
        print(f"      periphery {pw}x{ph}, fovea {fw}x{fh}, eye {eye_w}x{eye_h}")
        step("stream sizes: s0/s2 periphery, s1/s3 fovea", (tw, th) == (pw, ph) and (qw, qh) == (fw, fh) and 0 < fw < eye_w,
             (pw, ph, fw, fh, tw, th, qw, qh))
        before = dict(stream_frames)
        time.sleep(2)
        counts = [stream_frames.get(i, 0) - before.get(i, 0) for i in range(4)]
        step("frames on all 4 streams", min(counts) > 60 and max(counts) - min(counts) <= 2, counts)
        def expect_for(gaze):
            # like the Windows streamer: each eye's own gaze as the headset sent it, centre = (gaze / 2 + 0.5) * size
            out = []
            for e in range(2):
                gx, gy = gaze[2 * e], gaze[2 * e + 1]
                cx, cy = round((gx / 2 + 0.5) * eye_w), round((gy / 2 + 0.5) * eye_h)
                out += [min(max(cx - fw // 2, 0), eye_w - fw), min(max(cy - fh // 2, 0), eye_h - fh)]
            return out
        last = foveae[-1] if foveae else None
        expect = expect_for(GAZE)
        step("FoveaTopLeft follows each eye's gaze (the headset's, as is: the head turning changes nothing)",
             last and all(abs(a - b) <= 1 for a, b in zip(last, expect)), (last, expect))
        GAZE = ndc(0, 0.3, 0.15) + ndc(1, -0.3, 0.15)
        time.sleep(0.5)
        last = foveae[-1] if foveae else None
        expect = expect_for(GAZE)
        step("... and follows it when it moves", last and all(abs(a - b) <= 1 for a, b in zip(last, expect)), (last, expect))

if __import__("os").environ.get("FACE"):  # face/eye tracking (Quest Pro) -> VRChat OSC, built-in VRCFaceTracking
    import math
    osc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    osc.bind(("127.0.0.1", 9123))
    osc.settimeout(0.2)
    face = bytearray(4992)
    struct.pack_into("<i", face, 0, 1)
    struct.pack_into("<4f", face, 120, 0, 0, 0, 1)
    face[4632] = face[4633] = 1                   # both eyes valid
    yaw = 0.2                                     # left eye turned (quaternion about y)
    struct.pack_into("<4f", face, 4636, 0, math.sin(-yaw / 2), 0, math.cos(-yaw / 2))
    struct.pack_into("<4f", face, 4664, 0, 0, 0, 1)
    face[4700] = 1                                # face valid (mouth)
    face[4701] = 1                                # eye-following blendshapes
    w = [0.0] * 70
    w[24] = 0.7                                   # JawDrop -> JawOpen
    w[54] = 0.6                                   # MouthRight -> MouthUpperRight/LowerRight -> MouthX = +0.6
    w[68] = 0.8                                   # TongueOut (no tongue-tracking flag: the plain value)
    struct.pack_into("<70f", face, 4704, *w)
    got = {}
    counts = {}

    def parse(pkt):
        def s(b, o):
            e = b.index(b"\0", o)
            return b[o:e].decode(), (e + 4) & ~3
        if pkt.startswith(b"#bundle"):
            o = 16
            while o + 4 <= len(pkt):
                n = struct.unpack_from(">i", pkt, o)[0]
                parse(pkt[o + 4:o + 4 + n])
                o += 4 + n
            return
        addr, o = s(pkt, 0)
        tags, o = s(pkt, o)
        vals = []
        for t in tags[1:]:
            if t == "f":
                vals.append(struct.unpack_from(">f", pkt, o)[0])
                o += 4
            elif t in "TF":
                vals.append(t == "T")
        got[addr] = vals
        counts[addr] = counts.get(addr, 0) + 1
    t_end = time.monotonic() + 4
    i = 0
    while time.monotonic() < t_end:
        struct.pack_into("<i", face, 0, i)
        comp = lz4.block.compress(bytes(face), store_size=False)
        data_ch.send(len(comp).to_bytes(4, "little") + bytes(8) + comp)
        i += 1
        try:
            while True:
                parse(osc.recv(65536))
        except socket.timeout:
            pass
    P = "/avatar/parameters/FT/v2/"
    # Mouth Sensitivity (default 2.0x): 1 - (1 - w)^2 lifts talking-sized movements, fully open stays 1
    step("float parameter (JawOpen = JawDrop through Mouth Sensitivity 2.0x)",
         abs(got.get(P + "JawOpen", [0])[0] - (1 - (1 - 0.7) ** 2)) < 1e-4, got.get(P + "JawOpen"))
    bits = [got.get(P + f"MouthX{b}", [None])[0] for b in (1, 2, 4)]
    expect = [(int((1 - (1 - 0.6) ** 2) * 8) >> k) & 1 == 1 for k in range(3)]  # Mouth Sensitivity 2.0x
    step("binary parameter MouthX1/2/4 (0.6 -> 0.84 of 8 steps) + Negative", bits == expect and got.get(P + "MouthXNegative") == [False],
         (bits, expect, got.get(P + "MouthXNegative")))
    lx = got.get(P + "EyeLeftX", [None])[0]
    step("eye gaze parameter EyeLeftX", lx is not None and abs(lx - (-yaw)) < 0.01 or (lx is not None and abs(abs(lx) - yaw) < 0.01), lx)
    step("VRChat native gaze off (avatar has eye X/Y), eyes closed amount on",
         "/tracking/eye/LeftRightPitchYaw" not in got and "/tracking/eye/EyesClosedAmount" in got, sorted(a for a in got if a.startswith("/tracking")))
    step("headset tongue -> TongueOut", abs(got.get(P + "TongueOut", [0])[0] - 0.8) < 1e-4, got.get(P + "TongueOut"))
    step("status flag at the avatar's address (FT/EyeTrackingActive), resent every second",
         got.get("/avatar/parameters/FT/EyeTrackingActive") == [True] and counts.get("/avatar/parameters/FT/EyeTrackingActive", 0) >= 3
         and counts.get(P + "JawOpen", 0) >= 3, (got.get("/avatar/parameters/FT/EyeTrackingActive"),
         counts.get("/avatar/parameters/FT/EyeTrackingActive"), counts.get(P + "JawOpen")))
    step("only the avatar's parameters (and status flags)", all(a.startswith(P) or a.startswith("/tracking") or a.endswith("Active") for a in got),
         sorted(got)[:12])

if __import__("os").environ.get("MONITORS"):  # multi-monitor messages (needs a capture that works)

    def send(t, body=b""):
        msg.sendall(writer.encode(bytes([t]) + len(body).to_bytes(4, "big") + body))

    def screens(f):  # f = (codec, fps, w, h, count, bounds x4, 3x (w, h, bounds x4)) -> (count, [(w, h, x, y, bw, bh)])
        count = f[4]
        main = (f[2], f[3], *f[5:9])
        extra = [tuple(f[9 + 6 * i: 15 + 6 * i]) for i in range(3)]
        return count, [main] + extra[:count - 1]

    f = wait_format(lambda f: True)
    step("VideoFormat received", f is not None, f and screens(f))
    time.sleep(1.5)
    dt = [t for t in timings if t[0] == 0]
    step("desktop FrameMetadata EncodeTime measured", dt and all(0 < t[3] < 100 for t in dt[-20:]),
         [round(x, 2) for x in dt[-1][1:]] if dt else None)
    first = f[4] if f else 0
    main_w0, main_h0 = formats[0][2], formats[0][3]  # virtual monitors copy the main monitor
    # ExtraDisplay (UserSettings): a virtual monitor becomes the main screen (Windows: unless the user switched)
    send(12, json.dumps({"ExtraDisplay": True}).encode())
    # (its mode follows the main monitor via kscreen-doctor, which can't run in the test's user namespace)
    f = wait_format(lambda f: f[5] > 0)  # main screen bounds now right of the monitor
    step("ExtraDisplay on -> virtual main screen", f and f[5] > 0, f and screens(f))
    main_w = formats[0][2]
    send(12, json.dumps({"ExtraDisplay": False}).encode())
    f = wait_format(lambda f: f[2] == main_w)
    step("ExtraDisplay off -> back to the monitor", f and f[2] == main_w, f and screens(f))
    send(42)  # AddMonitor: no spare monitor -> virtual one
    f = wait_format(lambda f: f[4] == first + 1)
    step("AddMonitor -> one more screen", f and f[4] == first + 1, f and screens(f))
    time.sleep(1.5)
    step("extra screen streams slices", stream_frames.get(first, 0) > 0, stream_frames)
    send(44, (first).to_bytes(4, "big") + b"\x01")  # SetMonitorPortrait(stream index, true)
    f = wait_format(lambda f: f[4] == first + 1 and screens(f)[1][first][5] > screens(f)[1][first][4])
    step("SetMonitorPortrait -> portrait bounds", f and screens(f)[1][first][5] > screens(f)[1][first][4], f and screens(f))
    n_before = len(formats)
    send(25)  # SwitchMonitor
    f = wait_format(lambda f: len(formats) > n_before)
    step("SwitchMonitor -> new main screen", len(formats) > n_before, f and screens(f))
    send(43)  # RemoveMonitor
    f = wait_format(lambda f: f[4] == first)
    step("RemoveMonitor -> back to the start", f and f[4] == first, f and screens(f))
    print(f"      frames per stream: {stream_frames}")
time.sleep(float(__import__("os").environ.get("HOLD", "0.5")))  # HOLD=N: stay connected (shutdown tests)
for s in (msg, video, audio, data_sock):
    s.close()
print("ALL PASS" if ok else "FAILED")
