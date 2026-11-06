#!/usr/bin/env python3
"""Run in the Ubuntu build container: stage the dependency closure without GPU drivers."""
import os
from pathlib import Path
import re
import shutil
import subprocess

root = Path('/out/AppDir')
if root.exists(): shutil.rmtree(root)
bindir = root / 'usr/bin'
libdir = root / 'usr/lib'
resources = libdir / 'opendesk'
licenses = root / 'usr/share/doc/opendesk/dependencies'
for p in (bindir, resources, licenses): p.mkdir(parents=True, exist_ok=True)

def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)

copy('/out/build/streamer/opendesk-streamer', bindir / 'opendesk-streamer')
copy('/out/build/core/video-test', bindir / 'opendesk-video-test')
for name in ('libovr-vd.so', 'libopendesk-openxr.so', 'opendesk_xr.json'):
    copy('/out/runtime-build/runtime/' + name, resources / 'runtime' / name)
shutil.copytree('/out/xrizer', resources / 'xrizer')
for name in ('get_xrizer.sh', 'fix_wineopenxr.py'):
    copy('/src/tools/' + name, resources / 'tools' / name)
plugins = Path('/usr/lib/x86_64-linux-gnu/qt6/plugins')
for group in ('platforms', 'imageformats', 'iconengines', 'tls', 'platforminputcontexts',
              'wayland-decoration-client', 'wayland-graphics-integration-client',
              'wayland-shell-integration', 'xcbglintegrations', 'egldeviceintegrations'):
    if (plugins / group).exists(): shutil.copytree(plugins / group, root / 'usr/plugins' / group)
(bindir / 'qt.conf').write_text('[Paths]\nPrefix=..\nPlugins=plugins\n')

# Use the host graphics stack and C++ runtime when present. In particular, bundling
# an older libstdc++ ahead of a new Mesa driver prevents that driver from loading.
# libpipewire loads the host's SPA plugins from the path it was built with (Debian's
# /usr/lib/x86_64-linux-gnu/spa-0.2 here): only the host's own copy finds them.
# The host's GPU drivers (Mesa, NVIDIA) are loaded into our process and need the host's versions of their own
# dependencies too: a bundled older libxcb-dri3 already broke RADV ("undefined symbol: xcb_dri3_import_syncobj_checked").
# Hosts that run this build (glibc >= 2.39) have these at least as new as ours.
optional = re.compile(r'lib(?:stdc\+\+|gcc_s|GL|OpenGL|EGL|GLES|gbm|drm|va|vulkan|wayland|xkbcommon|fontconfig|freetype|harfbuzz|pipewire'
                      r'|xcb|X11|Xext|Xfixes|Xrender|Xau|Xdmcp|Xxf86vm|xshmfence|expat|zstd|lzma|bz2|ffi|udev|systemd|elf|sensors'
                      r'|z\.so|pciaccess|LLVM'
                      # Qt and FFmpeg: the host's when it has them all (AppRun decides per group, see there)
                      r'|Qt6|avcodec|avformat|avutil|avfilter|avdevice|swscale|swresample|postproc)')
system = re.compile(r'^(?:ld-linux|lib(?:c|m|dl|pthread|rt|resolv|util|anl|nss_\w+)\.so)')
todo = [p for p in root.rglob('*') if p.is_file() and p.read_bytes()[:4] == b'\x7fELF']
seen = set()
packages = set()
while todo:
    elf = todo.pop()
    for line in subprocess.check_output(['ldd', str(elf)], text=True).splitlines():
        if '=> not found' in line: raise RuntimeError(f'{elf}: {line}')
        match = re.search(r'^\s*(\S+) => (/\S+)', line)
        if not match: continue
        soname, source = match.groups()
        if soname in seen or system.match(soname): continue
        seen.add(soname)
        # The runtime libraries are staged together and install together.
        if soname == 'libovr-vd.so': continue
        destination = libdir / ('optional' if optional.match(soname) else '') / soname
        copy(source, destination)
        todo.append(destination)
        owner = subprocess.run(['dpkg-query', '-S', source], text=True, capture_output=True)
        if owner.returncode:
            owner = subprocess.run(['dpkg-query', '-S', str(Path(source).resolve())], text=True, capture_output=True)
        if owner.returncode == 0:
            packages.add(owner.stdout.split(': ')[0])

for elf in root.rglob('*'):
    if not elf.is_file() or elf.read_bytes()[:4] != b'\x7fELF': continue
    if elf.name.endswith('.so') and 'xrizer' in elf.parts: continue
    relative = os.path.relpath(libdir, elf.parent)
    subprocess.run(['patchelf', '--set-rpath', '$ORIGIN:$ORIGIN/' + relative, str(elf)], check=True)
    subprocess.run(['strip', '--strip-unneeded', str(elf)], check=True)
for package in sorted(packages):
    copyright = Path('/usr/share/doc') / package.split(':')[0] / 'copyright'
    if copyright.exists(): copy(copyright, licenses / (package.replace(':', '_') + '.copyright'))
(licenses / 'packages.txt').write_text(subprocess.check_output(
    ['dpkg-query', '-W', '-f=${Package} ${Version}\n', *sorted(packages)], text=True))
# AppRun takes the host's Qt only if it is at least the Qt this was built with (6.N symbol versions)
qt_version = subprocess.check_output(['dpkg-query', '-W', '-f=${Version}', 'libqt6core6t64'], text=True).split('+')[0]
(resources / 'qt-version').write_text('.'.join(qt_version.split('.')[:2]) + '\n')
# ... and only if it has the Qt libraries the program itself loads (plugin-only ones like the Wayland EGL
# integration differ between Qt versions; the host's plugins bring their own)
needed = subprocess.check_output(['ldd', '/out/build/streamer/opendesk-streamer'], text=True)
(resources / 'qt-libs').write_text('\n'.join(sorted(set(re.findall(r'(libQt6\w+\.so\.6)', needed)))) + '\n')
copy('/src/tools/appimage/AppRun', root / 'AppRun')
copy('/src/tools/appimage/opendesk-streamer.desktop', root / 'opendesk-streamer.desktop')
copy('/src/tools/appimage/opendesk.svg', root / 'opendesk.svg')
copy('/src/README.md', root / 'usr/share/doc/opendesk/README.md')
for folder in ('runtime/openxr', 'third_party'):
    for path in (Path('/src') / folder).rglob('*'):
        if path.is_file() and path.name.lower().startswith(('license', 'copying')):
            copy(path, licenses / 'source' / path.relative_to('/src'))
print(f'AppDir ready: {len(seen)} shared libraries; graphics drivers use the host installation')
