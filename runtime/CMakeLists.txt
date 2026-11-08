# OpenXR runtime side: libovr-opendesk (LibOVR CAPI over the streamer's VR interface) and, on top, the OpenDesk XR port.
find_package(PkgConfig REQUIRED)
find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)
set(OPENDESK_XR_EXTERNAL ${CMAKE_SOURCE_DIR}/third_party)

set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
add_custom_command(OUTPUT ${SHADER_OUT}/composite.spv.inc
    COMMAND ${GLSLC} -O -mfmt=c ${CMAKE_CURRENT_SOURCE_DIR}/libovr-opendesk/shaders/composite.comp -o ${SHADER_OUT}/composite.spv.inc
    DEPENDS libovr-opendesk/shaders/composite.comp)
add_custom_command(OUTPUT ${SHADER_OUT}/composite_ms.spv.inc
    COMMAND ${GLSLC} -O -DMULTISAMPLED=1 -mfmt=c ${CMAKE_CURRENT_SOURCE_DIR}/libovr-opendesk/shaders/composite.comp -o ${SHADER_OUT}/composite_ms.spv.inc
    DEPENDS libovr-opendesk/shaders/composite.comp)
add_custom_command(OUTPUT ${SHADER_OUT}/yuv.spv.inc
    COMMAND ${GLSLC} -O -mfmt=c ${CMAKE_CURRENT_SOURCE_DIR}/libovr-opendesk/shaders/yuv.comp -o ${SHADER_OUT}/yuv.spv.inc
    DEPENDS libovr-opendesk/shaders/yuv.comp)

add_library(ovr-vd SHARED
    libovr-opendesk/ovr_vd.cpp
    libovr-opendesk/compositor.cpp libovr-opendesk/compositor.h
    libovr-opendesk/math.h
    ${SHADER_OUT}/composite.spv.inc ${SHADER_OUT}/composite_ms.spv.inc ${SHADER_OUT}/yuv.spv.inc)
target_include_directories(ovr-vd PRIVATE ${SHADER_OUT} ${OPENDESK_XR_EXTERNAL}/LibOVR/Include ${OPENDESK_XR_EXTERNAL}/LibOVR/Include/Extras)
target_include_directories(ovr-vd PUBLIC ${OPENDESK_XR_EXTERNAL}/LibOVR/Include)
target_link_libraries(ovr-vd PRIVATE odvrclient Vulkan::Vulkan)
target_compile_options(ovr-vd PRIVATE -Wno-deprecated-enum-enum-conversion)

add_executable(ovr-test-app tests/ovr_test_app.cpp)
target_link_libraries(ovr-test-app PRIVATE ovr-vd Vulkan::Vulkan)

pkg_check_modules(OPENXR IMPORTED_TARGET openxr)
if(OPENXR_FOUND)
    # The distro OpenXR headers can be older than the Meta/FB extensions the runtime implements; use the
    # headers vendored with the runtime for the tests, and only link the distro loader.
    set(OPENXR_TEST_HEADERS ${OPENDESK_XR_EXTERNAL})
    add_executable(xr-exit-test tests/xr_exit_test.cpp)
    target_include_directories(xr-exit-test BEFORE PRIVATE ${OPENXR_TEST_HEADERS})
    target_link_libraries(xr-exit-test PRIVATE PkgConfig::OPENXR Vulkan::Vulkan)
    add_executable(xr-body-test tests/xr_body_test.cpp)
    target_include_directories(xr-body-test BEFORE PRIVATE ${OPENXR_TEST_HEADERS})
    target_link_libraries(xr-body-test PRIVATE PkgConfig::OPENXR)
    add_executable(xr-pose-probe tests/xr_pose_probe.cpp)
    target_include_directories(xr-pose-probe BEFORE PRIVATE ${OPENXR_TEST_HEADERS})
    target_link_libraries(xr-pose-probe PRIVATE PkgConfig::OPENXR)
    add_executable(xr-mask-test tests/xr_mask_test.cpp)
    target_include_directories(xr-mask-test BEFORE PRIVATE ${OPENXR_TEST_HEADERS})
    target_link_libraries(xr-mask-test PRIVATE PkgConfig::OPENXR)
endif()

execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD OUTPUT_VARIABLE VDLINUX_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/commit.h "const char* RuntimeCommitHash = \"opendesk-${VDLINUX_COMMIT}\";\n")

# OpenDesk XR (OpenDesk's OpenXR runtime, MIT, Matthieu Bucchianeri) ported to Linux on top of libovr-opendesk
add_library(opendesk-openxr SHARED
    openxr/framework/dispatch.cpp openxr/framework/dispatch.gen.cpp openxr/framework/entry.cpp
    openxr/log.cpp openxr/instance.cpp openxr/system.cpp openxr/session.cpp openxr/space.cpp openxr/action.cpp openxr/mappings.cpp
    openxr/frame.cpp openxr/swapchain.cpp openxr/vulkan_interop.cpp openxr/hand_tracking.cpp openxr/eye_tracking.cpp
    openxr/face_tracking.cpp openxr/body_tracking.cpp openxr/visibility_mask.cpp openxr/display_refresh_rate.cpp openxr/pch.cpp
    openxr/linux_bodystate.cpp openxr/perf_counter.cpp openxr/linux_graphics.cpp openxr/linux_opengl.cpp
    ${OPENDESK_XR_EXTERNAL}/cJSON/cJSON.c
    ${OPENDESK_XR_EXTERNAL}/openvr/samples/drivers/drivers/handskeletonsimulation/src/hand_simulation.cpp)
target_include_directories(opendesk-openxr PRIVATE openxr openxr/framework ${CMAKE_SOURCE_DIR}/third_party ${CMAKE_CURRENT_BINARY_DIR}/generated ${OPENDESK_XR_EXTERNAL}/cJSON
    ${OPENDESK_XR_EXTERNAL}/openvr/samples/drivers/drivers/handskeletonsimulation/src ${OPENDESK_XR_EXTERNAL}/openvr/samples/drivers/utils/vrmath
    ${OPENDESK_XR_EXTERNAL}/openvr/headers
    ${OPENDESK_XR_EXTERNAL}/LibOVR/Include ${OPENDESK_XR_EXTERNAL}/LibOVR/Include/Extras
    openxr/compat ${OPENDESK_XR_EXTERNAL}/DirectXMath/Inc ${OPENDESK_XR_EXTERNAL}/OpenXR-MixedReality/shared
    ${OPENDESK_XR_EXTERNAL}/OpenXR-MixedReality/shared/XrUtility ${OPENDESK_XR_EXTERNAL}/OpenXR-MixedReality/shared/SampleShared)
target_compile_definitions(opendesk-openxr PRIVATE RUNTIME_NAMESPACE=opendesk_openxr STANDALONE_RUNTIME NDEBUG)
target_compile_options(opendesk-openxr PRIVATE -Wno-deprecated-enum-enum-conversion -Wno-changes-meaning -fvisibility=hidden)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
    # Upstream Windows headers use members named after their type (NearFar, Pose).
    # GCC 13 added -Wno-changes-meaning; older GCC needs this compatibility switch.
    target_compile_options(opendesk-openxr PRIVATE -fpermissive)
endif()
target_link_libraries(opendesk-openxr PRIVATE ovr-vd Vulkan::Vulkan)
# relocatable: the streamer copies both libraries next to each other (~/.local/share/opendesk/openxr)
set_target_properties(opendesk-openxr PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
target_link_options(opendesk-openxr PRIVATE -Wl,--no-undefined)

# OpenXR runtime manifest (XR_RUNTIME_JSON, or ~/.config/openxr/1/active_runtime.json)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/opendesk_xr.json
"{\n  \"file_format_version\": \"1.0.0\",\n  \"runtime\": {\n    \"library_path\": \"./libopendesk-openxr.so\",\n    \"name\": \"OpenDesk XR (Linux)\"\n  }\n}\n")

# The runtime is dlopened inside Proton/Steam containers, whose libstdc++ can be older
# than the streamer's. Keep its C++ implementation private, behind the C OpenXR/OVR ABI.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ovr.exports "{ global: ovr_*; ovrvd_*; local: *; };\n")
target_link_options(ovr-vd PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/ovr.exports")
foreach(target ovr-vd opendesk-openxr)
    target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
endforeach()

# packaged layout: <prefix>/lib/opendesk/runtime (the streamer copies it to ~/.local/share/opendesk/openxr)
include(GNUInstallDirs)
install(TARGETS ovr-vd opendesk-openxr LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/opendesk/runtime)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/opendesk_xr.json DESTINATION ${CMAKE_INSTALL_LIBDIR}/opendesk/runtime)
