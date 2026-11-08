find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable(opendesk-streamer
    src/main.cpp
    src/mainwindow.cpp src/mainwindow.h
    src/pages.cpp src/pages.h
    src/settings.cpp src/settings.h
    src/engine.cpp src/engine.h
    src/sysinfo.cpp src/sysinfo.h
    src/setup.cpp src/setup.h
    src/shortcuts.cpp src/shortcuts.h
    src/updater.cpp src/updater.h
)

qt_add_resources(opendesk-streamer "theme" PREFIX "/" FILES theme/violet.qss theme/check.svg theme/arrow.svg theme/switch-on.svg theme/switch-off.svg theme/switch-off-disabled.svg)

# Project-owned UI artwork (no third-party logos are redistributed).
file(GLOB OPENDESK_ASSETS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS assets/opendesk/*)
qt_add_resources(opendesk-streamer "opendeskassets" PREFIX "/" FILES ${OPENDESK_ASSETS})

target_compile_definitions(opendesk-streamer PRIVATE
    OPENDESK_ENGINE_DIR="${PROJECT_SOURCE_DIR}/engine"
    OPENDESK_VERSION="${PROJECT_VERSION}"
    OPENDESK_UPSTREAM_VERSION="1.34.23")
target_link_libraries(opendesk-streamer PRIVATE Qt6::Widgets odengine)
target_compile_definitions(opendesk-streamer PRIVATE OPENDESK_TOOLS_DIR="${CMAKE_SOURCE_DIR}/tools")

# packaged layout (Arch PKGBUILD, make install): resources under <prefix>/lib/opendesk (od::resource_dir finds them)
include(GNUInstallDirs)
install(TARGETS opendesk-streamer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS ${PROJECT_SOURCE_DIR}/tools/get_xrizer.sh ${PROJECT_SOURCE_DIR}/tools/fix_wineopenxr.py
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/opendesk/tools)
install(FILES ${PROJECT_SOURCE_DIR}/tools/appimage/opendesk-streamer.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${PROJECT_SOURCE_DIR}/tools/appimage/opendesk.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

