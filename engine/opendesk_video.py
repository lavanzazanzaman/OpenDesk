"""Video channel (tcp/38830, streamer -> headset): VideoFormat + Frame packets.

Wire format (NetVideoClient.VideoNetworkLoop), raw little-endian structs, layouts dumped from the real
assembly with `dcgen --layout`:
  [u8 PacketType.VideoFormat=4][VideoFormat 112 B]
  [u8 PacketType.Frame=0][FrameMetadata 160 B][DataSize bytes of Annex-B access unit]
The headset feeds each access unit to MediaCodec as-is (no csd), so SPS/PPS must be in-band, and
matches decoded output back to FrameMetadata by TimestampUs (must be unique per frame).
"""
from __future__ import annotations

import asyncio
import logging
import os
import struct
import tempfile
import time

log = logging.getLogger("opendesk.video")

PACKET_FRAME = 0
PACKET_VIDEO_FORMAT = 4
SOURCE_DESKTOP = 0
CODEC_H264 = 1
CODEC_HEVC = 2
CODEC_HEVC10 = 6

FRAME_METADATA_SIZE = 160
IDLE_FLUSH_S = 0.003
VIDEO_FORMAT_SIZE = 112


def pack_video_format(source: int, codec: int, fps: int, width: int, height: int, stream_count: int = 1) -> bytes:
    rect_and_sizes = [0] * 22  # Bounds + Secondary/Tertiary/Quaternary FrameSize & Bounds
    rect_and_sizes[0:4] = [0, 0, width, height]  # Bounds
    data = struct.pack("<B3xiiiii", source, codec, fps, width, height, stream_count)
    data += struct.pack("<22i", *rect_and_sizes)
    assert len(data) == VIDEO_FORMAT_SIZE
    return bytes([PACKET_VIDEO_FORMAT]) + data


def pack_frame(timestamp_us: int, frame_index: int, source: int, is_sync: bool, width: int, height: int,
               data_size: int, frame_time_ms: float = 0.0, encode_time_ms: float = 0.0) -> bytes:
    buf = bytearray(FRAME_METADATA_SIZE)
    struct.pack_into("<qiB??", buf, 0, timestamp_us, frame_index, source, is_sync, False)
    struct.pack_into("<fff", buf, 16, frame_time_ms, 0.0, encode_time_ms)
    # RenderPose (28..119) left zeroed: HasValidPose=false for desktop streaming
    struct.pack_into("<ii", buf, 144, width, height)  # RenderResolution
    struct.pack_into("<i", buf, 152, data_size)
    return bytes([PACKET_FRAME]) + bytes(buf)


class AccessUnitSplitter:
    """Splits an Annex-B byte stream into access units without relying on AUD NALs.

    New AU begins at a non-VCL "prefix" NAL (AUD/SPS/PPS/SEI/...) following a VCL NAL, or at a VCL NAL
    whose first-slice flag is set (H.264 first_mb_in_slice == 0 / HEVC first_slice_segment_in_pic_flag)
    after a VCL NAL. Each AU is emitted once the next AU's first NAL is seen (one frame of buffering).
    """

    def __init__(self, codec: int):
        self.hevc = codec in (CODEC_HEVC, CODEC_HEVC10)
        self.buf = bytearray()
        self.au_start = None      # offset of current AU start in buf
        self.seen_vcl = False
        self.scan = 0

    def _classify(self, header: bytes) -> tuple[bool, bool, bool]:
        """-> (is_vcl, is_first_slice, is_prefix_nal)"""
        if self.hevc:
            t = (header[0] >> 1) & 0x3F
            if t < 32:
                return True, bool(header[2] & 0x80), False
            return False, False, t in (32, 33, 34, 35, 39) or 41 <= t <= 44 or 48 <= t <= 55
        t = header[0] & 0x1F
        if 1 <= t <= 5:
            return True, bool(header[1] & 0x80), False
        return False, False, t in (6, 7, 8, 9) or 14 <= t <= 18

    def pending(self) -> bool:
        return self.au_start is not None and self.seen_vcl

    def flush(self) -> bytes | None:
        """Emit the buffered access unit now (caller knows the encoder finished writing it)."""
        if not self.pending():
            return None
        au = bytes(self.buf[self.au_start:])
        self.buf.clear()
        self.au_start, self.seen_vcl, self.scan = None, False, 0
        return au

    def push(self, data: bytes) -> list[bytes]:
        self.buf += data
        out = []
        buf = self.buf
        i = self.scan
        while True:
            j = buf.find(b"\x00\x00\x01", i)
            if j < 0 or j + 6 > len(buf):
                break
            nal_start = j - 1 if j > 0 and buf[j - 1] == 0 else j
            is_vcl, first_slice, prefix = self._classify(bytes(buf[j + 3:j + 6]))
            new_au = (prefix and self.seen_vcl) or (is_vcl and first_slice and self.seen_vcl)
            if self.au_start is None:
                self.au_start = nal_start
            elif new_au:
                out.append(bytes(buf[self.au_start:nal_start]))
                self.au_start = nal_start
                self.seen_vcl = False
            if is_vcl:
                self.seen_vcl = True
            i = j + 3
        if self.au_start:  # compact: drop consumed bytes
            del buf[:self.au_start]
            i -= self.au_start
            self.au_start = 0
        self.scan = max(i, 0)
        return out


def is_sync(au: bytes, codec: int) -> bool:
    hevc = codec in (CODEC_HEVC, CODEC_HEVC10)
    i = 0
    while True:
        i = au.find(b"\x00\x00\x01", i)
        if i < 0 or i + 3 >= len(au):
            return False
        if hevc:
            if 16 <= (au[i + 3] >> 1) & 0x3F <= 21:  # BLA/IDR/CRA
                return True
        elif au[i + 3] & 0x1F == 5:
            return True
        i += 3


class EncodedStream:
    """Base: runs an encoder subprocess producing Annex-B and forwards access units as Desktop frames."""

    codec = CODEC_H264

    def __init__(self, writer: asyncio.StreamWriter, width: int, height: int, fps: int):
        self.writer = writer
        self.width, self.height, self.fps = width, height, fps
        self.proc: asyncio.subprocess.Process | None = None

    async def open_output(self) -> asyncio.StreamReader:
        raise NotImplementedError

    async def run(self) -> None:
        self.writer.write(pack_video_format(SOURCE_DESKTOP, self.codec, self.fps, self.width, self.height))
        await self.writer.drain()
        log.info("[video] -> VideoFormat Desktop codec=%d %dx%d@%d", self.codec, self.width, self.height, self.fps)
        reader = await self.open_output()
        splitter = AccessUnitSplitter(self.codec)
        index = sent_bytes = window_frames = 0
        last_report = time.monotonic()
        try:
            while True:
                # Encoders write one access unit at a time; if the pipe goes quiet while an AU is pending,
                # it is complete - send it now instead of waiting ~1 frame for the next AU's first NAL.
                try:
                    chunk = await asyncio.wait_for(reader.read(1 << 18), IDLE_FLUSH_S if splitter.pending() else None)
                except asyncio.TimeoutError:
                    chunk = None
                if chunk == b"":
                    break
                aus = splitter.push(chunk) if chunk else [splitter.flush()]
                for au in aus:
                    sync = is_sync(au, self.codec)
                    ts = time.monotonic_ns() // 1000
                    self.writer.write(pack_frame(ts, index, SOURCE_DESKTOP, sync, self.width, self.height,
                                                 len(au), frame_time_ms=1000 / self.fps) + au)
                    await self.writer.drain()
                    index += 1
                    window_frames += 1
                    sent_bytes += len(au)
                    if index == 1:
                        log.info("[video] -> first frame %s %d bytes", "sync" if sync else "NON-SYNC", len(au))
                now = time.monotonic()
                if now - last_report >= 5:
                    span = now - last_report
                    log.info("[video] %d frames, %.1f fps, %.1f Mbit/s", index, window_frames / span,
                             sent_bytes * 8 / span / 1e6)
                    sent_bytes, window_frames, last_report = 0, 0, now
        finally:
            await self.close()
            log.info("[video] stream stopped after %d frames", index)

    async def close(self) -> None:
        if self.proc and self.proc.returncode is None:
            self.proc.kill()
            await self.proc.wait()


class DesktopCaptureStream(EncodedStream):
    """gpu-screen-recorder: KMS capture (gsr-kms-server has cap_sys_admin) + VAAPI encode -> FIFO."""

    def __init__(self, writer, monitor: str, width: int, height: int, fps: int, codec: str, bitrate_kbps: int):
        super().__init__(writer, width, height, fps)
        self.monitor, self.codec_name, self.bitrate_kbps = monitor, codec, bitrate_kbps
        self.codec = {"h264": CODEC_H264, "hevc": CODEC_HEVC}[codec]
        self.fifo_dir = tempfile.mkdtemp(prefix="vdgsr-")
        self.fifo = os.path.join(self.fifo_dir, f"stream.{codec}")

    async def open_output(self) -> asyncio.StreamReader:
        os.mkfifo(self.fifo)
        cmd = ["gpu-screen-recorder", "-w", self.monitor, "-c", self.codec_name, "-k", self.codec_name,
               "-f", str(self.fps), "-bm", "cbr", "-q", str(self.bitrate_kbps), "-keyint", "1",
               "-fm", "cfr", "-tune", "performance", "-cursor", "yes",
               # latency: write each packet immediately; no extra frame of VAAPI pipelining
               "-ffmpeg-opts", "flush_packets=1", "-ffmpeg-video-opts", "async_depth=1",
               "-o", self.fifo]
        log.info("[video] %s", " ".join(cmd))
        self.proc = await asyncio.create_subprocess_exec(*cmd, stdout=asyncio.subprocess.DEVNULL,
                                                         stderr=asyncio.subprocess.DEVNULL)
        loop = asyncio.get_running_loop()
        fd = await loop.run_in_executor(None, os.open, self.fifo, os.O_RDONLY)
        reader = asyncio.StreamReader(limit=1 << 24)
        await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), os.fdopen(fd, "rb", 0))
        return reader

    async def close(self) -> None:
        await super().close()
        for path in (self.fifo, self.fifo_dir):
            try:
                os.rmdir(path) if path == self.fifo_dir else os.unlink(path)
            except OSError:
                pass


class TestPatternStream(EncodedStream):
    """ffmpeg testsrc2 -> libx264 Annex-B."""

    def __init__(self, writer, width: int, height: int, fps: int, bitrate: str):
        super().__init__(writer, width, height, fps)
        self.bitrate = bitrate

    async def open_output(self) -> asyncio.StreamReader:
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-re",
            "-f", "lavfi", "-i", f"testsrc2=size={self.width}x{self.height}:rate={self.fps}",
            "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-profile:v", "high",
            "-pix_fmt", "yuv420p", "-g", str(self.fps), "-bf", "0", "-b:v", self.bitrate, "-f", "h264", "-",
        ]
        self.proc = await asyncio.create_subprocess_exec(*cmd, stdout=asyncio.subprocess.PIPE)
        return self.proc.stdout
