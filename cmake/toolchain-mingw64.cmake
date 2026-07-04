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
