# QRTP — Progress

## Purpose
Build QRTP, an air-gapped, TCP-like file transfer protocol running over a screen-to-camera
optical channel: sliding window + selective-repeat ARQ, per-frame CRC32, end-to-end SHA-256, and
adaptive performance control (chunk size/EC level, FPS, window size).

## Architecture
- **framing.\* / crc32.\* / sha256.\*** — Binary frame header pack/unpack, CRC32, SHA-256 (all
  hand-implemented, no dependencies)
- **qr_encoder.\*** — Real QR generation via `libqrencode`, loaded at runtime with `dlopen` (no
  `-devel` package needed)
- **link.\*** — `ILink` interface + `LoopbackLink` (thread-safe, hardware-free, injects synthetic
  loss/corruption for testing)
- **optical_link.\*** — Real screen+camera `ILink`, gated behind `make WITH_OPENCV=1`
- **protocol.\*** — `Sender`/`Receiver`: handshake, sliding window, selective-repeat ARQ, teardown
- **adaptive.\*** — AIMD controller: window size, chunk size, EC level, FPS, RTT/RTO estimation
- **main.cpp** — CLI (`render-qr` / `demo` / `send` / `receive`)
- **Makefile** — `make` / `make test` / `make WITH_OPENCV=1`, C++17, header dependency tracking

Implemented in C++ (switched from the originally-sketched Python module layout — see the language
decision note below).

## Steps

| # | Step | Status |
|---|------|--------|
| 1 | Project setup (Makefile, header dependency tracking) | ✅ Done |
| 2 | Frame format & integrity (`framing.*`, `crc32.*`, `sha256.*`) | ✅ Done |
| 3 | Real QR encoder via `libqrencode` (dlopen) (`qr_encoder.*`) | ✅ Done |
| 4 | Loopback channel for hardware-free testing (`link.*`) | ✅ Done |
| 5 | Protocol state machine: handshake + sliding window + selective-repeat ARQ (`protocol.*`) | ✅ Done |
| 6 | RTT/RTO estimation (Jacobson/Karels) | ✅ Done |
| 7 | Adaptive controller: AIMD on window size, chunk size, EC level, FPS (`adaptive.*`) | ✅ Done |
| 8 | CLI entry point (`main.cpp`: `render-qr` / `demo`) | ✅ Done |
| 9 | End-to-end test over loopback with synthetic loss/corruption | ✅ Done — see `make test` |
| 10 | Real screen+camera backend (`optical_link.*`, OpenCV) | ✅ Written, ⬜ **not build-tested** — opencv-devel isn't installed in this environment |
| 11 | End-to-end test between two physical machines (screen + webcam) | ⬜ Pending — needs real hardware |
| 12 | Calibration burst (`CALIB` frames) to auto-tune initial FPS/QR density | ⬜ Pending — frame type reserved, handshake works without it (first `DATA` frame implies ACK) |
| 13 | Half-duplex (single-camera) stop-and-wait fallback mode | ⬜ Pending — not implemented |
| 14 | Windows port: `LoadLibrary`-based `qr_encoder.cpp`, `CAP_DSHOW` camera backend, `CMakeLists.txt` | ✅ **Build-verified** via MinGW-w64 cross-compilation (see session below) — still not run-tested on real Windows hardware |
| 15 | Cooperative cancellation (`ProtocolConfig::cancel`) so a UI "Stop" button can interrupt a transfer safely | ✅ Done — tested via `make test`'s `test_cancellation` |
| 16 | Native Win32 GUI (`gui_win32.cpp` → `qrtp_gui.exe`): mode switch, file/folder picker, Avvia/Ferma, live log | ✅ **Build-verified** via MinGW cross-compile (found and fixed 2 real bugs — see session below) — still not run-tested |
| 17 | Installer (`windows/installer.iss`, Inno Setup) | ✅ Written, ⬜ **not tested** — needs Inno Setup Compiler (Windows-only) to try; not reproducible via cross-compilation |
| 18 | MinGW cross-compile toolchain (`cmake/toolchain-mingw64.cmake`) so Windows .exes can be built from this Linux machine without a Windows box | ✅ Done — produces real, working (compile+link verified) qrtp.exe/qrtp_tests.exe/qrtp_gui.exe |

## How to Use
```bash
make            # builds bin/qrtp
make test       # builds and runs the automated test suite

# Hardware-free — prove the protocol end-to-end without a camera:
./bin/qrtp demo myfile.pdf --loss 0.05 --corrupt 0.03 --out ./received/

# Real QR generation (no camera needed to prove this works):
./bin/qrtp render-qr "hello" out.pbm

# Real screen+camera link (requires: sudo dnf install opencv-devel && make WITH_OPENCV=1)
./bin/qrtp receive --out ./received/ --camera 0
./bin/qrtp send myfile.pdf --camera 0
```

## Notes
- Language: implemented in C++17 rather than Python (see decision log below) — CLAUDE.md's
  "Project Purpose"/framework sections still describe the general goal accurately, just not the
  language.
- `libqrencode`'s runtime `.so` is present on this system but not its `-devel` headers; rather than
  requiring a system package install or hand-rolling a from-scratch QR generator (risky to get
  byte-perfect against ISO/IEC 18004's tables without a real decoder on hand to verify against),
  `qr_encoder.cpp` loads it via `dlopen`/`dlsym` against a small hand-declared ABI subset.
- `zbar`/OpenCV aren't installed in this environment, so the real camera-decode path
  (`optical_link.cpp`) is written but not build- or run-tested here — see `not_working.md`.
- Full-duplex operation assumes both machines have a monitor *and* a camera, facing each other, so
  the reverse ACK channel is a real optical link and not out-of-band.

## Design decisions log (2026-07-04)

Initial design pass, decisions made and why:
- **Selective-repeat ARQ over go-back-N**: optical read failures are expected to be isolated
  drops (motion blur on a single frame, a missed camera capture, a glare artifact) rather than a
  hard cutoff — re-sending only the missing byte ranges is much cheaper than re-sending everything
  after the first gap.
- **Dual-simplex full duplex model**: treat the link as two independent TCP-like simplex optical
  channels running in opposite directions (A's screen → B's camera, and B's screen → A's camera),
  so ACKs/SACKs travel as real QR frames on their own reverse channel instead of needing an
  out-of-band side channel.
- **19-byte fixed header (23 with CRC32)** — kept deliberately compact since it's overhead on
  every single QR frame, competing directly with file payload for the same limited symbol
  capacity.
- **`seq` = byte offset, not a frame counter**: realized during implementation that a plain
  incrementing sequence number can't be reassembled correctly once chunk size varies mid-transfer
  (which the adaptive controller does deliberately) — switched to TCP-style byte-offset addressing
  so the receiver always knows exactly where a chunk belongs regardless of its size.
- **Two integrity layers, deliberately redundant**: per-frame CRC32 (cheap, catches anything QR's
  own Reed-Solomon EC let through) plus a whole-file SHA-256 checked end-to-end after reassembly.
- **Loopback channel from the start** (`link.hpp`'s `LoopbackLink` + `demo` CLI command): the
  protocol state machine needed to be testable without two physical machines and a webcam on hand
  for every iteration — this is exactly what caught the two real bugs below.

## Implementation session (2026-07-04, same day) — language switch + two real bugs found and fixed

Built the full C++ implementation (Makefile, `include/qrtp/*.hpp`, `src/*.cpp`, `tests/test_main.cpp`)
per user request to switch the project to C++. Compiled cleanly on the first pass (g++ 15.2,
`-Wall -Wextra`, zero warnings). Two genuine bugs only surfaced once the full protocol was
exercised end-to-end under simulated loss via `make test` / `./bin/qrtp demo` — neither was
visible from reading the code in isolation:

1. **SACK batching starved the sender once the window collapsed.** Original default batched one
   SACK per 4 `DATA` frames (to save reverse-channel traffic on a real optical link, where every
   SACK costs a full displayed QR frame). Once AIMD shrank the window to 1 — which happens
   quickly under any real loss — the sender only ever has one frame in flight, so the receiver's
   "wait for 4 arrivals" condition could never be satisfied by fresh frames alone. Every single
   offset paid one full wasted RTO before an added "SACK immediately on a RETRANSMIT-flagged
   arrival" fallback rescued it — correct, but ~20x slower than necessary. **Fix:** default
   `sack_every_n_frames` to 1 (ack every frame); batching above the sender's window size is never
   safe since the window can shrink to 1 at any time.
2. **An unreachable AIMD recovery threshold pinned the controller at its floor.** Upshifting
   required a rolling 8-round window with `fer_low = 0.01` — effectively demanding a *perfect*
   8-in-a-row clean streak, since even one failure in 8 already gives 0.125 > 0.01. Under the
   test's ~15% effective per-round failure rate (5% loss + 3% corruption, compounding across both
   a `DATA` frame and its ACK), the probability of ever seeing a perfect streak is low enough that
   the controller degraded to window=1/min-chunk/max-EC and, statistically, never came back — a
   20KB transfer ran 90+ seconds without finishing before this was diagnosed. **Fix:** raised
   `fer_low` to `0.15` ("~1 bad round out of 8 is still fine to upshift on"); the identical test
   scenario then recovered (window climbed back to 6, chunk to 68) and completed in well under a
   second.

Also fixed along the way: the Makefile had no header-dependency tracking (`-MMD -MP`), so an
edit to `adaptive.hpp` alone silently failed to trigger a rebuild of anything that included it —
`make` reported "Nothing to be done" after a real fix was applied. Added proper `.d` file
generation and an `-include $(DEPS)` so header-only changes are never silently stale again.

Verified via `make test`'s end-to-end case (20000 random bytes, 5% loss, 3% corruption, 2ms
simulated latency): transfer completes, byte-for-byte identical to the source file, SHA-256
verified by the receiver.

## Windows port (2026-07-04, same day) — per user request to run this on two real Windows machines

The environment this project was built in is a Hyper-V-virtualized Linux VM with no `/dev/video*`
device at all (confirmed: no webcam, no UVC kernel module), so the real optical link could never
be exercised here regardless of OS — the ask was specifically to make the existing protocol
buildable and runnable as a native Windows application for two physical Windows PCs.

Changes:
- **`qr_encoder.cpp`**: added a `#ifdef _WIN32` branch using `LoadLibraryA`/`GetProcAddress`/
  `FreeLibrary` in place of `dlopen`/`dlsym`, tries `qrencode.dll` / `libqrencode.dll` /
  `libqrencode-4.dll` in order. Same design as the Linux path: load whatever's already on the
  machine at runtime, no headers or import lib needed at compile time.
- **`optical_link.cpp`**: opens the camera with `cv::CAP_DSHOW` specifically on Windows — OpenCV's
  auto-selected default backend (Media Foundation) is documented as slower/less reliable to open
  than requesting DirectShow explicitly.
- **`main.cpp`**: switched output-path construction from raw `dir + "/" + filename` string
  concatenation to `std::filesystem::path` joins (works correctly with Windows-style paths).
- **`CMakeLists.txt`** (new): cross-platform build replacing the Unix-only Makefile for Windows use
  — `WITH_OPENCV` option, `find_package(OpenCV)`, `find_package(Threads)`, links `dl` only on
  `UNIX` (Windows needs no extra link library for `LoadLibrary`, it's in kernel32 by default).
- Confirmed via web search that `libqrencode` is a genuine, currently-maintained vcpkg port
  (vcpkg.io/en/package/libqrencode) before writing it into the Windows build instructions — didn't
  want to hand the user an unverified package name.

**Not build- or run-tested**: no Windows machine, no MSVC, and `cmake` itself isn't installed here
(no sudo). Verified only that these changes don't regress the existing Linux build — `make clean &&
make && make test` still passes all checks after every change (the `#ifdef _WIN32` branches are
simply never compiled on this machine, so they could contain a mistake `-Wall -Wextra` here would
never catch). Treat the first Windows build as a real verification step, not a formality — see
`not_working.md`.

## GUI application (2026-07-04, same day) — per user request for something installable and simpler than the CLI

User wanted an actual installable Windows application (with mode selection, file loading, visible
QR codes), not just a command-line tool. Two real additions, both reusing the already-tested
protocol core untouched:

- **Cooperative cancellation** (`ProtocolConfig::cancel`, an optional `std::atomic<bool>*`):
  `Sender`/`Receiver` check it at every loop checkpoint (handshake retry, main data loop, and
  `Sender::wait_for`'s wait itself was changed to poll in 150ms slices instead of blocking for the
  full timeout, so cancellation is noticed quickly even mid-wait, not just between attempts). This
  didn't exist before — a GUI "Stop" button has no safe way to kill a background thread that's
  holding an open camera/window, so a graceful stop request was a real gap, not a nice-to-have.
  **This one is verified**, unlike the rest of this session: `test_cancellation` in
  `tests/test_main.cpp` starts a 200KB transfer, flips the cancel flag after 20ms, and asserts both
  threads stop within 5 seconds and report a clean failure rather than hanging or completing. Ran
  it via `make test` on Linux — passes.
- **`gui_win32.cpp`** (new): a native Win32 GUI — deliberately plain `CreateWindowExW`-built
  controls rather than a GUI framework, since the request was explicitly "as simple/portable as
  possible": zero extra runtime dependency beyond OpenCV, a single `.exe`. Mode radio buttons
  (send/receive), file/folder picker (`GetOpenFileNameW` / `SHBrowseForFolderW`), camera
  index/FPS fields, Avvia/Ferma buttons, and a live log fed from the exact same `ProtocolConfig::log`
  callback the CLI already uses. Runs the transfer on a background `std::thread` and marshals log
  lines/completion back to the UI thread via `PostMessageW` with custom `WM_APP+N` messages
  (Win32 windows are single-thread-affine — never touch a control from the worker thread). The QR
  display / camera preview still happens in OpenCV's own `cv::imshow` window (kept the existing
  `OpticalLink` untouched) rather than trying to embed OpenCV rendering inside the Win32 window,
  which would add real risk for no real benefit.
- **`windows/installer.iss`** (new): an [Inno Setup](https://jrsoftware.org/isinfo.php) script.
  Since `VCPKG_APPLOCAL_DEPS` (default on) already copies every DLL `qrtp_gui.exe` needs into the
  build output folder automatically, that folder is already a complete payload — the script just
  packages it, with a Start Menu/Desktop shortcut. Documented the even-simpler zero-installer path
  too (zip the build output folder, copy it to another PC, run the .exe) since that's the more
  literal reading of "as portable as possible."
- **CMakeLists.txt**: added the `qrtp_gui` target (`WIN32` executable subsystem — no console
  window), gated on `WIN32 AND WITH_OPENCV`, with the MSVC runtime statically linked
  (`MSVC_RUNTIME_LIBRARY`) so the .exe doesn't need the VC++ Redistributable installed separately.

**Not build- or run-tested**, same gap as the rest of the Windows port — this is real, standard
Win32 API usage (`CreateWindowExW`, `PostMessageW`, `GetOpenFileNameW`, `SHBrowseForFolderW`), but
GUI code has more moving parts (control creation, message routing, thread-to-UI marshalling) than
the CLI's `main.cpp` did, so treat the first Windows build/run of `qrtp_gui.exe` as the real test —
see `not_working.md` for what to check first if something's off.

## MinGW cross-compilation (2026-07-05) — closing the "never actually compiled for Windows" gap

User's Windows machine hit friction wanting to build via Visual Studio/vcpkg (understandably — the
ask was "just an installer," not "set up a C++ dev environment first"). Rather than only offering
the vcpkg/MSVC path, checked whether this Linux dev machine could cross-compile real Windows
binaries directly — it can: Fedora ships `mingw64-gcc-c++` and, critically, a prebuilt
`mingw64-opencv` (4.11.0, matching the vcpkg version used elsewhere in the docs). User installed
both plus `cmake` via `sudo dnf install` (three short rounds, since each missing piece was
discovered one at a time: gcc-c++/opencv, then cmake, then a bug fix needed a rebuild).

**libqrencode**: no `mingw64-qrencode` package exists, so pulled Fedora's own `qrencode` source RPM
(`dnf download --source qrencode`) and cross-compiled it directly via its bundled `CMakeLists.txt`
(`-DBUILD_SHARED_LIBS=ON -DWITH_TOOLS=OFF -DWITHOUT_PNG=ON`) — produced a real `libqrencode.dll`
depending on nothing but kernel32/msvcrt/libwinpthread.

**Added `cmake/toolchain-mingw64.cmake`** (new, committed to the repo): points CMake at
`x86_64-w64-mingw32-gcc/g++`, sets `CMAKE_FIND_ROOT_PATH` to the MinGW sysroot, and statically
links libgcc/libstdc++/winpthread so the output .exe needs fewer companion DLLs. Lets anyone
(including future sessions) reproduce this without a Windows machine at all:
```
cmake -B build-win -S . -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DWITH_OPENCV=ON
cmake --build build-win
```

**Two real bugs found and fixed** — exactly the payoff of actually compiling instead of "should
work by inspection":
1. `gui_win32.cpp` didn't `#define UNICODE`/`_UNICODE` before `<windows.h>`, so `IDC_ARROW` (and
   similarly all standard resource-ID macros) resolved to the ANSI (`LPSTR`) macro variant while
   the code called the explicit wide `LoadCursorW` — a type-mismatch compile error. Fixed by
   defining both macros before the `windows.h` include.
2. MinGW's default CRT startup object expects an (ANSI) `WinMain` symbol; linking a `wWinMain`-only
   program without `-municode` fails with "undefined reference to `WinMain`" — MSVC doesn't have
   this quirk (it picks the right startup automatically). Fixed by adding `target_link_options(...
   -municode)` for the GUI target, MinGW-only, in `CMakeLists.txt`.

Also hit and fixed: Fedora's `mingw64-opencv` package ships an `OpenCVConfig.cmake` with a
path-computation bug (one `../` too many walking up from its own location), making
`OpenCV_INCLUDE_DIRS` resolve to a nonexistent path (`//include/opencv4`) — worked around in
`CMakeLists.txt` by detecting the broken path and deriving the correct one from the toolchain's
sysroot instead (guarded so it's a no-op on a normal/non-broken OpenCV install, e.g. vcpkg).

**Result**: `qrtp.exe`, `qrtp_tests.exe`, and `qrtp_gui.exe` are now genuine, verified-compiling and
-linking PE32+ Windows binaries (confirmed via `file` and `objdump -p`; `qrtp_gui.exe` correctly
shows `Subsystem: Windows GUI`, no console window). This is meaningfully stronger evidence than the
previous "should compile by inspection" — an entire class of syntax/link errors is now ruled out.
**Still not run-tested**: no Wine or Windows machine available to actually execute them yet (Wine
install was requested but not completed this session — see `not_working.md`).

**Known issue at the time, since fixed** (see next session below): Fedora's `mingw64-opencv` is
built with every optional module enabled (GDAL, PostgreSQL client, GStreamer, Qt5, curl, poppler,
...), none of which this project uses. The full transitive DLL closure was ~210MB (91 non-system
DLLs) — correct, but heavier than necessary, *and* it turned out to also hide a real ABI bug (see
below).

## Real-world crash + minimal OpenCV rebuild (2026-07-05) — first genuine run on real Windows

User downloaded the v1 release build and ran `qrtp_gui.exe` on actual Windows hardware — the first
real execution of any part of the Windows port outside this dev environment. It crashed
immediately:
```
Impossibile trovare il punto di ingresso della procedura
_ZN7Imf_3_314ChromaticitiesC1ERKN9Imath_3_14Vec2IfEES5_S5_S5_
nella libreria di collegamento dinamico C:\qrtp\libopencv_imgcodecs4110.dll
```

Demangled: `Imf_3_3::Chromaticities::Chromaticities(Imath_3_1::Vec2<float> const&, ...)` — an
OpenEXR/Imath constructor. Root cause: Fedora's `mingw64-opencv` package was built against Imath's
`3_1` ABI-versioned namespace, but the currently-installed `mingw64-imath` provides `3_2` — the two
Fedora sub-packages had drifted out of sync with each other. Not fixable by swapping in a different
DLL (no `3_1`-ABI Imath DLL exists anywhere to copy); this is a genuine upstream packaging
inconsistency, not a mistake in how the dependency set was collected.

**Fix**: stopped relying on Fedora's prebuilt `mingw64-opencv` entirely. Pulled OpenCV 4.11.0's own
source via `dnf download --source mingw64-opencv` and cross-compiled a minimal build directly:
```
cmake -B build-win -S . -DCMAKE_TOOLCHAIN_FILE=.../toolchain-mingw64.cmake \
  -DBUILD_LIST=core,imgproc,imgcodecs,videoio,highgui,objdetect -DBUILD_SHARED_LIBS=ON \
  -DWITH_OPENEXR=OFF -DWITH_GDAL=OFF -DWITH_QT=OFF -DWITH_GSTREAMER=OFF -DWITH_FFMPEG=OFF \
  -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_JASPER=OFF -DWITH_OPENJPEG=OFF \
  -DBUILD_ZLIB=ON -DBUILD_PNG=ON -DBUILD_JPEG=ON
cmake --build build-win -j$(nproc)
cmake --install build-win   # required — a raw build tree has no aggregated opencv2/opencv.hpp,
                             # which OpenCVConfig.cmake's consumers (like this project) expect
```
`WITH_OPENEXR=OFF` directly removes the broken dependency; the rest (`GDAL`/`Qt5`/`GStreamer`/
`FFMPEG`/etc.) were disabled because nothing in `optical_link.cpp` needs them, and they were the
source of the earlier bloat. `highgui` builds against native Win32 UI instead of Qt5 automatically
once `WITH_QT=OFF`. PNG/JPEG/ZLib are statically bundled into the OpenCV DLLs themselves
(`BUILD_*=ON`), so they don't need to ship as separate DLLs at all.

Rebuilt `qrtp.exe`/`qrtp_tests.exe`/`qrtp_gui.exe` against this new OpenCV
(`-DOpenCV_DIR=.../build-win/install/lib/cmake/opencv4`), re-ran the case-insensitive DLL dependency
walk, and `strip`ped debug symbols from every binary (safe — only removes debugging info, not
functionality). **Result: 91 DLLs / 210MB → 13 DLLs / 26MB (10MB zipped)**, and confirmed via
`nm -D` that zero Imath/OpenEXR symbols remain anywhere in the packaged binaries. Uploaded as v2 to
the same GitHub release, replacing the broken build.

**Still unverified**: the user hadn't yet re-tested the v2 build as of this writing. The crash is
provably eliminated at the binary level (no OpenEXR/Imath symbols exist to fail resolving anymore),
but full runtime behavior (camera opens via `CAP_DSHOW`, GUI paints/responds correctly, QR
detection actually decodes) is still pending real confirmation.

## Real-hardware debugging saga, v3 through v12 (2026-07-05, same day) — first confirmed working transfers

A long back-and-forth with the user actually running builds on real Windows hardware (two PCs,
cameras facing each other's screens), each round shipping a fix and getting real log evidence back.
Summarized here since `not_working.md` has the full per-bug detail:

- **v3**: receive-mode camera window froze/crashed — `try_receive()` never pumped the HighGUI
  message loop (`send()` got this for free via its own `waitKey`; `try_receive()` only slept).
- **v4**: camera couldn't recognize *any* QR, ever — no `SetProcessDPIAware()`, so Windows display
  scaling (125%/150%, standard on modern laptops) silently blurred everything drawn, QR included.
  Also rendered the QR bigger/sharper (fills ~90% of screen, integer pixels-per-module).
- **v5**: still zero recognition even after v4 — QRTP frames are dense binary full of embedded zero
  bytes (a `SYN`'s `seq` field alone is 4 zero bytes), and `cv::QRCodeDetector`'s `std::string`
  return isn't trustworthy with embedded NULs. Fixed by Base64-encoding frame bytes before
  rendering (pure printable ASCII, sidesteps the question of decoder binary-safety entirely).
- **v6**: handshake started working (a few retries) but the data phase never did — `Sender` and
  `Receiver` both call `send()` *and* `try_receive()` on the same `OpticalLink`; a single shared
  window meant `try_receive()`'s camera preview overwrote the outgoing QR within ~1ms of it being
  shown. Split into two separate windows (outgoing-QR-only, camera-preview-only). Also dropped the
  default fps 10→3 (100ms dwell was an aggressive assumption for real webcam focus/expose time).
- **v7**: still failing at the AIMD's floor settings — turned out `adaptive_.ec_level()`/`fps()`
  were correctly computed and logged, but never fed back into the actual `OpticalLink` doing the
  rendering (`OpticalLinkConfig` was fixed at construction). Added `ILink::set_fps()`/
  `set_ec_level()`, called before every send in the data phase.
- **v8**: log the *first* real diagnostic build rather than another fix — added `OpticalLinkConfig`
  log channel (QR shown/read, rejected frames, DATA frames accepted) since debugging blind had run
  its course; every fix from here on is backed by real log evidence, not inference.
- **v9**: diagnostics showed 3 of 4 offsets in a real transfer succeeding — the optical channel
  genuinely works. The 4th kept failing because retransmitting a stuck offset always resends its
  *original* (larger) payload, and raising EC level for the same payload size needs *more* QR
  modules (more redundancy = more codewords), which at a fixed on-screen size means *smaller*,
  harder-to-resolve modules — confirmed via logged module_px dropping 11→10→9 as EC climbed M→Q→H.
  Fixed by splitting a stuck offset into pieces matching the current (smaller) adaptive chunk size
  instead of resending it unchanged.
- **v10**: first small-file transfer succeeded end-to-end! Large files appeared to freeze — the
  GUI's log `EDIT` control never had `EM_LIMITTEXT` raised, silently capping at Win32's default
  ~32K characters; the verbose v8 diagnostics hit that fast on a big transfer. Raised the limit,
  added periodic trimming.
- **v11**: a transfer completed (all bytes acked) but the receiver reported a hash mismatch — data
  all arrived, content still wrong. The v9 splitting fix had a race: a stuck offset's first
  sub-piece reuses the original's exact starting byte offset, so a stale/delayed camera capture of
  the pre-split *original* (larger) frame arriving after the smaller replacement silently overwrote
  correct data with no CRC to catch it (the stale frame is itself well-formed, just outdated).
  Fixed by rejecting any DATA frame for an already-confirmed offset outright, and rejecting any
  arrival *larger* than what's already stored at a not-yet-confirmed offset (re-chunking only ever
  shrinks, so a larger latecomer is always stale).
- **v12**: user confirmed full correctness *and* that their camera handles 60fps cleanly — real
  evidence the old fps_max=15 ceiling (a guess) was leaving throughput on the table for large
  files. Raised fps_max to 60, made fps growth on upshift multiplicative (matching chunk_size's
  existing behavior) instead of a flat +1 (was ~57 individual upshift events to reach the old
  ceiling, now ~12), roughly halved how long recovery from an isolated hiccup takes
  (`fer_window` 8→4, `fer_low` 0.15→0.25, same "~1 bad in N" tolerance ratio), and lowered
  `rto_min_ms` 150→50 so a fast/low-latency high-fps channel isn't artificially floored to a
  slower retry timeout than it actually needs.

**Net result**: correctness confirmed end-to-end by the user on real hardware (small and large
files). Large-file *throughput* after the v12 tuning changes is not yet re-confirmed — first tuning
pass based on real evidence (the user's own 60fps test) rather than a guess, but still pending a
real large-file timing report before considering it settled.

- **v13**: user reported throughput after v12 alternates between fast bursts and slowdowns — the
  classic AIMD sawtooth. Root cause: `record_loss_event()`/`downshift()` always applied a flat,
  maximal multiplicative decrease (halve the window, shrink chunk by 1/3, `fps_ *= 0.8`) whenever
  *any* frame timed out, whether it was one straggler out of a large in-flight window or the whole
  window vanishing at once — so every aggressive v12 climb ended in an oversized crash regardless of
  how serious the actual loss was. Fixed by adding a `severity` parameter (0..1, the fraction of the
  in-flight window that timed out this round, computed in `Sender::send_file()` before the
  retransmit loop mutates `inflight`) that scales the pullback proportionally: window shrink ranges
  from none (severity 0) to the old halving (severity 1), chunk-size shrink and fps pullback scale
  linearly with it, and EC-level escalation (which v9 showed can backfire) now only fires when
  severity is at least 0.5, not on every isolated timeout.

**Not yet re-confirmed by the user** — this is a tuning change reasoned from the AIMD sawtooth
pattern described, not from fresh diagnostic logs; `make test` and the full loopback suite still
pass, but real hardware confirmation that the burst/slowdown alternation has actually smoothed out
is pending.

## Optimization pass (2026-07-05, same day) — reverse (SACK) channel never adapted its own fps

User reported the (by-then-correct) transfer as still "a bit slow" and asked for further
optimization. Found one clear architecture gap and one clear artificial ceiling, both by reading
the actual send/receive code paths rather than guessing at new constants:

1. **The receiver's outgoing SACK display never adapted.** `sack_every_n_frames = 1` means every
   single `DATA` frame gets an immediate `SACK` reply — correct and necessary (see the v1 fix
   above), but that SACK is a real displayed QR frame subject to the same `cv::waitKey(1000/fps)`
   dwell as any other. Only `Sender` ever called `link_.set_fps()`/`set_ec_level()`; `Receiver` had
   no `AdaptiveController` at all, so its reverse channel stayed pinned at whatever fixed fps was
   passed on its own CLI/GUI invocation (default 3.0) for the *entire* transfer — no matter how high
   the sender's forward channel got tuned. Since every `DATA` frame requires exactly one SACK round
   trip, the reverse channel's static, conservative rate caps the whole transfer's throughput
   regardless of how well the forward channel performs. This is the more likely dominant bottleneck
   for "everything works correctly but is still slow," now that the v1–v13 correctness bugs are
   fixed.

   **Fix:** `Receiver` now owns its own `AdaptiveController` (`protocol.hpp`/`protocol.cpp`), fed
   from the only loss/clean signal available on that side: the `RETRANSMIT` flag on incoming `DATA`
   (a retransmit means a previous SACK likely didn't arrive in time). `send_sack()` now pushes
   `adaptive_.fps()`/`ec_level()` into the link before every send, so the reverse channel ramps up
   and backs off with the same AIMD logic the forward channel already uses — two independent
   controllers on two independent simplex channels, matching the project's own "dual-simplex full
   duplex" design intent, which previously only actually applied to one direction.

2. **`chunk_size_max` (1500 bytes) was an arbitrary round number, not a computed limit.** Chunk size
   only climbs this high once EC level is already at its most permissive (`L`) — `upshift()` grows
   chunk size first and only steps EC level down once chunk is maxed — so the real ceiling is
   version-40's byte-mode capacity at EC=L (2953 bytes, ISO/IEC 18004) minus the ~33% Base64
   expansion and 23-byte frame overhead: `base64_len(chunk + 23) <= 2953` → `chunk <= ~2191`. Raised
   to 2048 (leaves ~5% margin). This only matters once the channel is already clean enough for AIMD
   to have climbed this far — same self-correcting downshift/offset-splitting logic as before
   protects against it being too large for a given camera/lighting setup, exactly as it already does
   for `fps_max`.

Verified: `make clean && make && make test` — all checks pass, including a full randomized
loopback transfer (byte-for-byte + SHA-256 match) at 5% loss / 3% corruption, observed reaching
`chunk=965`+ mid-transfer without incident. Windows binaries (`qrtp.exe`/`qrtp_tests.exe`/
`qrtp_gui.exe`) rebuilt via the MinGW cross-toolchain, confirmed as valid PE32+ binaries.
**Not yet confirmed on real hardware** — `LoopbackLink` treats `set_fps()`/`set_ec_level()` as
no-ops (no physical display to throttle), so the loopback suite verifies correctness but can't
demonstrate the actual speedup; that requires a real two-PC optical transfer, same as every other
tuning change in this log.

## Logging: file size + elapsed seconds at transfer completion (2026-07-05, same day)

User asked for the log to report, at the end of a transfer, how many bytes moved and how many
seconds it took — useful now that throughput itself (not just correctness) is the thing being
tuned. Added to `protocol.cpp`:

- `Sender::send_file`: starts a clock right after the handshake completes ("ACK sent, connection
  established"), logs `transfer finished: N bytes in S.SSSSSSs` right after teardown succeeds.
- `Receiver::receive_file`: starts a clock right after `SYN` is parsed, logs the same
  size/duration alongside the existing hash-verified/HASH MISMATCH line.

Both start their clock just after the handshake rather than at function entry, so the two sides'
numbers are roughly comparable (excludes handshake-retry variance, which can differ a lot between
the two ends on a real optical link). Verified via `make test` and a real `demo` run (5% loss/3%
corruption): both sender and receiver logged matching sizes and elapsed times
(`50000 bytes in 0.286s`, sender and receiver agreeing to within ~1ms).

## QR window pinned topmost + full-screen sized; camera contrast/saturation boost (2026-07-06)

User asked for two things on the send side: the QR display window should always stay on top and
automatically fill the screen's full resolution without going into OS-level exclusive fullscreen,
and the camera should be tuned (contrast or other properties) to improve QR readability
specifically.

**Window (`optical_link.cpp`, `optical_link.hpp`):** `out_window_name_` previously resized/moved
itself on every `send()`/`flush_send_batch()` call to match whatever size the current QR happened
to render at (~90% of the shorter screen dimension) — so it visibly moved every time the AIMD
controller changed QR version mid-transfer, and had no protection against ending up hidden behind
another window. It's now fixed at `screen_w_ x screen_h_`, positioned at `(0,0)`, for the entire
session, and pinned with `cv::setWindowProperty(..., cv::WND_PROP_TOPMOST, 1)` — confirmed present
in the vendored OpenCV 4.11.0 headers used for the MinGW cross-compile
(`WND_PROP_TOPMOST = 5, "property to toggle normal window being topmost or not"`). Still an
ordinary decorated `WINDOW_NORMAL` window, not `WND_PROP_FULLSCREEN` (exclusive fullscreen hands
display/compositor control away from the window manager and can fight with alt-tab or a second
monitor — explicitly what the user asked to avoid). Added `pad_to_screen()`, a small helper that
centers the rendered QR (or QR grid) onto a same-sized white canvas before `imshow`, so every
frame stays a 1:1 pixel copy — no stretch-blur of the QR's square modules — and the window itself
never needs to resize/move again after construction. Removed `last_window_side_` (the field that
tracked the old per-frame resize logic), now unused.

**Camera contrast (`OpticalLinkConfig::enhance_camera_contrast`, default on):** requests a
deliberately oversized `CAP_PROP_CONTRAST` value (driver clamps to its real max — the same
technique already used for `camera_width`/`camera_height`) and `CAP_PROP_SATURATION = 0` right
after opening the camera. Targets the common real-world failure mode of a washed-out, low-contrast
capture (dim monitor backlight, glare, ambient light) blurring the boundary between a QR module
and its quiet zone; saturation to 0 is unconditionally safe since a QR code is pure black/white, so
color is never useful decoder input, only a possible source of chroma noise ahead of the grayscale
conversion `CaptureThread::run()` already does. Exposed as a per-machine runtime toggle — GUI
checkbox "Migliora contrasto fotocamera" (checked by default) and CLI `--no-contrast-boost` — same
pattern as `request_mjpeg`/`disable_autofocus`/`use_wechat_qrcode`. The "camera opened: ..." log
line now also reports the actually-negotiated contrast/saturation values, same as it already does
for resolution/fourcc.

**Verified**: `make test` (native, all checks pass) and a full MinGW cross-compile rebuild
(`qrtp.exe`/`qrtp_gui.exe`/`qrtp_tests.exe`), `dist-windows.zip` refreshed. **Not yet verified on
real hardware** — same standing caveat as every other change to the optical layer in this log; no
camera/monitor available in this dev environment.

## Software optimization/reliability pass: O(n²) SACK generation, camera busy-loop, canvas reuse (2026-07-06)

User asked to check for GPU offload opportunities first (see `not_working.md`, 2026-07-06 entry —
investigated, not pursued: no accessible hook in `WeChatQRCode`'s public API, and both its models
are too small relative to per-frame call rate for GPU dispatch overhead to plausibly pay off), then
asked for a broader sweep of software-level speed/reliability optimizations. Read through
`protocol.cpp`, `adaptive.cpp`, `framing.cpp`, `link.cpp`, `qr_encoder.cpp`, `base64.cpp`, and
`optical_link.cpp` looking specifically for algorithmic inefficiencies and reliability gaps not
already covered elsewhere in this log. Found five candidates, presented all with an impact/effort
read before touching anything; three were selected to implement:

**1. `Receiver::receive_file`'s `send_sack()` was O(n²) over a transfer's chunk count
(`protocol.cpp`).** `chunks` (the receiver's byte-offset-keyed reassembly buffer) accumulates every
chunk ever received for the whole file and is never pruned — necessary, since the final reassembly
loop needs all of it. But `send_sack()`'s range-building loop scanned this *entire* map from
`begin()` on every single call, skipping only entries at-or-before `cumulative` via an `if` check —
and with the default `sack_every_n_frames=1`, `send_sack()` runs once per DATA frame received. For a
10MB file at a ~1KB average chunk size (~10,000 chunks), that's on the order of tens of millions of
map-entry visits by the end of the transfer; for 100MB, billions — squarely the "large file"
scenario this project has repeatedly hit real bugs and slowness on (see the hash-mismatch entry
above, and the several large-file entries in `not_working.md`). Fix: since `chunks` is a
`std::map` ordered by offset, every entry that could possibly satisfy `off > cumulative` lies
strictly after `chunks.upper_bound(cumulative)` — so the loop now starts there instead of at
`begin()`, visiting only entries actually ahead of the cumulative point (bounded by how many
out-of-order chunks are currently buffered, not by the whole transfer's history). Identical output,
zero behavior change — verified with `make test` and a real 3MB loopback `demo` run (5%
loss/2% corruption, hash verified) at native speed, no camera/hardware needed for this one.

**2. `CaptureThread::run()` could busy-loop indefinitely on camera failure
(`optical_link.cpp`).** The comment on the old `if (!ok) continue;` assumed `cap->read()` itself
paces this loop by blocking until the next frame — true in the common case, but if the camera
disconnects or its driver wedges, some backends return `false` immediately instead of blocking,
which would spin this background thread at ~100% of a CPU core indefinitely with no log or signal
anything was wrong. Fix: a brief `5ms` sleep on failure, plus a one-time log line once 30
consecutive reads have failed (a sustained streak, not a single hiccup) — `CaptureThread` now takes
the same log callback `OpticalLinkConfig::log` already uses elsewhere.

**3. `send()`/`flush_send_batch()` reallocated and fully white-filled a full-screen-sized canvas on
every displayed frame (`optical_link.cpp`)** — a cost introduced by the same-day full-screen-window
change above, since the previous per-QR-sized-window approach never needed a separate padding
canvas at all. Replaced the free `pad_to_screen()` function with a stateful `show_padded()`: the
canvas (`OpticalLink::out_canvas_`) is now a persistent member, allocated once, and only re-filled
white when the centered content's own size actually changes (an AIMD-driven event — a QR version
change — far less frequent than every single frame). Consecutive frames at the same rendered size
now skip the fill entirely and go straight to the one unavoidable `copyTo`.

Two further candidates were surfaced but **not** implemented this pass (lower priority, left for
later if revisited): moving `Frame::payload` into the `Sender`'s `inflight` map entry instead of
copying it (payload isn't read again after `encode_frame()`), and raising the SACK range cap
(`kMaxEntries = 32` in `protocol.cpp`) to match `AdaptiveConfig::window_max` (64), since a fully-grown
window with heavy front-of-window loss could buffer more out-of-order chunks than the cap reports
in one SACK (redundant retransmits, not data loss, in that case).

**Verified**: `make test` (native), a 3MB randomized loopback `demo` transfer (5% loss/2%
corruption, hash verified) exercising #1 directly, and a full MinGW cross-compile rebuild with zero
warnings (`qrtp.exe`/`qrtp_gui.exe`/`qrtp_tests.exe`), `dist-windows.zip` refreshed. **#2 and #3 not
verified on real camera hardware** — same standing caveat as every other optical-layer change in
this log.

## Fixed qr_codes_per_frame showing inconsistent grid sizes (1 vs N) (2026-07-06, same day)

**Symptom**: user report — with `qr_codes_per_frame=2` set on *both* sender and receiver, the
displayed grid sometimes showed 2 QR codes, sometimes just 1, with no obvious pattern.

**Root cause, two distinct paths converging on the same effect** (`optical_link.cpp`):
`try_receive_multi()` called `flush_send_batch()` unconditionally on every single invocation, even
when only 1 QR had been queued so far — the comment justifying this ("a batch must reach the
screen before waiting for a reply, or it stalls forever") is correct, but flushing *immediately*,
every time, meant the actual on-screen grid size came down to "however many happened to queue
before the next unrelated poll," not the requested `qr_codes_per_frame`.

1. **Receiver side (the dominant cause)**: with the default `sack_every_n_frames=1`, `send_sack()`
   is called at most once per loop iteration, and `try_receive()` (which flushed) is called once
   per iteration too — *before* that same iteration's own `send_sack()` call. A second SACK could
   therefore never accumulate in the pending batch before being force-flushed: every "batch" on the
   receiver's own display was really just 1, regardless of `qr_codes_per_frame`.
2. **Sender side**: the window-fill loop's per-round count of *new* sends has no relationship to
   `qr_codes_per_frame` — it's however many window slots happen to be free that round (an ARQ
   dynamic), so it's frequently not a multiple of 2, leaving an odd leftover that got force-flushed
   alone.

**Fix**: `try_receive_multi()` no longer flushes a partial batch on sight. It now tracks the pending
batch's size across polls (`pending_size_at_last_poll_`) and only force-flushes once it's
*genuinely stopped growing* for `kMaxStallPolls` (2) consecutive polls — a batch that's still
actively filling up (the common case: the very next `send_sack()`/window-fill send arrives before
the next poll) is never touched by the counter at all, since it always falls into the "grew since
last poll" branch instead. Bounded specifically so a one-off control frame nothing will ever pair
with (SYN/ACK/FIN) or a genuine end-of-data straggler still can't wait forever — same no-hang
guarantee the old unconditional flush provided, just not on the very first poll after something is
queued. `flush_send_batch()` resets both counters wherever it's called from (either this stall path
or `send_multi()`'s own "batch is full" check), keeping the two in sync regardless of which
triggered it.

**Verified**: no OpenCV/camera needed for the core logic, since it's pure integer bookkeeping — a
standalone simulation of the exact call patterns confirmed the fix: the Receiver's one-send-per-
iteration pattern (10 sequential single sends) now consistently produces grids of `2 2 2 2 2`
instead of `1` ten times over; the Sender's variable-odd-count pattern (rounds of 3, 2, 1, 4, 1 new
sends = 11 items total) produces `2 2 2 2 2 1` — consistent pairs plus exactly one genuine final
straggler (11 is odd), with nothing left stuck in the pending batch afterward. Also confirmed `make
test` (native) and a full MinGW cross-compile rebuild with zero warnings.
(`qrtp.exe`/`qrtp_gui.exe`/`qrtp_tests.exe`), `dist-windows.zip` refreshed. **Not yet confirmed
end-to-end on real camera hardware** — same standing caveat as every other change to this
experimental, opt-in path.

## Raised fps_initial 3 -> 10 and lowered chunk_size_max/chunk_size_initial 1024/256 -> 180 (2026-07-06, same day)

Explicit user request: faster starting flash rate, smaller/simpler QR codes. Changed in both
places each constant is tracked (`AdaptiveConfig` in `adaptive.hpp` — the real library/CLI default
— and `OpticalLinkConfig::fps`, the link's own starting value before any `Sender`/`Receiver`
`AdaptiveController` calls `set_fps()`), plus the GUI's own pre-filled "Chunk max" field (now
`180`, matching) and the CLI `--chunk-max` help text. The GUI's own FPS field was **not** changed —
it's already a separate, deliberately-higher GUI-only default (`30`, from 2026-07-05), not the
value of `3` the request specifically named.

Also lowered `chunk_size_initial` from 256 to 180, matching the new `chunk_size_max`, even though
only "chunk max" was requested: `chunk_size_max` alone caps how far `upshift()` can *grow* chunk
size, it doesn't retroactively shrink a starting value already above it — leaving
`chunk_size_initial` at 256 would have meant starting 76 bytes *above* the new ceiling, a confusing
inconsistent initial state. Setting both to 180 means the controller starts already at the (new)
ceiling, can shrink below it under loss same as before, and grows back up to it via `upshift()`.

**Flagged, not silently assumed safe**: raising `fps_initial` to 10 directly re-enters the exact
territory the v6 real-hardware finding (`adaptive.hpp`'s own long-standing comment) found too fast
for a webcam to reliably lock focus/exposure — this change contradicts that once-documented
finding rather than building on new evidence superseding it. Worth revisiting given everything
since v6 that could plausibly change the calculus (camera contrast/saturation tuning, MJPEG,
WeChatQRCode, capture/decode pipelining) — but noted here in case it reproduces the same v6
symptom on real hardware.

**Verified**: `make test` (native), a 3MB randomized loopback `demo` transfer (5% loss/2%
corruption, hash verified — took longer in wall-clock terms than the same transfer at the old
1024-byte chunk size, expected: `LoopbackLink` doesn't model display dwell time/fps at all, so more,
smaller chunks under a fixed per-frame simulated latency is purely more total frames, not evidence
about real hardware throughput either way), and a full MinGW cross-compile rebuild with zero
warnings, `dist-windows.zip` refreshed. **Not yet verified on real hardware** — same standing
caveat as every other change to fps/chunk-size defaults in this log; this is precisely the kind of
change past entries (v6, v9, the 2048 regression) warn needs real hardware to confirm either way.

## Added a one-way (simplex) broadcast mode, coexisting with the bidirectional protocol (2026-07-06, same day)

User asked for an additional transfer mode — a camera reads a QR code for purely one-way data
communication — that coexists with the existing bidirectional (TCP-like) protocol rather than
replacing it. Went through plan mode first given the scope (new protocol layer, CLI, GUI); two
design questions were confirmed with the user before writing code:

1. **Reliability without any return channel**: the sender loops the *entire file* (a header frame
   with filename/size/hash, then every chunk) continuously — forever by default, or a fixed number
   of times — instead of any fixed-repeat-per-chunk scheme. The receiver assembles whatever it
   manages to capture across one or more of those loops and finishes the instant it has the
   complete file with a verified SHA-256 hash, without waiting for the sender's current loop to end.
   Chosen over fixed-repeat-per-chunk: no per-chunk redundancy count to guess at, and naturally
   robust to any loss pattern (a chunk missed in one loop just needs to survive in a later one).
2. **Hardware**: deliberately *not* made hardware-optional — `OpticalLink` is completely untouched;
   both machines still open a camera and a display window exactly as today. The new mode only
   differs in never using the reverse channel, not in what hardware it opens.

**New protocol layer** (`include/qrtp/broadcast.hpp` + `src/broadcast.cpp`), much simpler than
`protocol.cpp` since there's no ARQ/window/re-chunking at all:
- `BroadcastConfig`: fixed (no AIMD — there's no feedback to adapt on) `chunk_size=180`,
  `ec_level=ECLevel::Q` (higher than the bidirectional default M — a lost/corrupted read here isn't
  retried until the next loop, so extra per-QR redundancy directly improves each read's odds),
  `fps=10.0`, `loops=0` (0 = forever).
- `BroadcastSender::send_file()`: reads the file + SHA-256, sets fps/ec_level on the link once, then
  loops sending a header frame (`FrameType::SYN`) plus one `FrameType::DATA` frame per fixed-size
  chunk, checking cancellation between frames, never waiting for any reply.
- `BroadcastReceiver::receive_file()`: polls `try_receive()`, captures metadata from the first
  header seen, stores `chunks[offset]=payload` (a plain overwrite — repeats across loops are always
  byte-identical, so none of the bidirectional Receiver's "smaller-or-equal-size wins" staleness
  logic is needed), and returns success the moment the number of distinct offsets seen matches the
  expected chunk count *and* the reassembled SHA-256 matches — not waiting for the sender's loop.
- `pack_syn`/`parse_syn` (previously private to `protocol.cpp`) moved to `framing.hpp`/`framing.cpp`
  as public `pack_file_metadata`/`parse_file_metadata`, alongside `TransferMetadata` (also relocated
  from `protocol.hpp`), so both the bidirectional and broadcast protocols share identical
  filename/size/hash framing without duplicating it. No changes to `Frame`, `encode_frame`, or
  `decode_frame` — the wire format itself is untouched.
- Reuses `ILink` (`OpticalLink`/`LoopbackLink`) completely unchanged, per the hardware decision above.

**CLI** (`src/main.cpp`): two new subcommands, `qrtp broadcast <file> [--chunk-size N] [--ec-level
L|M|Q|H] [--loops N] [...]` and `qrtp listen --out <dir> [--timeout SECONDS] [...]`, sharing the
same camera-flag parsing as `send`/`receive` via a new `parse_camera_flag()` helper (previously
duplicated verbatim between the two).

**GUI** (`src/gui_win32.cpp`): a new "Modalità unidirezionale (broadcast)" checkbox (unchecked by
default) plus a "Loop (0=infinito)" field; `RunTransfer` branches to
`BroadcastSender`/`BroadcastReceiver` instead of `Sender`/`Receiver` when checked, reusing the
existing "Chunk max" field as broadcast mode's fixed chunk size and the FPS field as its fixed fps.
Window grown from 501 to 547px tall to fit the new controls.

**Verified**: no OpenCV/camera needed for the actual protocol logic — a new `test_broadcast_transfer`
(`tests/test_main.cpp`) sends a 5000-byte file over `LoopbackLink` with 5% loss/2% corruption and
`loops=5`, confirming exact byte-for-byte reassembly with a verified hash (run 5 times back-to-back
with no flakiness, given the fixed RNG seed). `make test` (native) and a full MinGW cross-compile
rebuild (`qrtp.exe`/`qrtp_gui.exe`/`qrtp_tests.exe`) both pass with zero warnings, `dist-windows.zip`
refreshed. **Not yet verified end-to-end on real hardware** — same standing caveat as every other
change touching the optical layer in this log; a real two-PC broadcast/listen run is the next
confirmation needed, especially around whether the fixed `ec_level=Q`/`chunk_size=180`/`fps=10`
defaults hold up without any ARQ safety net to fall back on.

## GUI reorganized into labeled groups, fixed 3fps for unidirezionale, qr_codes_per_frame incompatibility fixed, app icon added (2026-07-06, same day)

User reported the GUI was getting cluttered after the broadcast-mode checkbox/Loop field were
bolted onto the existing flat layout, and asked for two things: reorganize it so it's clear which
parameters belong to bidirezionale vs unidirezionale, and hard-code unidirezionale's fps to a fixed
3 (down from whatever the shared FPS field held).

**Layout (`src/gui_win32.cpp`)**: replaced the single "Modalità unidirezionale" checkbox with a
proper second radio-button pair ("Bidirezionale (TCP-like)" / "Unidirezionale (broadcast)",
mutually exclusive via its own `WS_GROUP`, independent of the existing Invia/Ricevi role group) and
reorganized every control into five `BS_GROUPBOX`-framed sections: **Ruolo e modalità** (role +
mode, with an inline hint explaining the fps/no-reply difference), **File**, **Fotocamera** (camera
index/resolution/MJPEG/autofocus/WeChatQRCode/contrast — genuinely shared by both modes, since
they're about capture, not protocol), and two side-by-side mode-specific groups —
**Bidirezionale** (FPS iniziale, QR per frame, Chunk max — the AIMD-adapted values) and
**Unidirezionale** (FPS fisso, Chunk, Loop — no AIMD, all fixed for the whole broadcast). A new
`UpdateModeUI()` grays out whichever mode-specific group doesn't apply, called once at creation and
again on every mode-radio click. Window grown from 501 to 660px tall to fit the reorganized layout.

**Fixed 3fps for unidirezionale**: the Unidirezionale group's own FPS field (`IDC_FPS_BROADCAST_EDIT`)
is permanently `WS_DISABLED`, always showing "3" — purely so the fixed value is visible rather than
implicit. The `IDC_START_BTN` handler now sets `fps = 3.0` unconditionally whenever Unidirezionale
is selected, never reading the (disabled) field at all.

**Found and fixed a real incompatibility while separating the mode-specific groups**:
`qr_codes_per_frame` (multi-QR-per-frame batching, `OpticalLinkConfig`) turns out to be fundamentally
incompatible with the broadcast/listen path — `BroadcastSender` never calls `try_receive()`, so a
partial batch that never reaches `qr_codes_per_frame` would sit in `pending_send_batch_` forever,
since nothing on the send-only side ever triggers `try_receive_multi()`'s flush-on-stall logic (see
the 2026-07-06 SACK/multi-QR entry above). Fixed at the source in both places this could be reached:
the GUI now forces `qr_grid = 1` whenever Unidirezionale is selected (its own field lives only in
the Bidirezionale group and is disabled otherwise), and `cmd_broadcast_optical`/`cmd_listen_optical`
(`main.cpp`) now force `ocfg.qr_codes_per_frame = 1` unconditionally, regardless of `--qr-grid` —
documented in the CLI's own `--qr-grid` help text rather than silently ignored.

**App icon**: user asked for "a nice icon" while the layout was being redone. Generated a
256x256 source (indigo rounded-square app-icon background, a classic QR-code finder-pattern motif
in a white inset card, scattered data-module texture, one small coral accent dot) with Python/
Pillow, exported as a multi-resolution `.ico` (16 through 256px) to `windows/icon/qrtp.ico`. Wired
in via a new `windows/qrtp.rc` + `windows/resource.h` (`IDI_APP_ICON`), compiled through MinGW's
`windres` (`CMAKE_RC_COMPILER`, already set by `cmake/toolchain-mingw64.cmake`; `enable_language(RC)`
added to `CMakeLists.txt`), and linked into the `qrtp_gui` target. `gui_win32.cpp`'s `WNDCLASSEXW`
now sets `hIcon`/`hIconSm` via `LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON))`, so it shows in
the title bar, taskbar, and Alt-Tab — `windows/installer.iss`'s shortcuts already point at the .exe
itself, so they pick it up with no changes needed there. Confirmed present via
`objdump -h qrtp_gui.exe` showing a `.rsrc` section sized to match the embedded icon.

**Verified**: `make test` (native), a full clean reconfigure + rebuild of the MinGW cross-compile
from scratch (`rm -rf build-win/* && cmake -B ... && cmake --build ...`) with zero warnings,
`dist-windows.zip` refreshed. Icon legibility checked visually at 16/24/32/48/64/128px (all remain
clearly recognizable as a QR code). **GUI layout/behavior not verified interactively on a real
Windows machine** — same standing caveat as every other GUI change in this log; no Windows display
available in this dev environment to actually click through the reorganized controls.

## Native Linux build/test with real OpenCV, plus a new GTK3 GUI for Linux (2026-07-06, same day)

User asked to close two remaining gaps: `WITH_OPENCV=1` had only ever been verified via MinGW
cross-compile for Windows, never natively on Linux (no `opencv-devel` installed in this dev
environment until now); and the GUI (`gui_win32.cpp`) only exists on Windows — no Linux
equivalent. User installed `opencv-devel`/`gtk3-devel` (plus `xorg-x11-server-Xvfb`, not used in
the end — see Verified below) after being asked; the rest of this entry happened without further
back-and-forth once those were in place.

**Native Linux build (`make WITH_OPENCV=1`)**: worked with **zero code changes** — confirms
`optical_link.cpp`/`qr_encoder.cpp` really were already portable (Windows-specific branches
already behind `#ifdef _WIN32`: `CAP_DSHOW` vs the default capture backend, `LoadLibrary` vs
`dlopen`). Genuinely new information found while checking feasibility, before writing anything:
Fedora's `opencv-devel` **already bundles `opencv_contrib`'s `wechat_qrcode` module**
(`/usr/include/opencv4/opencv2/wechat_qrcode.hpp` + `.so` are part of the stock package) — unlike
the MinGW Windows cross-compile, which needed a custom from-source OpenCV build specifically to
get that module (see the 2026-07-05 MinGW entries). `./bin/qrtp send/receive/broadcast --camera 0`
all correctly attempt to open `/dev/video0` via V4L2/GStreamer and fail with a clear
`OpticalLink: cannot open camera index 0` when there isn't one — the real camera-open code path,
genuinely exercised for the first time on any platform in this project (the Windows side could
only ever be compile/link-verified, never actually *run*, since there's no way to execute a
Windows `.exe` from this Linux dev machine).

**New GTK3 Linux GUI (`src/gui_gtk.cpp`)**: chosen over Qt specifically because `gtk+-3.0` is in
the package repositories of virtually every mainstream Linux distribution (Fedora `gtk3-devel`,
Debian/Ubuntu `libgtk-3-dev`, Arch `gtk3`) without needing a heavier stack on non-KDE systems.
Before writing it, extracted the toolkit-agnostic parts of `gui_win32.cpp`'s `RunTransfer` (build
the right `OpticalLinkConfig`, dispatch to `Sender`/`Receiver` or
`BroadcastSender`/`BroadcastReceiver`) into a new shared `include/qrtp/gui_common.hpp` +
`src/gui_common.cpp` (`qrtp::GuiTransferParams` + `qrtp::RunGuiTransfer`, taking `log`/`done`
callbacks instead of touching any toolkit's widgets directly) — `gui_win32.cpp`'s own `RunTransfer`
is now a thin wrapper around it, and `gui_gtk.cpp` is the only other caller, so this logic exists
in exactly one place instead of being copy-pasted into a second GUI from day one.

`gui_gtk.cpp` mirrors `gui_win32.cpp`'s functional layout — role (Invia/Ricevi) and mode
(Bidirezionale/Unidirezionale) radio groups, a File frame, a shared Fotocamera frame, and two
side-by-side mode-specific frames (Bidirezionale: FPS iniziale/QR per frame/Chunk max;
Unidirezionale: FPS fisso — permanently insensitive, always "3" — /Chunk/Loop), with the same
`qr_grid` forced to 1 for Unidirezionale (same `try_receive_multi()` incompatibility noted in the
2026-07-06 GUI-reorganization entry above) — but built with `GtkBox`/`GtkGrid`/`GtkFrame` instead
of fixed pixel coordinates, so the window is naturally resizable rather than fixed-size like the
Win32 one. Threading mirrors the Win32 version too: a `std::thread` worker, marshaling log/done
back to the GTK main thread via `g_idle_add()` (GTK's equivalent of `PostMessageW`/`WM_APP_LOG` —
the only thread-safe way to touch a widget from another thread). Window icon reuses the existing
artwork (`windows/icon/qrtp_preview.png`, copied to `linux/icon/qrtp.png`, loaded via
`gtk_window_set_icon_from_file()` from a path resolved next to the running executable via
`/proc/self/exe` — same "next to the binary" convention `qr_encoder.cpp`'s `LoadLibrary` already
uses — falling back to a repo-root-relative path for development runs). Added
`linux/qrtp.desktop` for standard freedesktop.org application-menu integration, matching the level
of care `windows/installer.iss` already gives the Windows side.

**Build integration**: `src/gui_common.cpp` added to `QRTP_CORE_SOURCES` only under
`WITH_OPENCV`/`WITH_OPENCV=1` (both `CMakeLists.txt` and `Makefile`, same conditional as
`optical_link.cpp`). `qrtp_gui` on Linux is defined via `pkg_check_modules(GTK3 gtk+-3.0)` — **not**
`REQUIRED**, specifically so a CLI-only build stays possible on a machine without GTK3 installed,
printing a one-line diagnostic instead of failing the whole configure step. The `Makefile` gained a
matching optional `bin/qrtp_gui` target (`pkg-config --cflags/--libs gtk+-3.0`), copying
`linux/icon/qrtp.png` next to the built binary as part of the same rule.

**Verified — further than any other GUI change in this project's history**: the user confirmed
**the window actually appeared** on a real Linux desktop session available in this dev environment
(`DISPLAY`/`WAYLAND_DISPLAY` were set) — a first for either GUI in this project; `gui_win32.cpp`
has only ever been compile/link-verified, never actually run interactively. Also confirmed: `make
clean && make WITH_OPENCV=1 && make test` (native Linux, real OpenCV, zero warnings beyond one
pre-existing OpenCV-header-internal `-Woverloaded-virtual` unrelated to this project's own code),
`make bin/qrtp_gui WITH_OPENCV=1` (zero warnings), and the process ran under a real `DISPLAY`
without emitting any GTK-Warning/GTK-Critical messages to stderr. A programmatic screenshot
(attempted via ImageMagick's `import`, then via PyGObject/`Gdk.pixbuf_get_from_window`) wasn't
completed — `import` is blocked by this system's ImageMagick security policy (`coder` rights
restricted to `{GIF,JPEG,PNG,WEBP}`, screen capture not in the allow-list) — superseded by the
user's own direct visual confirmation, which is stronger evidence anyway. Full clean MinGW
cross-compile rebuild also re-confirmed unaffected by the `CMakeLists.txt` changes (the new Linux
block is gated behind `UNIX AND NOT APPLE`, never evaluated true when cross-compiling for
Windows), `dist-windows.zip` refreshed. **Not yet tested with a real camera/optical transfer on
Linux** — this session had no physical camera hardware, same standing limitation as every other
platform this project targets.

## Linux distribution package (`qrtp-linux-x86_64.tar.gz`) (2026-07-06, same day)

User asked for a Linux equivalent of the Windows release asset (`dist-windows.zip`). Three
approaches were weighed given a real constraint discovered along the way: Fedora's `opencv-devel`
pulls in ~50+ `libopencv_*.so` modules via `pkg-config --libs opencv4` (vs. the Windows build's
deliberately minimal 11-module custom OpenCV build — see the 2026-07-05 OpenEXR-crash entry), so a
fully self-contained bundle (AppImage) would be disproportionately large unless OpenCV were
rebuilt from source with a matching minimal `BUILD_LIST`, mirroring the Windows approach.

Started down that path (found the exact minimal module list already proven for Windows —
`core,imgproc,imgcodecs,videoio,highgui,objdetect,dnn,calib3d,features2d,flann,wechat_qrcode` — and
the original OpenCV/opencv_contrib 4.11.0 source tarballs still on disk from that earlier build),
but this dev environment turned out to be genuinely resource-constrained (3.8GB RAM with well
under 1GB free at the time, zram swap already 70%+ used) — a real risk of an OOM-killed build
partway through. Freed ~2.2GB of disk headroom first (user-confirmed, item by item: emptied
`~/.local/share/Trash` at 1.3GB, cleared several user-level caches) but the tighter RAM constraint
remained, so the user decided to defer the from-source rebuild and ship the simpler option instead
for now.

**Shipped**: a plain tarball (`dist-linux/` → `qrtp-linux-x86_64.tar.gz`, 1.2MB compressed) with
just the three stripped binaries (`qrtp`/`qrtp_gui`/`qrtp_tests`, ~260KB each after `strip`),
`models/wechat_qrcode/`, `linux/icon/qrtp.png`, `linux/qrtp.desktop`, and a `README.txt` listing
the one-line install command for `opencv`/`gtk3` on Fedora/Debian-Ubuntu/Arch/openSUSE — relies on
the distribution's own package manager for OpenCV/GTK3 instead of bundling them, which is the
idiomatic Linux distribution model anyway (unlike Windows, where bundling DLLs is the norm since
there's no universal system package manager to lean on). Revisit the from-source minimal-OpenCV +
AppImage path later if a truly zero-prerequisite single-file distributable is wanted — the exact
`BUILD_LIST` to use is already recorded above.

**Verified**: extracted the tarball to a clean directory and ran the three binaries from there
(not from the dev `bin/` tree) — `qrtp_tests` all passing, `qrtp send --camera 0` failing cleanly
with no camera present (same real V4L2 open-path exercised as the native build entry above), and
`qrtp_gui` launched under the real `DISPLAY` available in this environment without errors.

## Per-distribution Linux packages + made WeChatQRCode compile-time optional (2026-07-06, same day)

User asked whether the Windows/Linux builds had actually been checked for backward/cross-version
compatibility — they hadn't, beyond this one Fedora 43 machine. Checking turned up a real, now-
confirmed reason the single generic tarball above can't be "one binary, every distro": `objdump -T`
on `qrtp_gui` showed it requires up to `GLIBC_2.38` (too new for Ubuntu 22.04's 2.35, Debian 12's
2.36 — both would refuse to even start it), and `ldd` showed OpenCV linked via an exact-version
SONAME (`libopencv_core.so.411`) that only Fedora 43 itself actually provides — any other distro's
own `opencv` package resolves to a *different* SONAME the binary can't use, `.pc`-file "install
opencv from your package manager" advice notwithstanding. Decided (after freeing disk space to make
room — see below) to build natively per major distribution instead of chasing one universal
binary, using `podman` (rootless, no daemon needed) to run each target distro's own official
container image and compile inside it — same idea as `cmake/toolchain-mingw64.cmake` cross-
compiling for Windows, just via a container instead of a cross-toolchain.

**Housekeeping first**: freed ~2.2GB of disk headroom before attempting any build (user confirmed
each item individually) — emptied `~/.local/share/Trash` (1.3GB), several user-level app caches
(~/.cache/libdnf5, mozilla, gnome-software, tracker3, vscode-cpptools, pip), and an old project
download no longer needed. Also identified (but explicitly did *not* touch, since it's actively-
used software, not scratch) `/var/lib/flatpak`'s 2.7GB — confirmed via `flatpak list` to be Chrome
and Telegram plus their runtimes, not disposable.

**Real, build-breaking finding while checking the target distros**: `find`/`dpkg -L` initially
suggested Ubuntu 22.04/24.04 and Debian 12 lack opencv_contrib's `wechat_qrcode` module entirely
(unlike Fedora) — `optical_link.cpp`'s unconditional `#include <opencv2/wechat_qrcode.hpp>` would
have been a hard compile error there. Fixed properly rather than working around it: added
`QRTP_HAVE_WECHAT_QRCODE`, detected in `CMakeLists.txt` (via `find_file` for the header in
`OpenCV_INCLUDE_DIRS`) and in the `Makefile` (via `pkg-config --libs opencv4 | grep wechat_qrcode`),
gating every `wechat_qrcode`-touching line in `optical_link.hpp`/`.cpp` behind `#ifdef`.
`OpticalLinkConfig::use_wechat_qrcode`'s default now follows the macro (`true` only when actually
available); forcing it `true` on a build without support throws a clear error at `OpticalLink`
construction instead of failing to compile or silently no-op'ing. **Plot twist**: once actually
building inside the containers, all three distros' `pkg-config`/compiler confirmed
`wechat_qrcode` *is* genuinely available and linkable there too — the earlier `dpkg -L` check was
wrong (checked the wrong signal). The conditional-compile fix stays anyway: it's real, low-risk
portability hardening for distros/versions that genuinely do lack it (older releases, minimal
installs), verified correct in both directions (compiles and behaves identically on Fedora with
the macro defined; the `#else` paths compile and would throw/fall-back correctly if ever hit) —
just not the specific blocker it first looked like on these four.

**Per-distro build process**: for each target (`ubuntu:22.04`, `ubuntu:24.04`, `debian:12`;
Fedora already covered by the native host build), `podman run` with the repo bind-mounted
*read-only* (never risking the live working tree) plus a separate writable output mount, running a
small script that installs `build-essential`/`libopencv-dev`/`libgtk-3-dev`/`libqrencode-dev`,
copies the read-only source to a writable path, then `make clean && make WITH_OPENCV=1 && make
test WITH_OPENCV=1 && make bin/qrtp_gui WITH_OPENCV=1`, `strip`s the three binaries, and copies
them out. Real, if minor, first-attempt mistakes along the way, both now fixed: an inline
multi-line `bash -c "..."` command got its newlines collapsed somewhere in tool-call handling,
silently turning `apt-get install -y ...` into one long malformed command line that hung
indefinitely waiting on stdin (worked around by writing the build steps as a mounted script file
instead of an inline string); and the first attempts didn't install `libqrencode`, so the freshly-
built `qrtp_tests` crashed immediately with a clear, correctly-reported "cannot load libqrencode"
error — not a real portability issue, just an incomplete container setup, fixed by adding
`libqrencode-dev` to the install step.

**Shipped**: four packages instead of one —
`qrtp-linux-x86_64-{fedora,ubuntu2204,ubuntu2404,debian12}.tar.gz` — each built and tested inside
that distro's own environment, so each one's OpenCV/GTK3/glibc dependencies genuinely match what
that distribution's own package manager provides (no more exact-SONAME/glibc-version mismatch).
Superseded the single generic `dist-linux/`/`qrtp-linux-x86_64.tar.gz` from the entry above.

**Verified**: `make test` passed *inside* all four build environments (native Fedora + the three
containers) — genuine functional confirmation per target, not just "compiles by inspection." The
Fedora package was additionally extracted to a clean directory and run standalone outside any
build tree (`qrtp_tests` passing, `qrtp_gui` launching under the real `DISPLAY` available here) —
the other three can't be run at all on this Fedora host (that's the whole point: different
glibc/OpenCV SONAMEs), so their verification is the in-container `make test` pass plus the
successful `qrtp_gui` link. **Still not tested on real camera/screen hardware on any of the four**
— same standing limitation as every other platform this project targets. **Windows compatibility
remains unverified beyond the MinGW toolchain's default PE subsystem version (5.2, a
cross-compile-toolchain default, not a deliberate or tested minimum)** and one historical
crash-then-fixed real-hardware attempt on an unrecorded Windows version — genuinely confirming
Windows backward-compatibility would need real hardware this dev environment doesn't have.
