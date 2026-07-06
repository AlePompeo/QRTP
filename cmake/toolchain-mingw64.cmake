# Cross-compile QRTP for 64-bit Windows from Linux using MinGW-w64 (Fedora packages:
# mingw64-gcc-c++, mingw64-opencv). Usage:
#   cmake -B build-win -S . -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DWITH_OPENCV=ON
#   cmake --build build-win
#
# This exists so the project can be built into real Windows binaries without needing an actual
# Windows machine or Visual Studio — useful for verifying the Windows-specific code paths
# (qr_encoder.cpp's LoadLibrary branch, optical_link.cpp's CAP_DSHOW branch) actually compile,
# which is stronger evidence than "should compile by inspection."

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX}/sys-root/mingw)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Statically link the MinGW runtime (libgcc/libstdc++/libwinpthread) so the resulting .exe needs
# fewer accompanying DLLs to run on a plain Windows machine.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++ -static -lwinpthread")

# Explicit Windows API baseline (2026-07-06). Left unset, mingw-w64's headers default to
# _WIN32_WINNT_WS03 (Windows Server 2003 / XP x64) -- but this project already calls at least one
# Vista-only API unconditionally (SetProcessDPIAware, in main.cpp/gui_win32.cpp), so the real
# floor was already Vista+; mingw-w64 just doesn't gate declarations behind _WIN32_WINNT as
# strictly as the official Windows SDK does, so this compiled without complaint while silently
# relying on that leniency rather than declaring the actual requirement. A binary built against
# the old default would still fail to *launch* at all on genuinely pre-Vista Windows (a missing
# import in a statically-linked DLL call is a load-time failure, regardless of what the PE
# header's declared subsystem version claims) -- Windows 7 (0x0601) is the honest, deliberate
# minimum from here on: comfortably past Vista, broad enough for any realistic target today.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_WIN32_WINNT=0x0601 -DWINVER=0x0601")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_WIN32_WINNT=0x0601 -DWINVER=0x0601")
# _WIN32_WINNT/WINVER above only govern which API *declarations* the headers expose -- the PE
# header's own declared minimum OS version is a separate linker-level field (was 5.02, the
# mingw-w64 default, i.e. Windows XP x64/Server 2003) that doesn't move on its own. Set it to
# match: 6.1 = Windows 7, consistent with the floor set above.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,1 -Wl,--major-os-version,6 -Wl,--minor-os-version,1")
