#!/usr/bin/env python3
"""Fetch a Windows (MSYS2 CLANG64) sysroot for cross-building OpenDesk on Linux with llvm-mingw.

    python3 -I tools/windows/fetch-sysroot.py <dest-dir> [package ...]

CLANG64 packages are built with clang + libc++ + UCRT, the same ABI as llvm-mingw, so their C++ libraries (Qt) link
with it. Dependencies are resolved from the repository database; packages already unpacked are skipped. The sysroot
ends up in <dest-dir>/clang64 (headers, import libraries, DLLs, CMake configs).
"""
import io
import os
import re
import sys
import tarfile
import urllib.request
from compression import zstd

REPO = "https://repo.msys2.org/mingw/clang64/"
PREFIX = "mingw-w64-clang-x86_64-"
DEFAULT = ["qt6-base", "ffmpeg", "lz4", "openssl", "vulkan-headers", "vulkan-loader", "openxr-sdk", "shaderc"]
# runtime-only or toolchain packages the cross build never needs (the compiler comes from llvm-mingw)
SKIP = {"clang", "llvm", "lld", "gcc", "gcc-libs", "crt-git", "headers-git", "winpthreads-git", "libwinpthread-git",
        "python", "perl", "bash", "cmake", "ninja", "pkgconf", "tcl", "tk", "gdb", "llvm-libs", "clang-libs"}


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def read_db():
    raw = fetch(REPO + "clang64.db")
    try:
        data = zstd.decompress(raw)
    except Exception:
        import gzip
        data = gzip.decompress(raw)
    db, provides = {}, {}
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        for m in t.getmembers():
            if not m.name.endswith("/desc"):
                continue
            fields, key = {}, None
            for line in t.extractfile(m).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    key = line.strip("%")
                    fields[key] = []
                elif line and key:
                    fields[key].append(line)
            name = fields["NAME"][0]
            db[name] = fields
            for p in fields.get("PROVIDES", []):
                provides[re.split("[<>=]", p)[0]] = name
    return db, provides


def main():
    dest = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "win-sysroot")
    wanted = [PREFIX + p for p in (sys.argv[2:] or DEFAULT)]
    os.makedirs(dest, exist_ok=True)
    done_dir = os.path.join(dest, ".installed")
    os.makedirs(done_dir, exist_ok=True)
    db, provides = read_db()
    todo, seen = list(wanted), set()
    while todo:
        name = re.split("[<>=]", todo.pop())[0]
        name = name if name in db else provides.get(name, name)
        if name in seen or name not in db or name[len(PREFIX):] in SKIP:
            continue
        seen.add(name)
        todo += db[name].get("DEPENDS", [])
    for name in sorted(seen):
        fields = db[name]
        stamp = os.path.join(done_dir, name + "-" + fields["VERSION"][0])
        if os.path.exists(stamp):
            continue
        print("fetch", fields["FILENAME"][0], flush=True)
        pkg = zstd.decompress(fetch(REPO + fields["FILENAME"][0]))
        with tarfile.open(fileobj=io.BytesIO(pkg)) as t:
            members = [m for m in t.getmembers() if not m.name.startswith(".") and ".." not in m.name.split("/")]
            t.extractall(dest, members=members, filter="data")
        open(stamp, "w").close()
    print(f"sysroot ready: {dest}/clang64 ({len(seen)} packages)")


if __name__ == "__main__":
    main()
