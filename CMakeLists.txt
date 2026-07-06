cmake_minimum_required(VERSION 3.16)
project(qrtp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Was never set, so every prior Windows build (v14 through v22) compiled with no optimization
# flags at all (bare -std=c++17) -- this project's own code (protocol/adaptive/optical_link/etc.),
# not the separately-built OpenCV DLLs, which were already Release-optimized. Zero-risk to fix:
# default to Release unless the invoker explicitly asked for something else.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(WITH_OPENCV "Build the real screen+camera optical link (requires OpenCV)" OFF)

set(QRTP_CORE_SOURCES
    src/crc32.cpp
    src/sha256.cpp
    src/base64.cpp
    src/framing.cpp
    src/adaptive.cpp
    src/link.cpp
    src/qr_encoder.cpp
    src/protocol.cpp
    src/broadcast.cpp
)

if(WITH_OPENCV)
    find_package(OpenCV REQUIRED)
    # Fedora's mingw64-opencv package ships an OpenCVConfig.cmake with a path-computation bug
    # (one "../" too many walking up from its own location), which makes OpenCV_INCLUDE_DIRS
    # resolve to a nonexistent directory when cross-compiling. Detect and fix it by deriving the
    # include dir from the toolchain's sysroot instead. Native/vcpkg OpenCV installs never hit
    # this (the guard condition is false there), so this is a no-op everywhere else.
    if(MINGW AND NOT EXISTS "${OpenCV_INCLUDE_DIRS}/opencv2/opencv.hpp")
        set(OpenCV_INCLUDE_DIRS "${CMAKE_FIND_ROOT_PATH}/include/opencv4")
        message(STATUS "Worked around broken OpenCV_INCLUDE_DIRS -> ${OpenCV_INCLUDE_DIRS}")
    endif()
    list(APPEND QRTP_CORE_SOURCES src/optical_link.cpp)
endif()

add_library(qrtp_core STATIC ${QRTP_CORE_SOURCES})
target_include_directories(qrtp_core PUBLIC include)

if(WITH_OPENCV)
    target_compile_definitions(qrtp_core PUBLIC QRTP_WITH_OPENCV)
    target_include_directories(qrtp_core PUBLIC ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(qrtp_core PUBLIC ${OpenCV_LIBS})
endif()

find_package(Threads REQUIRED)
target_link_libraries(qrtp_core PUBLIC Threads::Threads)

if(UNIX)
    # dlopen/dlsym for qr_encoder.cpp's runtime-loaded libqrencode (Windows uses
    # LoadLibrary/GetProcAddress instead, which need no extra link library).
    target_link_libraries(qrtp_core PUBLIC ${CMAKE_DL_LIBS})
endif()

add_executable(qrtp src/main.cpp)
target_link_libraries(qrtp PRIVATE qrtp_core)

enable_testing()
add_executable(qrtp_tests tests/test_main.cpp)
target_link_libraries(qrtp_tests PRIVATE qrtp_core)
add_test(NAME qrtp_tests COMMAND qrtp_tests)

# Native Win32 GUI (qrtp_gui.exe) — only meaningful on Windows with the real optical link enabled,
# since its entire purpose is driving a screen+camera transfer. WIN32 marks it as a GUI-subsystem
# executable (no console window). Statically linking the runtime keeps it a single portable .exe
# with no "Visual C++ Redistributable" needed on the target machine.
if(WIN32 AND WITH_OPENCV)
    add_executable(qrtp_gui WIN32 src/gui_win32.cpp)
    target_link_libraries(qrtp_gui PRIVATE qrtp_core comdlg32 shell32 ole32)
    set_property(TARGET qrtp_gui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(MINGW)
        # MinGW's default CRT startup object looks for an (ANSI) WinMain; -municode switches it to
        # the one that calls wWinMain instead, matching gui_win32.cpp's wide entry point. MSVC picks
        # the right startup automatically from whichever of WinMain/wWinMain is defined, so it
        # doesn't need this — this is a MinGW-specific linker requirement.
        target_link_options(qrtp_gui PRIVATE -municode)
    endif()
endif()
