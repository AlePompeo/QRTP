cmake_minimum_required(VERSION 3.16)
project(qrtp LANGUAGES CXX)

# Only needed for qrtp_gui.exe's embedded icon (windows/qrtp.rc) — RC is a no-op non-issue on
# non-Windows configurations, since that target itself is gated behind WIN32 below, but the
# compiler needs to be known before any target references an .rc source file.
if(WIN32)
    enable_language(RC)
endif()

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
    list(APPEND QRTP_CORE_SOURCES src/optical_link.cpp src/gui_common.cpp)
endif()

# opencv_contrib's wechat_qrcode module (a real CNN-based QR detector, see optical_link.hpp) is
# confirmed present in Fedora's stock opencv-devel package, but genuinely *absent* from Debian
# 12's and Ubuntu 22.04/24.04's stock libopencv-dev -- not universally packaged across distros.
# Checked via the header's presence rather than assuming any one distro's behavior generalizes.
set(QRTP_HAVE_WECHAT_QRCODE FALSE)
if(WITH_OPENCV)
    find_file(QRTP_WECHAT_QRCODE_HEADER "opencv2/wechat_qrcode.hpp" PATHS ${OpenCV_INCLUDE_DIRS} NO_DEFAULT_PATH)
    if(QRTP_WECHAT_QRCODE_HEADER)
        set(QRTP_HAVE_WECHAT_QRCODE TRUE)
        message(STATUS "opencv2/wechat_qrcode.hpp found -- WeChatQRCode decoder available")
    else()
        message(STATUS "opencv2/wechat_qrcode.hpp not found -- WeChatQRCode decoder unavailable, "
                        "classic cv::QRCodeDetector only (see OpticalLinkConfig::use_wechat_qrcode)")
    endif()
endif()

add_library(qrtp_core STATIC ${QRTP_CORE_SOURCES})
target_include_directories(qrtp_core PUBLIC include)

if(WITH_OPENCV)
    target_compile_definitions(qrtp_core PUBLIC QRTP_WITH_OPENCV)
    if(QRTP_HAVE_WECHAT_QRCODE)
        target_compile_definitions(qrtp_core PUBLIC QRTP_HAVE_WECHAT_QRCODE)
    endif()
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
    add_executable(qrtp_gui WIN32 src/gui_win32.cpp windows/qrtp.rc)
    target_include_directories(qrtp_gui PRIVATE windows) # resource.h, shared with qrtp.rc
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

# Native GTK3 GUI (qrtp_gui) for Linux — same role as the Win32 GUI above, different toolkit
# (gtk+-3.0 is in the package repos of virtually every mainstream Linux distribution, unlike Qt on
# non-KDE systems). Not REQUIRED: a CLI-only build (just `qrtp`) must keep working on a machine
# without GTK3 installed, so this only adds the target if pkg-config actually finds it.
if(UNIX AND NOT APPLE AND WITH_OPENCV)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTK3 gtk+-3.0)
    endif()
    if(GTK3_FOUND)
        add_executable(qrtp_gui src/gui_gtk.cpp)
        target_link_libraries(qrtp_gui PRIVATE qrtp_core ${GTK3_LIBRARIES})
        target_include_directories(qrtp_gui PRIVATE ${GTK3_INCLUDE_DIRS})
        target_compile_options(qrtp_gui PRIVATE ${GTK3_CFLAGS_OTHER})
    else()
        message(STATUS "gtk+-3.0 not found via pkg-config -- skipping qrtp_gui (qrtp, the CLI, "
                        "still builds fine). Install gtk3-devel (Fedora), libgtk-3-dev "
                        "(Debian/Ubuntu), or gtk3 (Arch) to enable it.")
    endif()
endif()

# Browser/WASM target (qrtp_web) -- entirely additive, no existing target above is touched or
# reconfigured by this. Built with `emcmake cmake -B build-wasm -DWITH_WASM=ON && cmake --build
# build-wasm --target qrtp_web` (the explicit --target matters: this file's other executables
# (qrtp/qrtp_tests/qrtp_gui) are still defined under an Emscripten configure too, since none of
# this is gated behind WITH_WASM -- building the specific target instead of the generator's
# default `all` avoids ever needing to know whether those happen to also compile under emcc).
#
# QRTP_CORE_SOURCES (the same OpenCV-free sources as qrtp_core above -- crc32/sha256/base64/
# framing/adaptive/link/protocol/broadcast) is genuinely 100% portable C++17 with no platform
# dependency, so it's reused byte-for-byte here, just listed again for a separate executable
# target rather than restructuring qrtp_core's own conditional (which would mean editing existing
# lines). optical_link.cpp/gui_common.cpp are OpenCV/native-window specific and have no place
# here; qr_encoder.cpp's dlopen has no WASM equivalent, so qr_encoder_wasm.cpp (static-linking
# libqrencode instead) stands in for it -- see that file's own header comment.
option(WITH_WASM "Build the browser/WebAssembly target (requires configuring with emcmake)" OFF)
if(WITH_WASM)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "WITH_WASM requires configuring with emcmake, e.g.: "
                             "emcmake cmake -B build-wasm -DWITH_WASM=ON")
    endif()

    set(QRTP_WASM_SOURCES
        src/crc32.cpp
        src/sha256.cpp
        src/base64.cpp
        src/framing.cpp
        src/adaptive.cpp
        src/link.cpp
        src/protocol.cpp
        src/broadcast.cpp
        src/qr_encoder_wasm.cpp
        src/wasm_link.cpp
    )

    # libqrencode and zxing-cpp are fetched as source and built for WASM under third_party/ (see
    # PROGRESS.md for the exact emcmake/emcc build steps) -- not committed to the repo, same
    # treatment as the Windows distribution's DLLs, and not pulled in via add_subdirectory() to
    # keep their own CMake machinery (tests, examples, unrelated writer/C-API options) out of this
    # project's own configure.
    set(QRTP_LIBQRENCODE_DIR "${CMAKE_SOURCE_DIR}/third_party/libqrencode")
    set(QRTP_ZXINGCPP_DIR "${CMAKE_SOURCE_DIR}/third_party/zxing-cpp")
    # find_library() isn't used here: the Emscripten toolchain file restricts library search to
    # CMAKE_FIND_ROOT_PATH (its own sysroot) unconditionally, the same cross-compiling gotcha
    # already worked around for the MinGW toolchain elsewhere in this project (see
    # cmake/toolchain-mingw64.cmake) -- so PATHS/NO_DEFAULT_PATH here would just never match
    # anything under third_party/. These are pre-built artifacts (like the Windows dist's DLLs),
    # not something the linker needs to search for, so a direct, checked path is simpler anyway.
    set(QRTP_LIBQRENCODE_LIB "${QRTP_LIBQRENCODE_DIR}/build-wasm/libqrencode.a")
    set(QRTP_ZXING_LIB "${QRTP_ZXINGCPP_DIR}/build-wasm/core/libZXing.a")
    if(NOT EXISTS "${QRTP_LIBQRENCODE_LIB}" OR NOT EXISTS "${QRTP_ZXING_LIB}")
        message(FATAL_ERROR "WITH_WASM: libqrencode/zxing-cpp not built yet under third_party/ "
                             "(see PROGRESS.md for the build steps)")
    endif()

    add_executable(qrtp_web ${QRTP_WASM_SOURCES})
    target_include_directories(qrtp_web PRIVATE
        include
        "${QRTP_LIBQRENCODE_DIR}"
        "${QRTP_ZXINGCPP_DIR}/core/src"
        "${QRTP_ZXINGCPP_DIR}/build-wasm/core" # generated Version.h
    )
    target_compile_features(qrtp_web PRIVATE cxx_std_17)
    # Emscripten disables C++ exception unwinding by default (-fno-exceptions-like behavior at
    # the wasm level) -- any throw (e.g. qr_encoder_wasm.cpp's payload-too-large check) calls
    # abort() instead of unwinding, which doesn't just fail that one call: the whole module
    # instance is left permanently unusable afterward. -fwasm-exceptions (native WASM exception
    # handling, well supported by this Emscripten version) must be passed at both compile and
    # link time, consistently, everywhere something might throw or catch across a call boundary
    # -- including the pre-built libqrencode/zxing-cpp static libs, so those must be (re)built
    # with the same flag (see PROGRESS.md for the updated build steps).
    target_compile_options(qrtp_web PRIVATE -fwasm-exceptions)
    target_link_libraries(qrtp_web PRIVATE "${QRTP_LIBQRENCODE_LIB}" "${QRTP_ZXING_LIB}")
    target_link_options(qrtp_web PRIVATE
        --bind
        -fwasm-exceptions
        -sASYNCIFY
        # Asyncify's default stack (4KB) is too small for this call depth (Sender/Receiver's
        # ARQ loop through std::function callbacks, std::string/std::vector, exceptions) --
        # silently aborts partway through a transfer with no useful error message otherwise.
        -sASYNCIFY_STACK_SIZE=65536
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createQrtpModule
        -sALLOW_MEMORY_GROWTH=1
        # web,node (not just web): the browser app (web/main.js) only ever runs this in a
        # browser, but the same build also doubles as a Node-loadable module for the
        # hardware-free round-trip sanity check (see PROGRESS.md) -- same relationship
        # LoopbackLink already has to real hardware for the native protocol tests.
        -sENVIRONMENT=web,node
        -sFORCE_FILESYSTEM=1 # MEMFS: lets run_send() take a plain path, unchanged from Sender::send_file's native signature -- JS writes the picked File's bytes there first (FS.writeFile), no C++-side change needed for file input.
        # None of these are attached to Module by default any more (tree-shaken unless listed
        # explicitly): FS (file writes for send()), HEAPU8+_malloc+_free (JS writes captured
        # camera pixels straight into the WASM heap before calling decodeQrRgba -- see web/main.js
        # -- rather than paying embind's per-element marshaling cost on a multi-megapixel buffer
        # every polled frame).
        -sEXPORTED_RUNTIME_METHODS=FS,HEAPU8
        -sEXPORTED_FUNCTIONS=_malloc,_free
    )
    set_target_properties(qrtp_web PROPERTIES OUTPUT_NAME "qrtp_web" SUFFIX ".js")
endif()
