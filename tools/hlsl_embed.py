#!/usr/bin/env python3
"""Embed an HLSL shader's source (includes inlined) in a C++ header, compiled at runtime with D3DCompile.

    hlsl_embed.py SHADER.hlsl PROFILE OUT.h [INCLUDE_DIR ...]

The Windows runtime is cross-built on Linux, where fxc is not available; d3dcompiler_47.dll ships with Windows. The
header defines g_<Name>_blob() (see hlsl_compile.h) with the shader's bytecode, compiled once on first use.
"""
import os
import re
import sys

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')


def inline(path, dirs, seen):
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = INCLUDE.match(line)
        if not m:
            out.append(line)
            continue
        name = m.group(1)
        for d in [os.path.dirname(path)] + dirs:
            candidate = os.path.join(d, name)
            if os.path.exists(candidate):
                break
        else:
            sys.exit(f"{path}: cannot find {name}")
        key = os.path.realpath(candidate)
        if key in seen:  # every include here is meant once (no include guards in the FidelityFX headers)
            continue
        seen.add(key)
        out.append(f"// ---- {name}\n")
        out.extend(inline(candidate, dirs, seen))
    return out


def main():
    shader, profile, header, dirs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    name = os.path.splitext(os.path.basename(shader))[0]
    source = "".join(inline(shader, dirs, set()))
    lines = []
    for chunk in source.splitlines(keepends=True):
        lines.append('"' + chunk.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "") + '"')
    with open(header, "w") as h:
        h.write(f"// generated from {os.path.basename(shader)} by tools/hlsl_embed.py\n#pragma once\n#include \"hlsl_compile.h\"\n\n")
        h.write(f"static const char g_{name}_source[] =\n" + "\n".join(lines or ['""']) + ";\n\n")
        h.write(f"inline const od_hlsl::blob & g_{name}_blob()\n{{\n"
                f"\tstatic const od_hlsl::blob b = od_hlsl::compile(g_{name}_source, sizeof(g_{name}_source) - 1, \"{name}.hlsl\", \"{profile}\");\n"
                f"\treturn b;\n}}\n")


if __name__ == "__main__":
    main()
