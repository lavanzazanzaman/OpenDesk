# Building from source

OpenDesk needs CMake 3.25 or newer, Ninja, a C++20 compiler, Qt 6.4 or newer, FFmpeg, PipeWire, PulseAudio client
libraries, libevdev, lz4, OpenSSL, Vulkan, EGL/GBM, Wayland and `glslc`. X11 libraries are optional and only needed for
X11 sessions. Third-party code is vendored under `third_party/`; there are no submodules.

## Install the dependencies

Arch and Arch-based distributions (tested):

```sh
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-wayland ffmpeg libpipewire libpulse libevdev lz4 openssl \
    vulkan-icd-loader vulkan-headers shaderc mesa libglvnd wayland wayland-protocols openxr python \
    libx11 libxext libxrandr libxfixes
```

On Arch you can also build a package instead: `cd packaging/arch && makepkg -si`.

Ubuntu 24.04 (tested) and Debian 13:

```sh
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev qt6-wayland libssl-dev liblz4-dev libevdev-dev \
    libpulse-dev libpipewire-0.3-dev libavcodec-dev libavfilter-dev libavutil-dev libavformat-dev libswscale-dev \
    libwayland-dev wayland-protocols plasma-wayland-protocols libgbm-dev libegl-dev libgles-dev libvulkan-dev \
    glslc libopenxr-dev libfmt-dev libx11-dev libxext-dev libxrandr-dev libxfixes-dev python3
```

Fedora (not tested yet; hardware encoding needs FFmpeg from RPM Fusion):

```sh
sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtwayland ffmpeg-devel \
    pipewire-devel pulseaudio-libs-devel libevdev-devel lz4-devel openssl-devel vulkan-loader-devel vulkan-headers glslc \
    mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel wayland-devel wayland-protocols-devel plasma-wayland-protocols-devel \
    openxr-devel libX11-devel libXext-devel libXrandr-devel libXfixes-devel python3
```

openSUSE Tumbleweed (not tested yet; hardware encoding needs FFmpeg from Packman):

```sh
sudo zypper install gcc-c++ cmake ninja qt6-base-devel qt6-base-private-devel qt6-wayland ffmpeg-devel pipewire-devel \
    libpulse-devel libevdev-devel liblz4-devel libopenssl-devel vulkan-devel shaderc mesa-libgbm-devel Mesa-libEGL-devel \
    Mesa-libGLESv3-devel wayland-devel wayland-protocols-devel plasma-wayland-protocols OpenXR-SDK-devel \
    libX11-devel libXext-devel libXrandr-devel libXfixes-devel python3
```

If you get one of the untested lists working or find a package that has changed name, a pull request is very welcome.

## Build and run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/streamer/opendesk-streamer
```

To install it:

```sh
sudo cmake --install build
```

The first start opens the System check, which reports anything missing on your system and can fix most of it.

## Portable AppImage

`tools/build-appimage.sh` builds `dist/OpenDesk-Linux-x86_64.AppImage` inside Docker, so it needs Docker but none of
the packages above.

## Local checks and releases

Nothing is built on GitHub. Everything runs locally:

```sh
tools/ci.sh linux
```

`tools/release.sh` builds both release files into `dist/`. With `--publish` it also tags the commit and creates the
GitHub release; the AppImages already out there update from that release. Bump `project(... VERSION ...)` in
`CMakeLists.txt` first.
