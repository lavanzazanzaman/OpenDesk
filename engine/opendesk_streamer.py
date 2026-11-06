#!/usr/bin/env python3
"""Proof-of-concept Linux OpenDesk streamer: discovery + connection bring-up.

Milestone 1: answer the headset's discovery broadcast so this PC shows up in the Computers list.
Milestone 2: accept the headset's TCP connections (messaging/data/video/audio) and log what it sends.
No video is produced yet.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import socket
import subprocess
import sys
import uuid
from pathlib import Path

import opendesk_proto as od
import opendesk_audio as audio
import opendesk_input as input
import opendesk_video as video
import opendesk_vr as vr

log = logging.getLogger("vd")
JSON_EVENTS = False
BACKGROUND_TASKS: set = set()


def emit(event: str, **fields) -> None:
    """GUI bridge: one JSON object per line on stdout (logs go to stderr)."""
    if JSON_EVENTS:
        print(json.dumps({"event": event, **fields}), flush=True)
CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "OpenDesk"
STATE_FILE = CONFIG_DIR / "engine.json"
USB_NET_DRIVERS = {"cdc_ncm", "cdc_ether", "rndis_host"}
SKIP_PREFIXES = ("lo", "docker", "br-", "virbr", "veth", "tun", "tap", "wg")


def load_state() -> dict:
    state = json.loads(STATE_FILE.read_text()) if STATE_FILE.exists() else {}
    if "id" not in state:
        state["id"] = str(uuid.uuid4())
        STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        STATE_FILE.write_text(json.dumps(state, indent=2))
    return state


def sysfs(iface: str, name: str) -> str | None:
    try:
        return Path(f"/sys/class/net/{iface}/{name}").read_text().strip()
    except OSError:
        return None


def iface_driver(iface: str) -> str:
    try:
        return Path(f"/sys/class/net/{iface}/device/driver").resolve().name
    except OSError:
        return ""


def local_adapters() -> list[od.NetworkAdapter]:
    adapters = []
    for entry in json.loads(subprocess.check_output(["ip", "-j", "addr"])):
        iface = entry["ifname"]
        if iface.startswith(SKIP_PREFIXES) or "UP" not in entry.get("flags", []):
            continue
        is_usb = iface_driver(iface) in USB_NET_DRIVERS
        speed = sysfs(iface, "speed")
        mac = (entry.get("address") or "").replace(":", "").upper()
        common = dict(mac=mac, is_wireless=Path(f"/sys/class/net/{iface}/wireless").exists(),
                      is_gigabit=bool(speed and speed.lstrip("-").isdigit() and int(speed) >= 1000),
                      is_usb=is_usb)
        for addr in entry.get("addr_info", []):
            ip = addr["local"]
            if addr["family"] == "inet" and not is_usb:
                adapters.append(od.NetworkAdapter(ip=ip, **common))
            elif addr["family"] == "inet6" and is_usb and ip.startswith("fe80:"):
                # USB (NCM) link: headset uses our link-local v6 with its own scope id
                adapters.append(od.NetworkAdapter(ip=ip, **common))
    return adapters


class DiscoveryProtocol(asyncio.DatagramProtocol):
    def __init__(self, computer: od.ComputerInfo, accounts: set[str]):
        self.computer = computer
        self.accounts = accounts
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data: bytes, addr):
        host = addr[0]
        if len(data) == 17:
            if data[:16] == self.computer.connection_id:
                emit("selected", host=host, platform=data[16])
                log.info("headset %s selected this computer (platform=%s) -> expect TCP on %d",
                         host, od.PLATFORMS.get(data[16], data[16]), od.MESSAGING_PORT)
            else:
                log.warning("17-byte select from %s with unknown ConnectionID %s", host, data[:16].hex())
            return
        if len(data) <= 243:
            return
        try:
            req = od.parse_discovery_request(data)
        except Exception as exc:
            log.debug("undecodable %d-byte datagram from %s: %r", len(data), host, exc)
            return
        if req.account_id not in self.accounts:
            log.info("ignoring discovery from %s: account %r (%s) not allowed", host, req.account_id,
                     req.platform_name)
            emit("pairing_request", account=req.account_id, platform=req.platform, host=host)
            return
        # re-scan: the USB (NCM) link only gets its link-local address once the headset brings it up
        adapters = local_adapters()
        if [a.ip for a in adapters] != [a.ip for a in self.computer.adapters]:
            log.info("adapters now: %s", ", ".join(f"{a.ip}{' (usb)' if a.is_usb else ''}" for a in adapters))
        self.computer.adapters = adapters
        reply = od.build_discovery_reply(req, self.computer)
        self.transport.sendto(reply, addr)
        log.info("discovery reply -> %s (%s, account %r, %d bytes)", host, req.platform_name,
                 req.account_id, len(reply))


def udp_socket(family: int) -> socket.socket:
    sock = socket.socket(family, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if family == socket.AF_INET:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("0.0.0.0", od.DISCOVERY_PORT))
    else:
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        sock.bind(("::", od.DISCOVERY_PORT))
    sock.setblocking(False)
    return sock


DUMP_DIR = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "OpenDesk" / "dumps"


async def handle_messaging(args, computer: od.ComputerInfo, reader: asyncio.StreamReader,
                           writer: asyncio.StreamWriter):
    global SESSION
    SESSION = Session()
    peer = writer.get_extra_info("peername")
    log.info("[messaging] connection from %s", peer[0])
    emit("connected", host=peer[0], usb=peer[0].startswith("fe80"))
    stream = od.AesStreamReader(reader, computer.key, computer.iv)
    SESSION.messaging = (writer, od.AesStreamWriter(computer.key, computer.iv))
    # Headset is a *client* of SharedStreamerSettings: it waits for our Reload (JSON) to initialize it.
    # Enums are numeric: StreamingSource 0 Desktop / 1 VR, Runtime 2 = OpenDesk XR.
    settings = {"HasVR": bool(args.vr), "AdapterName": "Linux PoC"}
    if args.vr:
        settings["ActiveRuntime"] = 2
    await SESSION.send_streamer_settings(settings, reload=True)
    if args.vr and args.start_vr:
        asyncio.get_running_loop().call_later(3.0, lambda: asyncio.ensure_future(
            SESSION.send_streamer_settings({"StreamingSource": 1})))
    DUMP_DIR.mkdir(parents=True, exist_ok=True)
    seq = 0
    try:
        while True:
            msg_type, payload = await od.read_net_message(stream)
            name = od.MESSAGE_TYPES.get(msg_type, f"type{msg_type}")
            seq += 1
            (DUMP_DIR / f"msg_{seq:04d}_{name}.bin").write_bytes(payload)
            SESSION.apply(name, payload)
            if name in ("MobileSettingsReload", "MobileSettingsPropertyChanged", "UserSettingsReload",
                        "UserSettingsPropertyChanged", "StreamerSettingsPropertyChanged"):
                emit("settings", kind=name, values=json.loads(payload or b"{}"))
            preview = payload[:96]
            text = preview.decode("utf-8", "replace") if preview.isascii() else preview.hex()
            log.info("[messaging] <- %s len=%d %s", name, len(payload), text)
    except (asyncio.IncompleteReadError, ConnectionError):
        log.info("[messaging] %s disconnected", peer[0])
        emit("disconnected", host=peer[0])
    finally:
        writer.close()


class Session:
    """Per-connection state: headset settings mirror + coordination between channels."""

    # keys whose change requires restarting the encoder with a new VideoFormat
    VIDEO_KEYS = {"DesktopFramerate", "DesktopBitrateLimit", "MeasuredBandwidth", "HmdType", "StreamingSource",
                  "VRFramerate"}

    def __init__(self):
        self.data_ready = asyncio.Event()   # bandwidth measured -> headset is about to flip IsConnected
        self.video_changed = asyncio.Event()
        self.user: dict = {}                # SharedUserSettings (headset is server)
        self.mobile: dict = {}              # SharedMobileSettings (headset is server)
        self.streamer: dict = {}            # SharedStreamerSettings (we are server; headset may change it)
        self.latest_frame_data: bytes | None = None
        self.messaging = None               # (StreamWriter, AesStreamWriter)

    async def send_streamer_settings(self, values: dict, reload: bool = False) -> None:
        writer, out = self.messaging
        if reload:
            self.streamer.clear()
        self.streamer.update(values)
        msg_type = od.MSG_STREAMER_SETTINGS_RELOAD if reload else od.MSG_STREAMER_SETTINGS_PROPERTY_CHANGED
        payload = json.dumps(values, separators=(",", ":")).encode()
        writer.write(out.encode(od.encode_net_message(msg_type, payload)))
        await writer.drain()
        log.info("[messaging] -> %s %s", "StreamerSettingsReload" if reload else "StreamerSettingsPropertyChanged",
                 payload.decode())
        if self.VIDEO_KEYS & values.keys():
            self.video_changed.set()

    def streaming_source(self) -> int:
        return int(self.streamer.get("StreamingSource", 0))

    def apply(self, name: str, payload: bytes) -> None:
        target = {"UserSettingsReload": self.user, "UserSettingsPropertyChanged": self.user,
                  "MobileSettingsReload": self.mobile, "MobileSettingsPropertyChanged": self.mobile,
                  "StreamerSettingsPropertyChanged": self.streamer}.get(name)
        if target is None:
            return
        try:
            values = json.loads(payload)
        except ValueError:
            return
        if name.endswith("Reload"):
            target.clear()
        target.update(values)
        if self.VIDEO_KEYS & values.keys():
            self.video_changed.set()

    def desktop_fps(self) -> int:
        return int(self.mobile.get("DesktopFramerate", 60))  # [DefaultValue(60)] in SharedMobileSettings

    def desktop_bitrate(self) -> int:
        """SharedMobileSettings.UpdateDesktopBitrate: min(MeasuredBandwidth, lerp(4 Mbps, max, limit))."""
        limit = round(float(self.user.get("DesktopBitrateLimit", 0.0)), 3)
        hmd = self.mobile.get("HmdType")
        lo, hi = 4_000_000, {HMD_OCULUS_QUEST1: 40_000_000, HMD_PICO_SPACE_PRO: 200_000_000}.get(hmd, 120_000_000)
        wanted = int(lo + (hi - lo) * limit)
        return min(int(self.mobile.get("MeasuredBandwidth", wanted)), wanted)


# Xenko.VR.HmdType values with a non-default GetMaxDesktopBitrate (Quest 3 = 1000 -> 120 Mbps)
HMD_OCULUS_QUEST1 = 259
HMD_PICO_SPACE_PRO = 1700


SESSION = Session()


async def handle_video(args, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    peer = writer.get_extra_info("peername")
    log.info("[video] connection from %s", peer[0])

    async def drain_incoming():  # headset sends eye-gaze positions ([i32 16][2x Vector2]) here
        total = 0
        while data := await reader.read(65536):
            total += len(data)
        log.info("[video] headset closed after sending %d bytes", total)

    drain = asyncio.create_task(drain_incoming())
    session = SESSION
    try:
        await asyncio.wait_for(session.data_ready.wait(), timeout=30)
        await asyncio.sleep(1.0)  # let the headset flip IsConnected before the format arrives
        while True:
            params = video_params(args, session)
            session.video_changed.clear()
            log.info("[video] starting %s: %d fps, %.1f Mbit/s", params[0], params[1], params[2] / 1000)
            emit("video", mode=params[0], fps=params[1], kbps=params[2], codec=params[3])
            run = asyncio.create_task(make_stream(args, writer, params).run())
            while True:  # wait until the stream dies or a setting actually changes fps/bitrate
                changed = asyncio.create_task(session.video_changed.wait())
                done, _ = await asyncio.wait({run, changed}, return_when=asyncio.FIRST_COMPLETED)
                if run in done:
                    changed.cancel()
                    run.result()
                    return
                session.video_changed.clear()
                if video_params(args, session) != params:
                    break
            log.info("[video] headset settings changed -> restarting encoder")
            run.cancel()
            try:
                await run
            except asyncio.CancelledError:
                pass
    except (ConnectionError, asyncio.TimeoutError) as exc:
        log.info("[video] stopped: %r", exc)
    finally:
        drain.cancel()
        writer.close()


def video_params(args, session: Session) -> tuple[str, int, int, str]:
    """(mode, fps, bitrate kbps, codec): CLI overrides win, otherwise follow the headset's settings.
    Any change restarts only the encoder (new VideoFormat), never the connection."""
    if args.vr and session.streaming_source() == 1:
        return "vr-test", args.vr_fps or int(session.user.get("VRFramerate", 72)), args.vr_bitrate_kbps, "h264"
    fps = args.fps or session.desktop_fps()
    kbps = args.bitrate_kbps or max(session.desktop_bitrate() // 1000, 1000)
    return args.source, fps, kbps, args.codec


def make_stream(args, writer: asyncio.StreamWriter, params: tuple[str, int, int, str]):
    mode, fps, kbps, codec = params
    if mode == "vr-test":
        w, h = (int(x) for x in args.vr_eye_size.split("x"))
        return vr.VrTestStream(writer, SESSION, fps, (w, h), kbps)
    if mode == "test":
        w, h = (int(x) for x in args.size.split("x"))
        return video.TestPatternStream(writer, w, h, fps, f"{kbps}k")
    name, w, h = pick_monitor(args.monitor)
    return video.DesktopCaptureStream(writer, name, w, h, fps, codec, kbps)


def pick_monitor(wanted: str | None) -> tuple[str, int, int]:
    """gpu-screen-recorder --list-monitors prints `NAME|WxH` lines."""
    out = subprocess.run(["gpu-screen-recorder", "--list-monitors"], capture_output=True, text=True).stdout
    monitors = []
    for line in out.splitlines():
        if "|" in line:
            name, size = line.split("|", 1)
            w, h = size.strip().split("x")
            monitors.append((name.strip(), int(w), int(h)))
    if not monitors:
        raise RuntimeError("gpu-screen-recorder found no monitors")
    for m in monitors:
        if wanted in (None, m[0]):
            return m
    raise RuntimeError(f"monitor {wanted!r} not found, have {[m[0] for m in monitors]}")


async def send_bandwidth_probe(writer: asyncio.StreamWriter, size: int) -> None:
    """NetDataClient.TryMeasureBitrate: i32 BE N, then N bytes; headset times the 2nd half.
    N <= 0 makes the headset skip measuring (assumes 1 Gbps)."""
    writer.write(size.to_bytes(4, "big", signed=True))
    chunk = os.urandom(1 << 20)
    for off in range(0, max(size, 0), len(chunk)):
        writer.write(chunk[:min(len(chunk), size - off)])
        await writer.drain()
    await writer.drain()
    log.info("[data] -> bandwidth probe %d bytes", size)
    SESSION.data_ready.set()


async def handle_audio(args, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    """Desktop audio -> headset; headset microphone -> PipeWire virtual source."""
    peer = writer.get_extra_info("peername")
    log.info("[audio] connection from %s", peer[0])
    session = SESSION
    mic = asyncio.create_task(audio.receive_mic(reader, enabled=not args.no_mic))
    try:
        await asyncio.wait_for(session.data_ready.wait(), timeout=30)
        await audio.stream_desktop_audio(writer, args.audio_device, enabled=lambda: not args.no_audio)
    except (ConnectionError, asyncio.TimeoutError) as exc:
        log.info("[audio] stopped: %r", exc)
    finally:
        mic.cancel()
        try:
            await mic
        except asyncio.CancelledError:
            pass
        writer.close()


async def handle_data(args, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    """Bandwidth probe, then FrameData stream (tracking + input) from the headset."""
    peer = writer.get_extra_info("peername")
    log.info("[data] connection from %s", peer[0])
    await send_bandwidth_probe(writer, int(args.probe_mb * (1 << 20)))
    injector = None
    if not args.no_input:
        _, w, h = pick_monitor(args.monitor)
        injector = input.InputInjector(w, h)
    packets = 0
    try:
        while True:
            fd = await input.read_frame_data(reader)
            packets += 1
            if packets == 1:
                log.info("[data] <- first FrameData %d bytes", len(fd))
            SESSION.latest_frame_data = fd
            if injector:
                injector.apply(fd)
    except (asyncio.IncompleteReadError, ConnectionError):
        log.info("[data] %s disconnected after %d FrameData packets", peer[0], packets)
    finally:
        if injector:
            injector.close()
        writer.close()


def raw_handler(name: str, on_connect=None):
    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        peer = writer.get_extra_info("peername")
        log.info("[%s] connection from %s", name, peer[0])
        if on_connect:
            await on_connect(writer)
        total = packets = 0
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                if packets < 3:
                    log.info("[%s] <- %d bytes: %s", name, len(data), data[:48].hex())
                total += len(data)
                packets += 1
        except ConnectionError:
            pass
        finally:
            log.info("[%s] %s disconnected after %d bytes", name, peer[0], total)
            writer.close()
    return handle


async def apply_options(args, options: dict) -> None:
    """Live option changes from the GUI; video restarts its encoder, the headset stays connected."""
    vr_before = args.vr
    for key in ("codec", "no_audio", "vr"):
        if key in options:
            setattr(args, key, options[key])
    log.info("options: codec=%s no_audio=%s vr=%s", args.codec, args.no_audio, args.vr)
    if SESSION.messaging and args.vr != vr_before:
        values = {"HasVR": bool(args.vr)}
        if args.vr:
            values["ActiveRuntime"] = 2
        elif SESSION.streaming_source() == 1:
            values["StreamingSource"] = 0
        await SESSION.send_streamer_settings(values)
    SESSION.video_changed.set()


async def read_commands(args, accounts: set[str]) -> None:
    """GUI -> engine: {"cmd":"set_accounts","accounts":[...]} | {"cmd":"set_options",...} | {"cmd":"quit"}"""
    loop = asyncio.get_running_loop()
    reader = asyncio.StreamReader()
    await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
    while line := await reader.readline():
        try:
            cmd = json.loads(line)
        except ValueError:
            continue
        if cmd.get("cmd") == "set_accounts":
            accounts.clear()
            accounts.update(cmd.get("accounts", []))
            log.info("allowed accounts: %s", sorted(accounts))
        elif cmd.get("cmd") == "set_options":
            await apply_options(args, cmd)
        elif cmd.get("cmd") == "quit":
            break
    os._exit(0)


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--account", action="append", default=[],
                    help="headset AccountID allowed to connect (repeatable; GUI sends updates on stdin)")
    ap.add_argument("--json", action="store_true",
                    help="GUI mode: JSON events on stdout, JSON commands on stdin")
    ap.add_argument("--name", default=socket.gethostname())
    ap.add_argument("--description", default="Linux streamer (PoC)")
    ap.add_argument("--streamer-version", default="1.34.23.0",
                    help="advertised streamer version; must match the headset app version")
    ap.add_argument("--probe-mb", type=float, default=16,
                    help="bandwidth probe size in MB sent on the data channel (0 = skip measuring)")
    ap.add_argument("--source", choices=("desktop", "test"), default="desktop")
    ap.add_argument("--monitor", help="monitor for desktop capture (default: first from gsr --list-monitors)")
    ap.add_argument("--codec", choices=("h264", "hevc"), default="h264", help="desktop capture codec")
    ap.add_argument("--bitrate-kbps", type=int, help="override bitrate (default: headset's desktop bitrate slider)")
    ap.add_argument("--fps", type=int, help="override frame rate (default: headset's desktop frame rate)")
    ap.add_argument("--size", default="1920x1080", help="test-pattern resolution")
    ap.add_argument("--vr", action="store_true",
                    help="advertise VR (HasVR, runtime OpenDesk XR); double-press the headset menu button to toggle VR")
    ap.add_argument("--start-vr", action="store_true", help="with --vr: switch the headset to VR right after connect")
    ap.add_argument("--vr-eye-size", default="1440x1536", help="VR per-eye render size (encoded at 1/1.5)")
    ap.add_argument("--vr-fps", type=int, help="override VR frame rate (default: headset VRFramerate)")
    ap.add_argument("--vr-bitrate-kbps", type=int, default=60000, help="VR bitrate for both eyes together")
    ap.add_argument("--no-input", action="store_true", help="don't inject headset input via uinput")
    ap.add_argument("--no-audio", action="store_true", help="don't stream desktop audio")
    ap.add_argument("--no-mic", action="store_true", help="don't create a virtual mic for the headset microphone")
    ap.add_argument("--audio-device", default="@DEFAULT_MONITOR@", help="PulseAudio/PipeWire source to capture")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    global JSON_EVENTS
    JSON_EVENTS = args.json
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, stream=sys.stderr,
                        format="%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S")

    state = load_state()
    adapters = local_adapters()
    computer = od.ComputerInfo(
        id=state["id"], name=args.name, description=args.description, adapters=adapters,
        connection_id=os.urandom(16), key=os.urandom(32), iv=os.urandom(16),
        version=tuple(int(p) for p in (args.streamer_version.split(".") + ["0"] * 4)[:4]),
    )
    log.info("computer id=%s name=%r version=%s", computer.id, computer.name,
             ".".join(map(str, computer.version)))
    for a in adapters:
        log.info("  adapter %s wifi=%s gbit=%s usb=%s", a.ip, a.is_wireless, a.is_gigabit, a.is_usb)
    if not any(a.is_usb for a in adapters):
        log.info("  (no USB/NCM link-local yet - re-checked on every discovery request)")

    loop = asyncio.get_running_loop()
    accounts = set(args.account)
    if args.json:
        # keep a strong reference: the event loop only holds tasks weakly
        BACKGROUND_TASKS.add(asyncio.create_task(read_commands(args, accounts)))
    emit("ready", id=computer.id, name=computer.name, version=".".join(map(str, computer.version)),
         adapters=[{"ip": a.ip, "wifi": a.is_wireless, "gbit": a.is_gigabit, "usb": a.is_usb} for a in adapters])
    for family in (socket.AF_INET, socket.AF_INET6):
        await loop.create_datagram_endpoint(lambda: DiscoveryProtocol(computer, accounts),
                                            sock=udp_socket(family))
    servers = [await asyncio.start_server(lambda r, w: handle_messaging(args, computer, r, w),
                                          host=None, port=od.MESSAGING_PORT, reuse_address=True)]
    servers.append(await asyncio.start_server(lambda r, w: handle_data(args, r, w), host=None,
                                              port=od.DATA_PORT, reuse_address=True))
    servers.append(await asyncio.start_server(lambda r, w: handle_audio(args, r, w), host=None,
                                              port=od.AUDIO_PORT, reuse_address=True))
    servers.append(await asyncio.start_server(lambda r, w: handle_video(args, r, w), host=None,
                                              port=od.VIDEO_PORT, reuse_address=True))
    log.info("listening: udp/%d, tcp/%d-%d - refresh the Computers list on the headset",
             od.DISCOVERY_PORT, od.MESSAGING_PORT, od.AUDIO_PORT)
    await asyncio.gather(*(s.serve_forever() for s in servers))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
