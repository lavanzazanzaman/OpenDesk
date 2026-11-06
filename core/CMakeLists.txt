find_package(OpenSSL 3 REQUIRED)

# Protocol primitives: plain C++ + OpenSSL, no Qt (reused later by the VR pieces)
add_library(odproto STATIC
    proto/bytes.cpp proto/bytes.h
    proto/crypto.cpp proto/crypto.h
    proto/protocol.cpp proto/protocol.h
)
target_include_directories(odproto PUBLIC proto)
target_link_libraries(odproto PUBLIC OpenSSL::Crypto)

add_executable(odproto-tool tests/odproto_tool.cpp)
target_link_libraries(odproto-tool PRIVATE odproto)

if(WIN32)
    # Windows: DXGI Desktop Duplication + WASAPI + SendInput. The OpenXR runtime, PipeWire/portal capture and the
    # VAAPI encoder are Linux-only and are not built here.
    find_package(Qt6 6.4 REQUIRED COMPONENTS Gui Network)
    find_package(PkgConfig REQUIRED)
    find_package(Vulkan REQUIRED)

    # VR runtime client side (no Qt): used by libovr-opendesk inside OpenXR apps
    add_library(odvrclient STATIC vr/vr_ipc_win.cpp vr/vr_ipc.h vr/vr_client_win.cpp vr/vr_client.h vr/frame_data.h vr/vr_types.h)
    target_include_directories(odvrclient PUBLIC vr engine proto)
    target_compile_definitions(odvrclient PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_link_libraries(odvrclient PUBLIC advapi32)
    pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libavformat libavfilter libswscale libswresample)

    qt_add_library(odengine STATIC
        engine/adapters.cpp engine/adapters.h
        engine/platform.cpp engine/platform.h
        engine/native_engine.cpp engine/native_engine.h
        engine/input_win.cpp engine/input_win.h
        engine/gamepad_win.cpp engine/gamepad_win.h
        engine/audio_win.cpp engine/audio_win.h
        engine/pulse_win.h
        engine/video.cpp engine/video.h engine/video_source.h
        engine/channel.cpp engine/channel.h
        engine/games.cpp engine/games.h
        engine/face_tracking.cpp engine/face_tracking.h
        proto/netbuf.h
        capture/video_library.cpp capture/video_library.h
        capture/file_player.cpp capture/file_player.h
        capture/hardware_encoder_win.cpp capture/hardware_encoder.h
        capture/win_capture.cpp capture/win_capture.h
        capture/displays_win.cpp capture/displays_win.h
        capture/desktop_video.cpp capture/desktop_video.h
        capture/win_gpu.cpp capture/win_gpu.h
        vr/vr_types.cpp vr/vr_types.h
        vr/native_handle.h vr/vk_buffers.cpp vr/vk_buffers.h
        vr/vr_ipc_win.cpp vr/vr_ipc.h
        vr/vr_server_win.cpp vr/vr_server_state.cpp vr/vr_server.h
        vr/vr_sender.cpp vr/vr_sender.h
        vr/runtime_registration_win.cpp vr/runtime_registration.h
    )
    target_include_directories(odengine PUBLIC engine capture proto vr)
    target_compile_definitions(odengine PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00
        OPENDESK_RUNTIME_DIR="${CMAKE_BINARY_DIR}/runtime"
        OPENDESK_TOOLS_DIR="${CMAKE_SOURCE_DIR}/tools")
    target_link_libraries(odengine PUBLIC odproto odvrclient Qt6::Gui Qt6::Network
        PRIVATE PkgConfig::LZ4 PkgConfig::FFMPEG Vulkan::Vulkan
        d3d11 dxgi ole32 oleaut32 uuid winmm iphlpapi ws2_32 ksguid advapi32)
    set_target_properties(odengine PROPERTIES AUTOMOC ON)

    # VR IPC round trip against the real server (no headset, no GPU frames needed)
    add_executable(vr-test-client tests/vr_test_client.cpp)
    target_link_libraries(vr-test-client PRIVATE odengine)
    add_executable(vr-server-test tests/vr_server_test.cpp)
    target_link_libraries(vr-server-test PRIVATE odengine)

    # The other tests exercise the Linux capture stack and are not part of the Windows build.
    return()
endif()

# ---------------------------------------------------------------------------------------------------------------
# Linux / Steam runtime: Qt D-Bus, PipeWire or X11 capture, VAAPI/NVENC, the OpenXR runtime client.

# VR runtime client side (no Qt): used by libovr-opendesk inside OpenXR apps and by test clients
add_library(odvrclient STATIC vr/vr_ipc.cpp vr/vr_ipc.h vr/vr_client.cpp vr/vr_client.h vr/frame_data.h vr/vr_types.h)
target_include_directories(odvrclient PUBLIC vr engine proto)
set_target_properties(odvrclient PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Native engine runtime (Qt Network, runs on its own thread inside the streamer)
find_package(Qt6 6.4 REQUIRED COMPONENTS Gui Network DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
pkg_check_modules(EVDEV REQUIRED IMPORTED_TARGET libevdev)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple libpulse)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavfilter libavutil libavformat libswscale)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(X11CAP IMPORTED_TARGET x11 xext xrandr xfixes) # X11 sessions' screen capture (optional)
pkg_check_modules(GPU REQUIRED IMPORTED_TARGET gbm egl glesv2)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(PLASMA_PROTOCOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocols)

# Wayland client glue: KWin screen cast (+ virtual outputs), output order, logical output geometry
set(WL_GEN ${CMAKE_CURRENT_BINARY_DIR}/wayland)
file(MAKE_DIRECTORY ${WL_GEN})
set(WL_SOURCES)
foreach(xml ${PLASMA_PROTOCOLS_DIR}/zkde-screencast-unstable-v1.xml ${PLASMA_PROTOCOLS_DIR}/kde-output-order-v1.xml
        ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml)
    get_filename_component(base ${xml} NAME_WE)
    add_custom_command(OUTPUT ${WL_GEN}/${base}-client-protocol.h ${WL_GEN}/${base}-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${WL_GEN}/${base}-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${WL_GEN}/${base}-protocol.c
        DEPENDS ${xml})
    list(APPEND WL_SOURCES ${WL_GEN}/${base}-client-protocol.h ${WL_GEN}/${base}-protocol.c)
endforeach()
set_source_files_properties(${WL_SOURCES} PROPERTIES LANGUAGE C)
find_package(Vulkan REQUIRED)
qt_add_library(odengine STATIC
    engine/adapters.cpp engine/adapters.h
    engine/platform.cpp engine/platform.h
    engine/native_engine.cpp engine/native_engine.h
    engine/input.cpp engine/input.h
    engine/gamepad.cpp engine/gamepad.h
    engine/audio.cpp engine/audio.h
    engine/pulse.cpp engine/pulse.h
    engine/video.cpp engine/video.h engine/video_source.h
    engine/channel.cpp engine/channel.h
    engine/games.cpp engine/games.h
    engine/wineopenxr_fix.cpp engine/wineopenxr_fix.h
    proto/netbuf.h
    capture/video_library.cpp capture/video_library.h
    capture/file_player.cpp capture/file_player.h
    vr/vr_types.cpp vr/vr_types.h
    engine/face_tracking.cpp engine/face_tracking.h
    vr/dmabuf.cpp vr/dmabuf.h vr/vk_buffers.cpp vr/vk_buffers.h
    vr/test_pattern.cpp vr/test_pattern.h
    vr/vr_sender.cpp vr/vr_sender.h
    vr/vr_server.cpp vr/vr_server_state.cpp vr/vr_server.h
    vr/runtime_registration.cpp vr/runtime_registration.h
    capture/portal.cpp capture/portal.h
    capture/pipewire_capture.cpp capture/pipewire_capture.h
    capture/x11_capture.h
    capture/hardware_encoder.cpp capture/hardware_encoder.h
    capture/dmabuf_readback.cpp capture/dmabuf_readback.h
    capture/kwin_screencast.cpp capture/kwin_screencast.h
    capture/displays.cpp capture/displays.h
    capture/desktop_video.cpp capture/desktop_video.h
    ${WL_SOURCES}
)
target_include_directories(odengine PRIVATE ${WL_GEN})
target_include_directories(odengine PUBLIC engine capture proto vr)
target_compile_definitions(odengine PRIVATE OPENDESK_RUNTIME_DIR="${CMAKE_BINARY_DIR}/runtime" OPENDESK_TOOLS_DIR="${CMAKE_SOURCE_DIR}/tools")
target_link_libraries(odengine PUBLIC odproto odvrclient Qt6::Gui Qt6::Network Qt6::DBus PRIVATE PkgConfig::LZ4 PkgConfig::EVDEV PkgConfig::PULSE PkgConfig::PIPEWIRE PkgConfig::FFMPEG PkgConfig::WAYLAND PkgConfig::GPU Vulkan::Vulkan)
set_target_properties(odengine PROPERTIES AUTOMOC ON)
if(X11CAP_FOUND)
    target_sources(odengine PRIVATE capture/x11_capture.cpp)
    target_link_libraries(odengine PRIVATE PkgConfig::X11CAP)
    target_compile_definitions(odengine PRIVATE OPENDESK_HAVE_X11)
else()
    target_sources(odengine PRIVATE capture/x11_stub.cpp)
endif()

add_executable(video-test tests/video_test.cpp)
target_link_libraries(video-test PRIVATE odengine)
target_link_libraries(video-test PRIVATE PkgConfig::FFMPEG)

add_executable(dmabuf-readback-test tests/dmabuf_readback_test.cpp)
target_link_libraries(dmabuf-readback-test PRIVATE odengine PkgConfig::GPU)

add_executable(vr-test-client tests/vr_test_client.cpp)
target_link_libraries(vr-test-client PRIVATE odengine)

add_executable(vr-server-test tests/vr_server_test.cpp)
target_link_libraries(vr-server-test PRIVATE odengine)

# vd-wineopenxr-fix [--check] wineopenxr.dll: the streamer's Proton WineOpenXR fix from the command line
add_executable(vd-wineopenxr-fix tests/wineopenxr_fix_tool.cpp)
target_link_libraries(vd-wineopenxr-fix PRIVATE odengine)

add_executable(openvr-smoke tests/openvr_smoke.cpp)
target_link_libraries(openvr-smoke PRIVATE dl)

add_executable(openvr-render tests/openvr_render.cpp)
target_include_directories(openvr-render PRIVATE ${CMAKE_SOURCE_DIR}/third_party/openvr/headers)
target_link_libraries(openvr-render PRIVATE Vulkan::Vulkan dl)
