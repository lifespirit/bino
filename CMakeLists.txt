# Copyright (C) 2022, 2023, 2024, 2025, 2026
# Martin Lambers <marlam@marlam.de>
#
# Copying and distribution of this file, with or without modification, are
# permitted in any medium without royalty provided the copyright notice and this
# notice are preserved. This file is offered as-is, without any warranty.

cmake_minimum_required(VERSION 3.12)
set(CMAKE_FIND_PACKAGE_SORT_ORDER NATURAL)
set(CMAKE_FIND_PACKAGE_SORT_DIRECTION DEC)
set(CMAKE_AUTOMOC ON)

project(Bino LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

# Required: Qt6
find_package(Qt6 6.7.0 REQUIRED COMPONENTS OpenGL OpenGLWidgets Multimedia LinguistTools)

# Experimental USB shutter control (off by default; no NVIDIA GPU required).
option(ENABLE_NVSTUSB "Build experimental NVIDIA 3D Vision USB support" OFF)
if(ENABLE_NVSTUSB)
    if(NOT UNIX)
        message(FATAL_ERROR "NVIDIA 3D Vision USB support currently requires Linux/Unix")
    endif()
    # Use a locally built libnvstusb without installing it system-wide,
    # or fall back to the libnvstusb pkg-config entry.
    set(NVSTUSB_SOURCE_DIR "" CACHE PATH "Path to a built libnvstusb source tree")
    if(NVSTUSB_SOURCE_DIR)
        set(NVSTUSB_LOCAL_LIB "${NVSTUSB_SOURCE_DIR}/src/.libs/libnvstusb.so")
        if(NOT EXISTS "${NVSTUSB_LOCAL_LIB}")
            message(FATAL_ERROR "Build libnvstusb first: ${NVSTUSB_LOCAL_LIB} not found")
        endif()
        add_library(nvstusb_local SHARED IMPORTED)
        set_target_properties(nvstusb_local PROPERTIES
            IMPORTED_LOCATION "${NVSTUSB_LOCAL_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${NVSTUSB_SOURCE_DIR}/include")
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(NVSTUSB REQUIRED IMPORTED_TARGET libnvstusb)
    endif()
endif()
# Optional: QVR for Virtual Reality support
find_package(QVR 4.1.0 QUIET)
if(QVR_FOUND)
    add_definitions(-DWITH_QVR)
    include_directories(${QVR_INCLUDE_DIRS})
    link_directories(${QVR_LIBRARY_DIRS})
endif()

# Optional native Wayland presentation timestamps. The source also builds
# without the protocol so existing non-Linux and older Qt builds stay intact.
option(ENABLE_WAYLAND_PRESENTATION "Observe real Wayland presentation timings (QtWayland >= 6.9)" ON)
if(ENABLE_WAYLAND_PRESENTATION AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND Qt6_VERSION VERSION_GREATER_EQUAL "6.9.0")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    endif()
    find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
    find_file(WAYLAND_PRESENTATION_XML NAMES presentation-time.xml
        HINTS /usr/share/wayland-protocols/stable/presentation-time
              /usr/local/share/wayland-protocols/stable/presentation-time)
    if(TARGET PkgConfig::WAYLAND_CLIENT AND WAYLAND_SCANNER_EXECUTABLE
            AND WAYLAND_PRESENTATION_XML)
        set(WAYLAND_PRESENTATION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/presentation-time-client-protocol.h")
        set(WAYLAND_PRESENTATION_CODE "${CMAKE_CURRENT_BINARY_DIR}/presentation-time-protocol.c")
        add_custom_command(
            OUTPUT "${WAYLAND_PRESENTATION_HEADER}" "${WAYLAND_PRESENTATION_CODE}"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header
                "${WAYLAND_PRESENTATION_XML}" "${WAYLAND_PRESENTATION_HEADER}"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code
                "${WAYLAND_PRESENTATION_XML}" "${WAYLAND_PRESENTATION_CODE}"
            DEPENDS "${WAYLAND_PRESENTATION_XML}"
            VERBATIM)
        set(HAVE_WAYLAND_PRESENTATION TRUE)
    endif()
endif()

# The executable
add_executable(bino
	src/main.cpp src/version.hpp
	src/log.hpp src/log.cpp
	src/tools.hpp src/tools.cpp
	src/screen.hpp src/screen.cpp src/tiny_obj_loader.h
	src/modes.hpp src/modes.cpp
	src/metadata.hpp src/metadata.cpp
	src/playlist.hpp src/playlist.cpp
	src/videoframe.hpp src/videoframe.cpp
	src/videosink.hpp src/videosink.cpp
	src/bino.hpp src/bino.cpp
	src/qvrapp.hpp src/qvrapp.cpp
	src/widget.hpp src/widget.cpp
	src/stereowindow.hpp src/stereowindow.cpp
	src/waylandpresentation.hpp src/waylandpresentation.cpp
	src/nvstusbcontroller.hpp src/nvstusbcontroller.cpp
	src/commandinterpreter.hpp src/commandinterpreter.cpp
	src/playlisteditor.hpp src/playlisteditor.cpp
	src/gui.hpp src/gui.cpp
	src/overlay.hpp src/overlay.cpp
	src/overlay-audio.hpp src/overlay-audio.cpp
	src/overlay-subtitle.hpp src/overlay-subtitle.cpp
	src/overlay-ui.hpp src/overlay-ui.cpp
	src/urlloader.hpp src/urlloader.cpp
	src/digestiblemedia.hpp src/digestiblemedia.cpp
	src/appicon.rc)
qt6_add_translations(bino TS_FILES i18n/bino_de.ts i18n/bino_ja.ts i18n/bino_ka.ts i18n/bino_zh.ts)
qt6_add_resources(bino "misc" PREFIX "/" FILES
	src/shader-color.vert.glsl
	src/shader-color.frag.glsl
	src/shader-view.vert.glsl
	src/shader-view.frag.glsl
	src/shader-display.vert.glsl
	src/shader-display.frag.glsl
	src/shader-vrdevice.vert.glsl
	src/shader-vrdevice.frag.glsl
	res/bino-logo-small.svg
	res/bino-logo-small-512.png
	res/bino-fallback-frame.png)
set_target_properties(bino PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_libraries(bino PRIVATE Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Multimedia ${QVR_LIBRARIES})
if(HAVE_WAYLAND_PRESENTATION)
    target_sources(bino PRIVATE "${WAYLAND_PRESENTATION_CODE}" "${WAYLAND_PRESENTATION_HEADER}")
    target_include_directories(bino PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(bino PRIVATE WITH_WAYLAND_PRESENTATION)
    target_link_libraries(bino PRIVATE PkgConfig::WAYLAND_CLIENT)
    message(STATUS "Experimental Wayland wp_presentation feedback: YES")
else()
    message(STATUS "Experimental Wayland wp_presentation feedback: NO")
endif()
if(ENABLE_NVSTUSB)
    target_compile_definitions(bino PRIVATE WITH_NVSTUSB)
    if(NVSTUSB_SOURCE_DIR)
        target_link_libraries(bino PRIVATE nvstusb_local)
    else()
        target_link_libraries(bino PRIVATE PkgConfig::NVSTUSB)
    endif()
endif()
install(TARGETS bino RUNTIME DESTINATION bin)

# The manual and man page (optional, only if pandoc is found)
find_program(PANDOC NAMES pandoc DOC "pandoc executable")
if(PANDOC)
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/bino-manual.html"
	COMMAND ${PANDOC} "-s" "-t" "html" "--toc" "--css" "bino-manual.css" "${CMAKE_SOURCE_DIR}/doc/bino-manual.md" "-o" "${CMAKE_BINARY_DIR}/bino-manual.html"
	WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
	DEPENDS "${CMAKE_SOURCE_DIR}/doc/bino-manual.md"
	COMMENT "Generating HTML manual with pandoc" VERBATIM)
    add_custom_target(manual ALL DEPENDS "${CMAKE_BINARY_DIR}/bino-manual.html")
    install(FILES "doc/bino-manual.css" "${CMAKE_BINARY_DIR}/bino-manual.html" DESTINATION share/doc/bino)
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/bino.1"
	COMMAND ${PANDOC} "-s" "-t" "man" "${CMAKE_SOURCE_DIR}/doc/bino-manual.md" "-o" "${CMAKE_BINARY_DIR}/bino.1"
	WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
	DEPENDS "${CMAKE_SOURCE_DIR}/doc/bino-manual.md"
	COMMENT "Generating man page with pandoc" VERBATIM)
    add_custom_target(manpage ALL DEPENDS "${CMAKE_BINARY_DIR}/bino.1")
    install(FILES "${CMAKE_BINARY_DIR}/bino.1" DESTINATION share/man/man1)
endif()

# Add auxiliary files for Linux-ish systems
if(UNIX)
    install(FILES "res/bino-logo-small-16.png"  RENAME "org.bino3d.bino.png" DESTINATION share/icons/hicolor/16x16/apps)
    install(FILES "res/bino-logo-small-22.png"  RENAME "org.bino3d.bino.png" DESTINATION share/icons/hicolor/22x22/apps)
    install(FILES "res/bino-logo-small-32.png"  RENAME "org.bino3d.bino.png" DESTINATION share/icons/hicolor/32x32/apps)
    install(FILES "res/bino-logo-small-48.png"  RENAME "org.bino3d.bino.png" DESTINATION share/icons/hicolor/48x48/apps)
    install(FILES "res/bino-logo-small-64.png"  RENAME "org.bino3d.bino.png" DESTINATION share/icons/hicolor/64x64/apps)
    install(FILES "res/bino-logo-small-128.png" RENAME "org.bino3d.bino.png" DESTINATION share/icons/hicolor/128x128/apps)
    install(FILES "res/bino-logo-small-256.png" RENAME "org.bino3d.bino.png" DESTINATION share/icons/hicolor/256x256/apps)
    install(FILES "res/bino-logo-small-512.png" RENAME "org.bino3d.bino.png" DESTINATION share/icons/hicolor/512x512/apps)
    install(FILES "res/bino-logo-small.svg"     RENAME "org.bino3d.bino.svg" DESTINATION share/icons/hicolor/scalable/apps)
    install(FILES "res/org.bino3d.bino.desktop" DESTINATION share/applications)
    install(FILES "res/org.bino3d.bino.metainfo.xml" DESTINATION share/metainfo)
endif()

if(ENABLE_NVSTUSB)
    message(STATUS "Build Bino with NVIDIA 3D Vision USB support: YES")
else()
    message(STATUS "Build Bino with NVIDIA 3D Vision USB support: NO")
endif()
if (QVR_FOUND)
    message(STATUS "Build Bino with QVR support: YES")
else()
    message(STATUS "Build Bino with QVR support: NO")
endif()
if (PANDOC)
    message(STATUS "Build manual and man page with pandoc: YES")
else()
    message(STATUS "Build manual and man page with pandoc: NO")
endif()
