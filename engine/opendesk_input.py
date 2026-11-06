"""Data channel (tcp/38820, headset -> streamer): FrameData packets -> Linux input via uinput.

Packet (NetDataClient.SendFrameData, raw little-endian):
  [i32 lz4_size][u64 xxh3 of uncompressed][lz4 block]  -> first N bytes of FrameData (N varies by state)
FrameData offsets below come from `dcgen --layout Upstream.Mobile.FrameData Upstream.Mobile.Shared`
(full table: work/dumps/FrameData.layout.txt).
"""
from __future__ import annotations

import asyncio
import logging
import struct

import lz4.block
from evdev import AbsInfo, UInput, ecodes as e

log = logging.getLogger("opendesk.input")

FRAME_DATA_SIZE = 4992
INPUT_END = 118  # InputState + LockStates

# RawPointerState (size 10): _active, _overlayActive, _leftDown, _rightDown, _middleDown, pad, i16 X, i16 Y
LEFT_POINTER, RIGHT_POINTER = 12, 22
KEYBOARD, KEY_SLOTS, KEY_SLOT_SIZE = 32, 8, 3   # {u16 ScanCode, u8 KeyStateFlags} x 8
MOUSE = 60   # _active,_left,_middle,_right,_x1,_x2 @60..65, f32 X @68, f32 Y @72, i8 ScrollX @76, i8 ScrollY @77
KEY_DOWN, KEY_PRESSED = 1, 2

# PC set-1 scancodes 1..83 equal Linux KEY_* codes; the rest by name (Upstream.Interfaces.ScanCodes)
SCANCODE_TO_KEY = {i: i for i in range(1, 84)}
SCANCODE_TO_KEY.update({
    84: e.KEY_SYSRQ, 86: e.KEY_102ND, 87: e.KEY_F11, 88: e.KEY_F12, 112: e.KEY_F21, 115: e.KEY_F24,
    121: e.KEY_AGAIN, 123: e.KEY_CUT, 125: e.KEY_PASTE,
    0xE01C: e.KEY_KPENTER, 0xE01D: e.KEY_RIGHTCTRL, 0xE035: e.KEY_KPSLASH, 0xE038: e.KEY_RIGHTALT,
    0xE045: e.KEY_PAUSE, 0xE047: e.KEY_HOME, 0xE048: e.KEY_UP, 0xE049: e.KEY_PAGEUP, 0xE04B: e.KEY_LEFT,
    0xE04D: e.KEY_RIGHT, 0xE04F: e.KEY_END, 0xE050: e.KEY_DOWN, 0xE051: e.KEY_PAGEDOWN, 0xE052: e.KEY_INSERT,
    0xE053: e.KEY_DELETE, 0xE05B: e.KEY_LEFTMETA, 0xE05C: e.KEY_RIGHTMETA, 0xE05D: e.KEY_COMPOSE,
})

# headset scroll is a per-packet value that halves each packet (RawMouseState.ResetAndFadeScroll);
# emitted as hi-res wheel units (120 = one notch). Scale is a guess - tune by feel.
SCROLL_HIRES_PER_UNIT = 15


async def read_frame_data(reader: asyncio.StreamReader) -> bytes:
    header = await reader.readexactly(12)
    size = int.from_bytes(header[:4], "little", signed=True)
    return lz4.block.decompress(await reader.readexactly(size), uncompressed_size=FRAME_DATA_SIZE)


class InputInjector:
    def __init__(self, width: int, height: int):
        self.width, self.height = width, height
        self.pointer = UInput({
            e.EV_KEY: [e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE],
            e.EV_ABS: [(e.ABS_X, AbsInfo(0, 0, width - 1, 0, 0, 0)),
                       (e.ABS_Y, AbsInfo(0, 0, height - 1, 0, 0, 0))],
        }, name="OpenDesk Pointer", vendor=0x1d6b, product=0x5644)
        self.kbd_mouse = UInput({
            e.EV_KEY: sorted(set(SCANCODE_TO_KEY.values()))
                      + [e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE, e.BTN_SIDE, e.BTN_EXTRA],
            e.EV_REL: [e.REL_X, e.REL_Y, e.REL_WHEEL, e.REL_HWHEEL, e.REL_WHEEL_HI_RES, e.REL_HWHEEL_HI_RES],
        }, name="OpenDesk Keyboard/Mouse", vendor=0x1d6b, product=0x5645)
        self.keys_down: set[int] = set()
        self.pointer_pos: dict[int, tuple[int, int]] = {}
        self.pointer_buttons = (False, False, False)
        self.mouse_buttons = (False,) * 5
        self.frac = [0.0, 0.0]
        self.wheel_acc = [0, 0]
        log.info("uinput devices ready (pointer %dx%d)", width, height)

    def close(self) -> None:
        for key in self.keys_down:
            self.kbd_mouse.write(e.EV_KEY, key, 0)
        self.kbd_mouse.syn()
        self.pointer.close()
        self.kbd_mouse.close()

    def apply(self, fd: bytes) -> None:
        if len(fd) < INPUT_END:
            return
        self._pointers(fd)
        self._mouse(fd)
        self._keyboard(fd)

    def _pointers(self, fd: bytes) -> None:
        buttons = [False, False, False]
        moved = False
        for off in (LEFT_POINTER, RIGHT_POINTER):
            active, overlay, left, right, middle = fd[off:off + 5]
            if not active or overlay:
                self.pointer_pos.pop(off, None)
                continue
            x, y = struct.unpack_from("<hh", fd, off + 6)
            x, y = min(max(x, 0), self.width - 1), min(max(y, 0), self.height - 1)
            if self.pointer_pos.get(off) != (x, y):  # the pointer that moved takes the cursor
                self.pointer_pos[off] = (x, y)
                self.pointer.write(e.EV_ABS, e.ABS_X, x)
                self.pointer.write(e.EV_ABS, e.ABS_Y, y)
                moved = True
            buttons = [b or bool(v) for b, v in zip(buttons, (left, right, middle))]
        changed = tuple(buttons) != self.pointer_buttons
        for code, now, before in zip((e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE), buttons, self.pointer_buttons):
            if now != before:
                self.pointer.write(e.EV_KEY, code, int(now))
        self.pointer_buttons = tuple(buttons)
        if moved or changed:
            self.pointer.syn()

    def _mouse(self, fd: bytes) -> None:
        if not fd[MOUSE]:
            return
        buttons = tuple(bool(b) for b in fd[MOUSE + 1:MOUSE + 6])  # left, middle, right, x1, x2
        dx, dy = struct.unpack_from("<ff", fd, MOUSE + 8)
        sx, sy = struct.unpack_from("<bb", fd, MOUSE + 16)
        dirty = False
        for i, d in enumerate((dx, dy)):
            self.frac[i] += d
            step = int(self.frac[i])
            if step:
                self.frac[i] -= step
                self.kbd_mouse.write(e.EV_REL, (e.REL_X, e.REL_Y)[i], step)
                dirty = True
        for i, (s, hi, lo) in enumerate(((sy, e.REL_WHEEL_HI_RES, e.REL_WHEEL),
                                         (sx, e.REL_HWHEEL_HI_RES, e.REL_HWHEEL))):
            if s:
                v = s * SCROLL_HIRES_PER_UNIT
                self.kbd_mouse.write(e.EV_REL, hi, v)
                self.wheel_acc[i] += v
                notches = int(self.wheel_acc[i] / 120)
                if notches:
                    self.wheel_acc[i] -= notches * 120
                    self.kbd_mouse.write(e.EV_REL, lo, notches)
                dirty = True
        codes = (e.BTN_LEFT, e.BTN_MIDDLE, e.BTN_RIGHT, e.BTN_SIDE, e.BTN_EXTRA)
        for code, now, before in zip(codes, buttons, self.mouse_buttons):
            if now != before:
                self.kbd_mouse.write(e.EV_KEY, code, int(now))
                dirty = True
        self.mouse_buttons = buttons
        if dirty:
            self.kbd_mouse.syn()

    def _keyboard(self, fd: bytes) -> None:
        down, taps = set(), set()
        for i in range(KEY_SLOTS):
            sc, state = struct.unpack_from("<HB", fd, KEYBOARD + i * KEY_SLOT_SIZE)
            key = SCANCODE_TO_KEY.get(sc)
            if not sc:
                continue
            if key is None:
                log.debug("unmapped scancode 0x%04x", sc)
                continue
            if state & KEY_DOWN:
                down.add(key)
            elif state & KEY_PRESSED:
                taps.add(key)
        dirty = False
        for key in sorted(down - self.keys_down):
            self.kbd_mouse.write(e.EV_KEY, key, 1)
            dirty = True
        for key in sorted(self.keys_down - down):
            self.kbd_mouse.write(e.EV_KEY, key, 0)
            dirty = True
        for key in sorted(taps - down - self.keys_down):  # pressed and released within one packet
            self.kbd_mouse.write(e.EV_KEY, key, 1)
            self.kbd_mouse.syn()
            self.kbd_mouse.write(e.EV_KEY, key, 0)
            dirty = True
        self.keys_down = down
        if dirty:
            self.kbd_mouse.syn()
