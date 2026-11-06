"""Audio channel (tcp/38840), NetAudioClient: packets are [i32 LE size][raw PCM].

streamer -> headset: s16le, 48 kHz, stereo (VideoPlayer: AAudioStream(Output, PcmI16, Music, 48000, 2)).
headset -> streamer: microphone, s16le, 48 kHz, mono (Microphone.cs), only while MicPassthrough is on.
The headset drops queued audio once > ~18 KB is buffered, so keep packets small (5 ms here).
"""
from __future__ import annotations

import asyncio
import logging
import os

log = logging.getLogger("opendesk.audio")

RATE = 48000
OUT_CHANNELS = 2
PACKET_MS = 5
PACKET_BYTES = RATE * PACKET_MS // 1000 * OUT_CHANNELS * 2  # 960
MIC_SOURCE = "opendesk_mic"


async def stream_desktop_audio(writer: asyncio.StreamWriter, device: str, enabled=lambda: True) -> None:
    """Keeps capturing while disabled (so re-enabling is instant) but only sends when enabled()."""
    cmd = ["parec", f"--device={device}", "--format=s16le", f"--rate={RATE}", f"--channels={OUT_CHANNELS}",
           "--latency-msec=5", "--process-time-msec=2", "--raw", "--client-name=OpenDesk streamer"]
    # ask PipeWire for a 256-sample (5.3 ms) quantum instead of the default 1024 (21 ms)
    env = {**os.environ, "PIPEWIRE_LATENCY": f"256/{RATE}", "PULSE_LATENCY_MSEC": "5"}
    proc = await asyncio.create_subprocess_exec(*cmd, stdout=asyncio.subprocess.PIPE, env=env)
    log.info("[audio] capturing %s -> headset (s16le %d Hz %dch)", device, RATE, OUT_CHANNELS)
    header = PACKET_BYTES.to_bytes(4, "little")
    sent = 0
    try:
        while True:
            pcm = await proc.stdout.readexactly(PACKET_BYTES)
            if not enabled():
                continue
            writer.write(header + pcm)
            await writer.drain()
            sent += 1
    except asyncio.IncompleteReadError:
        log.warning("[audio] parec exited (rc=%s)", proc.returncode)
    finally:
        if proc.returncode is None:
            proc.kill()
            await proc.wait()
        log.info("[audio] desktop audio stopped after %.1f s", sent * PACKET_MS / 1000)


class MicSink:
    """Plays headset mic PCM into a PipeWire virtual source, created lazily and removed on close."""

    def __init__(self):
        self.module_id: str | None = None
        self.proc: asyncio.subprocess.Process | None = None

    async def _ensure(self) -> None:
        if self.proc:
            return
        out = await (await asyncio.create_subprocess_exec(
            "pactl", "load-module", "module-null-sink", "media.class=Audio/Source/Virtual",
            f"sink_name={MIC_SOURCE}", "channel_map=mono", f"rate={RATE}",
            "sink_properties=device.description=Virtual_Desktop_Microphone",
            stdout=asyncio.subprocess.PIPE)).communicate()
        self.module_id = out[0].decode().strip() or None
        self.proc = await asyncio.create_subprocess_exec(
            "pacat", "--playback", f"--device={MIC_SOURCE}", "--format=s16le", f"--rate={RATE}", "--channels=1",
            "--latency-msec=20", "--raw", "--client-name=OpenDesk mic", stdin=asyncio.subprocess.PIPE)
        log.info("[audio] headset mic -> PipeWire source '%s' (module %s)", MIC_SOURCE, self.module_id)

    async def write(self, pcm: bytes) -> None:
        await self._ensure()
        self.proc.stdin.write(pcm)
        await self.proc.stdin.drain()

    async def close(self) -> None:
        if self.proc and self.proc.returncode is None:
            self.proc.kill()
            await self.proc.wait()
        if self.module_id:
            await (await asyncio.create_subprocess_exec("pactl", "unload-module", self.module_id)).wait()
            log.info("[audio] removed mic source")


async def receive_mic(reader: asyncio.StreamReader, enabled: bool) -> None:
    mic = MicSink() if enabled else None
    total = 0
    try:
        while True:
            size = int.from_bytes(await reader.readexactly(4), "little", signed=True)
            pcm = await reader.readexactly(size)
            if total == 0:
                log.info("[audio] <- first mic packet %d bytes", size)
            total += size
            if mic:
                await mic.write(pcm)
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        if mic:
            await mic.close()
        log.info("[audio] mic stream ended after %.1f s", total / (RATE * 2))
