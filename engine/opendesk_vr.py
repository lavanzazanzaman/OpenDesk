"""Fake VR stream: proves the VR path of the upstream protocol before a real OpenXR runtime feeds it.

Per frame: take the latest headset pose (FrameData), render both eyes on the GPU (headless EGL) into the
*defoveation-warped* layout the headset expects, encode each eye as its own H.264 stream and send
  [Frame: FrameMetadata(Source=VR, RenderPose=eye poses+FOV) + left AU][Slice: StreamIndex=1 + right AU]
with identical TimestampUs (VideoPlayer pairs secondary output by pts).

Scene: a sky/ground grid at infinity (rotation-locked to the world, ignores translation), a red marker
at world -Z (forward at recenter), and screen-space eye markers: red bar left of centre in the LEFT eye,
blue bar right of centre in the RIGHT eye.

Defoveation (DefoveationPrimitive.cs): screen s -> texture t, t = 2s^2 (s<.25), 1.5(s-.25)+.125, 1-2(1-s)^2
(s>.75). We render texture pixels, so each pixel samples screen position s = t^-1.
"""
from __future__ import annotations

import asyncio
import logging
import math
import struct
import time

import moderngl
import numpy as np

import opendesk_video as vv

log = logging.getLogger("opendesk.vr")

SOURCE_VR = 1
PACKET_SLICE = 1
DECOMPRESSION_RATIO = 1.5

VERTEX = """
#version 330
in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
"""

FRAGMENT = """
#version 330
uniform vec2 size;
uniform mat3 rot;      // eye orientation (camera -> world)
uniform vec4 fov;      // left, right, up, down tangents (all positive)
uniform int eye;       // 0 left, 1 right
out vec4 color;

float unwarp(float t) {
    if (t < 0.125) return sqrt(t / 2.0);
    if (t > 0.875) return 1.0 - sqrt((1.0 - t) / 2.0);
    return (t - 0.125) / 1.5 + 0.25;
}

float grid(float v, float step, float width) {
    float d = abs(fract(v / step + 0.5) - 0.5) * step;
    return 1.0 - smoothstep(0.0, width, d);
}

void main() {
    // row 0 of the read-back buffer is gl_FragCoord.y = 0.5 -> treat it as the top of the image
    vec2 t = gl_FragCoord.xy / size;
    float s = unwarp(t.x);
    float r = unwarp(t.y);
    vec3 dir = normalize(vec3(mix(-fov.x, fov.y, s), mix(fov.z, -fov.w, r), -1.0));
    vec3 w = rot * dir;
    float lon = degrees(atan(w.x, -w.z));
    float lat = degrees(asin(clamp(w.y, -1.0, 1.0)));

    vec3 c = lat > 0.0 ? mix(vec3(0.35, 0.55, 0.9), vec3(0.05, 0.1, 0.35), lat / 90.0)
                       : mix(vec3(0.3, 0.45, 0.2), vec3(0.1, 0.15, 0.05), -lat / 90.0);
    float g = max(grid(lon, 10.0, 0.15), grid(lat, 10.0, 0.15));
    c = mix(c, vec3(0.9), g * 0.8);
    c = mix(c, vec3(1.0, 1.0, 0.2), 1.0 - smoothstep(0.0, 0.4, abs(lat)));        // horizon
    if (abs(lon) < 2.0 && abs(lat) < 2.0) c = vec3(1.0, 0.1, 0.1);                 // world forward
    if (abs(abs(lon) - 90.0) < 1.0 && abs(lat) < 6.0) c = vec3(1.0, 0.6, 0.0);     // left/right
    if (abs(abs(lon) - 180.0) < 1.5 && abs(lat) < 6.0) c = vec3(0.8, 0.0, 0.8);   // behind

    // screen-space eye markers (fixed in view): left eye red bar left of centre, right eye blue bar right
    if (r > 0.30 && r < 0.33) {
        if (eye == 0 && s > 0.36 && s < 0.46) c = vec3(1.0, 0.0, 0.0);
        if (eye == 1 && s > 0.54 && s < 0.64) c = vec3(0.1, 0.3, 1.0);
    }
    color = vec4(c, 1.0);
}
"""


def quat_to_mat3(x: float, y: float, z: float, w: float) -> np.ndarray:
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ], dtype=np.float32)


class EyeRenderer:
    def __init__(self, width: int, height: int):
        self.width, self.height = width, height
        self.ctx = moderngl.create_standalone_context(backend="egl")
        self.prog = self.ctx.program(vertex_shader=VERTEX, fragment_shader=FRAGMENT)
        quad = np.array([-1, -1, 1, -1, -1, 1, 1, 1], dtype=np.float32)
        self.vao = self.ctx.vertex_array(self.prog, [(self.ctx.buffer(quad.tobytes()), "2f", "pos")])
        self.fbo = self.ctx.simple_framebuffer((width, height), components=3)
        self.prog["size"].value = (width, height)

    def render(self, eye: int, rot: np.ndarray, fov: tuple[float, float, float, float]) -> bytes:
        self.fbo.use()
        self.prog["eye"].value = eye
        self.prog["rot"].write(rot.T.tobytes())  # GLSL mat3 is column-major
        self.prog["fov"].value = fov
        self.vao.render(moderngl.TRIANGLE_STRIP)
        return self.fbo.read(components=3, alignment=1)


def pack_render_pose(left_q, left_p, right_q, right_p, fov_l, fov_r) -> bytes:
    """RenderPose (FrameMetadata+28 .. +119): 3 bools, pad, L quat+pos, R quat+pos, FovLeft, FovRight."""
    return (struct.pack("<???x", True, True, True)
            + struct.pack("<7f", *left_q, *left_p) + struct.pack("<7f", *right_q, *right_p)
            + struct.pack("<4f", *fov_l) + struct.pack("<4f", *fov_r))


def pack_vr_frame(ts_us: int, frame_index: int, is_sync: bool, render_pose: bytes, eye_w: int, eye_h: int,
                  data_size: int, frame_time_ms: float) -> bytes:
    buf = bytearray(vv.FRAME_METADATA_SIZE)
    struct.pack_into("<qiB??", buf, 0, ts_us, frame_index, SOURCE_VR, is_sync, False)
    struct.pack_into("<fff", buf, 16, frame_time_ms, frame_time_ms, 0.0)
    buf[28:28 + len(render_pose)] = render_pose
    assert 28 + len(render_pose) == 120
    struct.pack_into("<ii", buf, 144, eye_w, eye_h)  # RenderResolution
    struct.pack_into("<i", buf, 152, data_size)
    return bytes([vv.PACKET_FRAME]) + bytes(buf)


def pack_slice(ts_us: int, stream_index: int, data_size: int) -> bytes:
    return bytes([PACKET_SLICE]) + struct.pack("<qii", ts_us, stream_index, data_size)


def pack_vr_format(fps: int, enc_w: int, enc_h: int, eye_w: int, eye_h: int) -> bytes:
    sizes = [0] * 22
    sizes[0:4] = [0, 0, eye_w, eye_h]      # Bounds
    sizes[4:6] = [enc_w, enc_h]            # SecondaryFrameSize
    data = struct.pack("<B3xiiiii", SOURCE_VR, vv.CODEC_H264, fps, enc_w, enc_h, 2) + struct.pack("<22i", *sizes)
    return bytes([vv.PACKET_VIDEO_FORMAT]) + data


class VrTestStream:
    def __init__(self, writer: asyncio.StreamWriter, session, fps: int, eye_size: tuple[int, int], bitrate_kbps: int):
        self.writer, self.session, self.fps = writer, session, fps
        self.eye_w, self.eye_h = eye_size
        # encoded size = eye / 1.5, rounded to a multiple of 16
        self.enc_w = int(self.eye_w / DECOMPRESSION_RATIO) // 16 * 16
        self.enc_h = int(self.eye_h / DECOMPRESSION_RATIO) // 16 * 16
        self.bitrate_kbps = bitrate_kbps
        self.procs: list[asyncio.subprocess.Process] = []

    def encoder_cmd(self) -> list[str]:
        per_eye = max(self.bitrate_kbps // 2, 2000)
        return ["ffmpeg", "-hide_banner", "-loglevel", "error",
                "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{self.enc_w}x{self.enc_h}", "-r", str(self.fps), "-i", "-",
                "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-pix_fmt", "yuv420p",
                "-g", str(self.fps), "-bf", "0", "-b:v", f"{per_eye}k", "-maxrate", f"{per_eye}k",
                "-bufsize", f"{per_eye // self.fps * 2}k", "-f", "h264", "-"]

    async def _pump(self, proc, queue: asyncio.Queue) -> None:
        splitter = vv.AccessUnitSplitter(vv.CODEC_H264)
        while chunk := await proc.stdout.read(1 << 18):
            for au in splitter.push(chunk):
                await queue.put(au)

    def _pose(self):
        """Latest head pose from FrameData -> per-eye (quat, pos). Identity until tracking arrives."""
        fd = self.session.latest_frame_data
        if fd is None or len(fd) < 148:
            q, p, idx = (0.0, 0.0, 0.0, 1.0), (0.0, 1.6, 0.0), 0
        else:
            idx = struct.unpack_from("<i", fd, 0)[0]
            q = struct.unpack_from("<4f", fd, 120)
            p = struct.unpack_from("<3f", fd, 136)
        rot = quat_to_mat3(*q)
        half_ipd = float(self.session.mobile.get("IPD", 0.063)) / 2
        right = rot[:, 0]
        lp = tuple(float(v) for v in np.array(p) - right * half_ipd)
        rp = tuple(float(v) for v in np.array(p) + right * half_ipd)
        return idx, q, rot, lp, rp

    def _fov(self, key: str) -> tuple[float, float, float, float]:
        f = self.session.mobile.get(key) or {"UpTan": 1.0, "DownTan": 1.0, "LeftTan": 1.0, "RightTan": 1.0}
        return f["LeftTan"], f["RightTan"], f["UpTan"], f["DownTan"]

    async def run(self) -> None:
        renderer = EyeRenderer(self.enc_w, self.enc_h)
        self.writer.write(pack_vr_format(self.fps, self.enc_w, self.enc_h, self.eye_w, self.eye_h))
        await self.writer.drain()
        log.info("[vr] -> VideoFormat VR H.264 x2, encoded %dx%d per eye (eye %dx%d) @%d",
                 self.enc_w, self.enc_h, self.eye_w, self.eye_h, self.fps)
        self.procs = [await asyncio.create_subprocess_exec(*self.encoder_cmd(), stdin=asyncio.subprocess.PIPE,
                                                           stdout=asyncio.subprocess.PIPE) for _ in range(2)]
        queues = [asyncio.Queue(), asyncio.Queue()]
        pumps = [asyncio.create_task(self._pump(p, q)) for p, q in zip(self.procs, queues)]
        pending: dict[int, tuple[int, int, bytes]] = {}
        sender = asyncio.create_task(self._send(queues, pending))
        period = 1.0 / self.fps
        next_t = time.monotonic()
        index = 0
        try:
            while not sender.done():
                idx, q, rot, lp, rp = self._pose()
                fl, fr = self._fov("FovLeft"), self._fov("FovRight")
                ts = time.monotonic_ns() // 1000
                pending[index] = (ts, idx, pack_render_pose(
                    q, lp, q, rp,
                    (fl[2], fl[3], fl[0], fl[1]), (fr[2], fr[3], fr[0], fr[1])))  # FovPort: Up, Down, Left, Right
                for eye, (proc, fov) in enumerate(zip(self.procs, (fl, fr))):
                    proc.stdin.write(renderer.render(eye, rot, fov))
                await asyncio.gather(*(p.stdin.drain() for p in self.procs))
                index += 1
                next_t += period
                await asyncio.sleep(max(0.0, next_t - time.monotonic()))
            sender.result()
        finally:
            for t in (*pumps, sender):
                t.cancel()
            for p in self.procs:
                if p.returncode is None:
                    p.kill()
                    await p.wait()
            renderer.ctx.release()
            log.info("[vr] stream stopped after %d frames", index)

    async def _send(self, queues, pending) -> None:
        k = sent_bytes = 0
        last = time.monotonic()
        while True:
            left = await queues[0].get()
            right = await queues[1].get()
            ts, pose_index, render_pose = pending.pop(k)
            sync = vv.is_sync(left, vv.CODEC_H264)
            self.writer.write(pack_vr_frame(ts, pose_index, sync, render_pose, self.eye_w, self.eye_h,
                                            len(left), 1000 / self.fps) + left)
            self.writer.write(pack_slice(ts, 1, len(right)) + right)
            await self.writer.drain()
            k += 1
            sent_bytes += len(left) + len(right)
            if k == 1:
                log.info("[vr] -> first stereo frame (%s) %d + %d bytes", "sync" if sync else "NON-SYNC",
                         len(left), len(right))
            now = time.monotonic()
            if now - last >= 5:
                log.info("[vr] %d frames, %.1f Mbit/s, head pose frame %d", k, sent_bytes * 8 / (now - last) / 1e6,
                         pose_index)
                sent_bytes, last = 0, now
