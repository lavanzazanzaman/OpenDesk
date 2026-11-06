"""OpenDesk wire-protocol primitives (reverse-engineered from upstream 1.34.22).

References (decompiled, work/il/apk134):
  Upstream.Net/AesStream.cs              chunk framing
  Upstream.Net/ComputerDiscoveryClient.cs discovery broadcast + reply
  Upstream.Net/NetMessage.cs             messaging framing
"""
from __future__ import annotations

import asyncio
import base64
import os
import re
from dataclasses import dataclass, field
from xml.sax.saxutils import escape

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

DISCOVERY_PORT = 38850
MESSAGING_PORT = 38810
DATA_PORT = 38820
VIDEO_PORT = 38830
AUDIO_PORT = 38840

BROADCAST_KEY = bytes([
    32, 71, 236, 94, 194, 34, 85, 255, 165, 172, 187, 150, 6, 104, 106, 57,
    57, 62, 244, 114, 75, 174, 237, 9, 48, 36, 239, 82, 57, 98, 205, 80,
])
BROADCAST_IV = bytes([82, 200, 129, 118, 144, 104, 249, 4, 62, 20, 120, 110, 20, 180, 63, 31])

PLATFORMS = {0: "Oculus", 1: "OculusQuest", 2: "Vive", 3: "Pico", 5: "Google",
             6: "PlayForDream", 7: "Apple", 8: "Steam"}

MESSAGE_TYPES = {
    1: "PeerInfo", 3: "CrashCallstack", 4: "UpdateRequired", 5: "UpdateUnavailable", 6: "UpdateInProgress",
    11: "UserSettingsReload", 12: "UserSettingsPropertyChanged", 13: "MobileSettingsReload",
    14: "MobileSettingsPropertyChanged", 15: "StreamerSettingsReload", 16: "StreamerSettingsPropertyChanged",
    21: "BoundaryDataChanged", 22: "SkeletonDataChanged", 23: "RestartStream", 24: "ForceKeyFrame",
    25: "SwitchMonitor", 26: "ToggleDictation", 27: "LaunchStreamVR", 28: "ExitSteamVR", 29: "Recenter",
    30: "RequestGames", 31: "Games", 32: "RequestGameThumbnails", 33: "GameThumbnail", 34: "LaunchGame",
    35: "ExitGame", 36: "GameExited", 40: "ShowDesktop", 41: "DisposeGamepadEmulation", 42: "AddMonitor",
    43: "RemoveMonitor", 44: "SetMonitorPortrait", 50: "RequestVideos", 51: "Videos",
    52: "RequestVideoThumbnails", 53: "VideoThumbnail", 54: "PlayVideo", 55: "DownloadVideo", 56: "StopVideo",
    57: "VideoStopped", 58: "SeekVideo", 59: "CancelDownloadVideo", 60: "AudioTracks",
    61: "SetAudioStreamIndex", 62: "PasteUrl", 63: "CancelPasteUrl", 64: "VideoAdded", 65: "SubtitleTracks",
    66: "SetSubtitleTrackId", 67: "SetSubtitleStreamIndex", 68: "SetSubtitleOffsetAmount",
    70: "ScreenshotToDesktop", 71: "Exit", 72: "EnableVRPassthrough", 73: "DisableVRPassthrough",
    74: "ToggleVRPassthrough", 75: "ToggleHandPassthrough", 76: "ToggleDeskPassthrough",
    77: "TogglePerformanceOverlay", 79: "ToggleFoveatedStreaming", 80: "ChangeLanguage", 81: "Disconnect",
    82: "RequestCursor", 83: "Cursor", 90: "WakeUpComputer", 91: "ComputerAwaken", 100: "Log",
    101: "ClipboardChanged", 102: "ClipboardRequestText", 103: "ClipboardRequestImage",
    104: "ClipboardRequestFileDescriptors", 105: "ClipboardRequestFiles", 106: "ClipboardText",
    107: "ClipboardImage", 108: "ClipboardFileDescriptors", 109: "ClipboardFiles", 110: "ClipboardCopying",
    111: "ClipboardCopyingComplete", 112: "ClipboardLimitExceeded", 113: "ClipboardCopied",
}

MSG_STREAMER_SETTINGS_RELOAD = 15
MSG_STREAMER_SETTINGS_PROPERTY_CHANGED = 16


# --- AesStream ---------------------------------------------------------------------------------

def write_varint(n: int) -> bytes:
    out = bytearray()
    while n >= 0x80:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    out.append(n)
    return bytes(out)


def read_varint(buf: bytes, pos: int) -> tuple[int, int]:
    value = shift = 0
    while True:
        if shift == 35:
            raise ValueError("bad 7-bit length")
        b = buf[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return value, pos


class AesStreamWriter:
    """One Write() = varint(len) + payload + random pad to 16, CBC chained across writes."""

    def __init__(self, key: bytes, iv: bytes):
        self._enc = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()

    def encode(self, payload: bytes) -> bytes:
        block = write_varint(len(payload)) + payload
        block += os.urandom(-len(block) % 16)
        return self._enc.update(block)


class AesChunkDecoder:
    """Decodes a buffer holding whole AesStream chunks (UDP datagrams)."""

    def __init__(self, key: bytes, iv: bytes):
        self._dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()

    def chunks(self, data: bytes) -> list[bytes]:
        plain = self._dec.update(data[:len(data) // 16 * 16])
        out, pos = [], 0
        while pos + 16 <= len(plain):
            start = pos
            length, pos = read_varint(plain, pos)
            out.append(plain[pos:pos + length])
            pos = start + ((pos - start) + length + 15) // 16 * 16
        return out


class AesStreamReader:
    """Byte-stream view over an AesStream carried on an asyncio StreamReader (TCP)."""

    def __init__(self, reader: asyncio.StreamReader, key: bytes, iv: bytes):
        self._reader = reader
        self._dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
        self._buf = bytearray()

    async def _next_chunk(self) -> None:
        first = self._dec.update(await self._reader.readexactly(16))
        length, pos = read_varint(first, 0)
        total = (pos + length + 15) // 16 * 16
        rest = self._dec.update(await self._reader.readexactly(total - 16)) if total > 16 else b""
        self._buf += (first + rest)[pos:pos + length]

    async def read_exactly(self, n: int) -> bytes:
        while len(self._buf) < n:
            await self._next_chunk()
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out


# --- Discovery ---------------------------------------------------------------------------------

@dataclass
class DiscoveryRequest:
    rsa_xml: str
    platform: int
    account_id: str

    @property
    def platform_name(self) -> str:
        return PLATFORMS.get(self.platform, str(self.platform))


def parse_discovery_request(data: bytes) -> DiscoveryRequest:
    # Client writes: RSA xml, byte 0, byte platform, AccountID (each a separate AesStream chunk).
    chunks = AesChunkDecoder(BROADCAST_KEY, BROADCAST_IV).chunks(data)
    return DiscoveryRequest(chunks[0].decode("ascii"), chunks[2][0], chunks[3].decode("utf-8"))


def rsa_public_key_from_xml(xml: str) -> rsa.RSAPublicKey:
    def field_int(tag: str) -> int:
        m = re.search(rf"<{tag}>([^<]+)</{tag}>", xml)
        if not m:
            raise ValueError(f"missing {tag} in RSA key xml")
        return int.from_bytes(base64.b64decode(m.group(1)), "big")

    return rsa.RSAPublicNumbers(field_int("Exponent"), field_int("Modulus")).public_key()


@dataclass
class NetworkAdapter:
    ip: str
    mac: str = ""
    is_wireless: bool = False
    is_gigabit: bool = False
    is_usb: bool = False


@dataclass
class ComputerInfo:
    id: str
    name: str
    description: str
    adapters: list[NetworkAdapter]
    connection_id: bytes
    key: bytes
    iv: bytes
    # The headset refuses a streamer with a higher minor version. 1.20.3 is the lowest Windows
    # version it does not reject as a manual update, so it connects to every app from 1.20.3 on.
    version: tuple[int, int, int, int] = (1, 20, 3, 0)
    os: str = "Windows"            # OS enum only has Windows/MacOS
    region: str = "Europe"
    allow_remote: bool = False
    encrypt_local: bool = False
    encrypt_remote: bool = True
    extra: dict = field(default_factory=dict)

    def to_xml(self) -> bytes:
        """DataContractSerializer output; element order verified against the real assembly (tools/dcgen)."""
        b = lambda v: "true" if v else "false"  # noqa: E731
        b64 = lambda v: base64.b64encode(v).decode()  # noqa: E731
        adapters = "".join(
            "<NetworkAdapter>"
            f"<IPAddresString>{escape(a.ip)}</IPAddresString><IsGigabit>{b(a.is_gigabit)}</IsGigabit>"
            f"<IsUsb>{b(a.is_usb)}</IsUsb><IsWireless>{b(a.is_wireless)}</IsWireless>"
            f"<MACAddresString>{escape(a.mac)}</MACAddresString>"
            "</NetworkAdapter>"
            for a in self.adapters
        )
        major, minor, build, revision = self.version
        xml = (
            '<Computer xmlns="http://schemas.datacontract.org/2004/07/VirtualDesktop.Interfaces" '
            'xmlns:i="http://www.w3.org/2001/XMLSchema-instance">'
            f"<AllowRemoteConnections>{b(self.allow_remote)}</AllowRemoteConnections>"
            f"<ConnectionID>{b64(self.connection_id)}</ConnectionID>"
            f"<Description>{escape(self.description)}</Description>"
            f"<EncryptLocalTraffic>{b(self.encrypt_local)}</EncryptLocalTraffic>"
            f"<EncryptRemoteTraffic>{b(self.encrypt_remote)}</EncryptRemoteTraffic>"
            f"<ID>{escape(self.id)}</ID>"
            f"<IV>{b64(self.iv)}</IV>"
            f"<Key>{b64(self.key)}</Key>"
            f"<Name>{escape(self.name)}</Name>"
            f"<OS>{self.os}</OS>"
            f"<PrivateAdapters>{adapters}</PrivateAdapters>"
            f"<Region>{self.region}</Region>"
            '<StreamerVersion xmlns:a="http://schemas.datacontract.org/2004/07/System">'
            f"<a:_Build>{build}</a:_Build><a:_Major>{major}</a:_Major>"
            f"<a:_Minor>{minor}</a:_Minor><a:_Revision>{revision}</a:_Revision>"
            "</StreamerVersion></Computer>"
        )
        return xml.encode("utf-8")


def build_discovery_reply(req: DiscoveryRequest, computer: ComputerInfo) -> bytes:
    """RSA-OAEP-SHA1(replyKey32 + replyIV16) [128 bytes] + AesStream(replyKey, replyIV, Computer xml)."""
    reply_key, reply_iv = os.urandom(32), os.urandom(16)
    wrapped = rsa_public_key_from_xml(req.rsa_xml).encrypt(
        reply_key + reply_iv,
        padding.OAEP(mgf=padding.MGF1(hashes.SHA1()), algorithm=hashes.SHA1(), label=None),
    )
    return wrapped + AesStreamWriter(reply_key, reply_iv).encode(computer.to_xml())


# --- Messaging -----------------------------------------------------------------------------------

async def read_net_message(stream: AesStreamReader) -> tuple[int, bytes]:
    """NetMessage: u8 MessageType, i32 big-endian payload length, payload."""
    header = await stream.read_exactly(5)
    length = int.from_bytes(header[1:5], "big", signed=True)
    return header[0], await stream.read_exactly(length)


def encode_net_message(msg_type: int, payload: bytes = b"") -> bytes:
    return bytes([msg_type]) + len(payload).to_bytes(4, "big", signed=True) + payload
