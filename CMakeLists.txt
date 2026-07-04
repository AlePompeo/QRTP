cmake_minimum_required(VERSION 3.16)
project(qrtp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WITH_OPENCV "Build the real screen+camera optical link (requires OpenCV)" OFF)

set(QRTP_CORE_SOURCES
    src/crc32.cpp
    src/sha256.cpp
    src/framing.cpp
    src/adaptive.cpp
    src/link.cpp
    src/qr_encoder.cpp
    src/protocol.cpp
)

if(WITH_OPENCV)
    find_package(OpenCV REQUIRED)
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
endif()
