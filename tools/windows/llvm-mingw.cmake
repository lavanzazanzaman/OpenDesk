# Cross-build OpenDesk for Windows on Linux with llvm-mingw and an MSYS2 CLANG64 sysroot (tools/windows/fetch-sysroot.py):
#   python3 -I tools/windows/fetch-sysroot.py ../toolchain/win-sysroot
#   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=tools/windows/llvm-mingw.cmake -DOPENDESK_WIN_SYSROOT=$PWD/../toolchain/win-sysroot/clang64
# Host tools (moc, rcc, uic, glslc) come from the Linux Qt/shaderc install, which must be the sysroot's Qt version.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT LLVM_MINGW)
    set(LLVM_MINGW /opt/llvm-mingw)
endif()
if(NOT OPENDESK_WIN_SYSROOT)
    set(OPENDESK_WIN_SYSROOT ${CMAKE_CURRENT_LIST_DIR}/../../../toolchain/win-sysroot/clang64)
endif()
get_filename_component(OPENDESK_WIN_SYSROOT "${OPENDESK_WIN_SYSROOT}" ABSOLUTE)
# try_compile projects get a fresh cache: pass the paths on
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW OPENDESK_WIN_SYSROOT)

set(CMAKE_C_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH ${OPENDESK_WIN_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_PREFIX_PATH ${OPENDESK_WIN_SYSROOT})

set(QT_HOST_PATH /usr CACHE PATH "Linux Qt install with the matching moc/rcc/uic")

set(ENV{PKG_CONFIG_LIBDIR} ${OPENDESK_WIN_SYSROOT}/lib/pkgconfig:${OPENDESK_WIN_SYSROOT}/share/pkgconfig)
set(ENV{PKG_CONFIG_PATH} "")
# the .pc files say prefix=/clang64: the sysroot is the directory that holds clang64
get_filename_component(_od_sysroot_parent "${OPENDESK_WIN_SYSROOT}" DIRECTORY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${_od_sysroot_parent})
