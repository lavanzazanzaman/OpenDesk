#!/usr/bin/env python3
"""Fix Proton's WineOpenXR session-list use-after-free in a wineopenxr.dll (PE, x86-64).

WineOpenXR's xrDestroySession frees the session wrapper but never unlinks it from `session_list`
(upstream Proton 9/10/11: `free(wine_session)` without `list_remove`). xrPollEvent later walks that list to map
event session handles, reading freed memory; once it is reused the game crashes (First Contact via RiftLift:
access violation in wineopenxr.dll xrPollEvent ~30 s in, because Revive-based bridges create and destroy
temporary sessions first).

The patch reroutes xrDestroySession's success path (`mov rcx, rbx; call [free]`) to a few instructions in the
.text padding that take session_list_lock, unlink the wrapper, release the lock, then free and return exactly like
the original epilogue. Everything is located by the DLL's COFF symbols and checked byte by byte against the expected
code; anything unexpected aborts without writing.

    fix_wineopenxr.py [--check] path/to/wineopenxr.dll
exit codes: 0 patched now or already, 1 error / unsupported build, 2 (--check) not patched yet
"""
import struct
import sys

MARKER = b"VDLNX-WOXR1"  # after the injected code: identifies a patched DLL


class PE:
    def __init__(self, data: bytearray):
        self.d = data
        if data[:2] != b"MZ":
            raise ValueError("not a PE file")
        self.pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.pe:self.pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        self.machine, self.nsec, _, self.symtab, self.nsym, self.optsize = struct.unpack_from("<HHIIIH", data, self.pe + 4)
        if self.machine != 0x8664:
            raise ValueError("not x86-64")
        self.opt = self.pe + 24
        self.image_base = struct.unpack_from("<Q", data, self.opt + 24)[0]
        self.sections = []
        for i in range(self.nsec):
            o = self.opt + self.optsize + 40 * i
            name = data[o:o + 8].rstrip(b"\0").decode(errors="replace")
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append({"name": name, "vsize": vsize, "va": va, "rawsize": rawsize, "rawptr": rawptr, "hdr": o})

    def symbols(self):
        """COFF symbol table: name -> RVA (Wine's PE modules are built with it)."""
        if not self.symtab or not self.nsym:
            return {}
        strtab = self.symtab + 18 * self.nsym
        out = {}
        i = 0
        while i < self.nsym:
            o = self.symtab + 18 * i
            raw = self.d[o:o + 8]
            if raw[:4] == b"\0\0\0\0":
                off = struct.unpack_from("<I", raw, 4)[0]
                end = self.d.index(b"\0", strtab + off)
                name = self.d[strtab + off:end].decode(errors="replace")
            else:
                name = raw.rstrip(b"\0").decode(errors="replace")
            value, secnum, _, sclass, naux = struct.unpack_from("<IhHBB", self.d, o + 8)
            if 0 < secnum <= len(self.sections) and name not in out:
                out[name] = self.sections[secnum - 1]["va"] + value
            i += 1 + naux
        return out

    def section_of(self, rva):
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsize"]):
                return s
        raise ValueError(f"RVA {rva:#x} outside all sections")

    def off(self, rva):
        s = self.section_of(rva)
        return s["rawptr"] + rva - s["va"]


def rel32(from_next_rva, to_rva):
    return struct.pack("<i", to_rva - from_next_rva)


def main(argv):
    check_only = "--check" in argv
    paths = [a for a in argv if not a.startswith("--")]
    if len(paths) != 1:
        print(__doc__)
        return 1
    path = paths[0]
    data = bytearray(open(path, "rb").read())
    if MARKER in data:
        print("already patched")
        return 0
    try:
        pe = PE(data)
    except ValueError as e:
        print(f"unsupported: {e}")
        return 1
    sym = pe.symbols()
    need = ["xrDestroySession", "xrCreateSession", "session_list", "session_list_lock", "__imp_free",
            "__imp_EnterCriticalSection", "__imp_LeaveCriticalSection"]
    missing = [n for n in need if n not in sym]
    if missing:
        print(f"unsupported build (no symbols for {', '.join(missing)})")
        return 1
    destroy, create = sym["xrDestroySession"], sym["xrCreateSession"]
    lst, lock = sym["session_list"], sym["session_list_lock"]
    imp_free, imp_enter, imp_leave = sym["__imp_free"], sym["__imp_EnterCriticalSection"], sym["__imp_LeaveCriticalSection"]

    # the wrapper's list entry offsets, from xrCreateSession's list_add_tail: lea rax,[rbx+0x18]; mov [rbx+0x18],rdi ...
    # mov [rbx+0x20],rdx
    body = bytes(data[pe.off(create):pe.off(create) + 0x300])
    if b"\x48\x8d\x43\x18\x48\x89\x7b\x18" not in body or b"\x48\x89\x53\x20" not in body:
        print("unsupported build (session list layout differs)")
        return 1

    # xrDestroySession's success path: mov rcx,rbx; call [rip+__imp_free]; mov eax,esi; add rsp,0x40; pop rbx; pop rsi;
    # pop rbp; ret
    code = bytes(data[pe.off(destroy):pe.off(destroy) + 0x100])
    site = None
    for i in range(len(code) - 20):
        if code[i:i + 5] == b"\x48\x89\xd9\xff\x15":
            target = destroy + i + 9 + struct.unpack_from("<i", code, i + 5)[0]
            if target == imp_free and code[i + 9:i + 19] == b"\x89\xf0\x48\x83\xc4\x40\x5b\x5e\x5d\xc3":
                site = destroy + i
                break
    if site is None:
        print("unsupported build (xrDestroySession differs)")
        return 1
    if check_only:
        print("not patched")
        return 2

    # the injected code goes into the zero padding after .text's contents
    text = pe.section_of(destroy)
    cave = (text["va"] + text["vsize"] + 15) & ~15
    size = 64 + len(MARKER)
    if cave + size > text["va"] + text["rawsize"] or any(data[pe.off(cave):pe.off(cave) + size]):
        print("unsupported build (no room after .text)")
        return 1
    c = bytearray()

    def emit(b):
        c.extend(b)

    def lea_rcx(to):  # lea rcx,[rip+to]
        emit(b"\x48\x8d\x0d" + rel32(cave + len(c) + 7, to))

    def call_ind(to):  # call [rip+to]
        emit(b"\xff\x15" + rel32(cave + len(c) + 6, to))

    lea_rcx(lock)
    call_ind(imp_enter)
    emit(b"\x48\x8b\x43\x18")  # mov rax,[rbx+0x18]   entry.next
    emit(b"\x48\x8b\x53\x20")  # mov rdx,[rbx+0x20]   entry.prev
    emit(b"\x48\x89\x02")      # mov [rdx],rax        prev->next = next
    emit(b"\x48\x89\x50\x08")  # mov [rax+8],rdx      next->prev = prev
    lea_rcx(lock)
    call_ind(imp_leave)
    emit(b"\x48\x89\xd9")      # mov rcx,rbx
    call_ind(imp_free)
    emit(b"\x89\xf0\x48\x83\xc4\x40\x5b\x5e\x5d\xc3")  # the original epilogue
    c.extend(MARKER)
    assert len(c) <= size

    o = pe.off(cave)
    data[o:o + len(c)] = c
    # jmp cave (the 4 bytes after it are never reached; nops for readability)
    o = pe.off(site)
    data[o:o + 9] = b"\xe9" + rel32(site + 5, cave) + b"\x90" * 4
    # .text's virtual size must cover the new code
    struct.pack_into("<I", data, text["hdr"] + 8, cave + len(c) - text["va"])
    del lst  # the list head is reached through the entry itself; kept above only to verify the symbols exist
    open(path, "wb").write(data)
    print(f"patched: xrDestroySession+{site - destroy:#x} -> {cave + pe.image_base:#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
