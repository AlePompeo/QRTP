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

## Set an honest, deliberate Windows API baseline (Windows 7) instead of the toolchain's accidental default (2026-07-06, same day)

Follow-up to the compatibility entry above: asked whether Windows could be made backward-compatible
"regardless," given no real Windows machine to test on. Two things checked:

1. **CPU instruction set**: no `-march`/`-mtune` set anywhere — `x86_64-w64-mingw32-g++`'s own
   default target is already the generic `x86-64` baseline (`-mtune=generic`), so hardware
   backward-compatibility was already fine, nothing to change there.
2. **Windows API version baseline**: genuinely inconsistent. `x86_64-w64-mingw32-g++ -E -dM` on
   `<sdkddkver.h>` showed the *unset* default resolves to `_WIN32_WINNT_WS03` (Windows Server
   2003 / XP x64) — but `main.cpp`/`gui_win32.cpp` already call `SetProcessDPIAware()`
   unconditionally, a Vista-only API. This compiled without complaint (mingw-w64's headers don't
   gate declarations behind `_WIN32_WINNT` as strictly as the official Windows SDK does), silently
   relying on that leniency rather than declaring the real requirement — and a binary built this
   way would still *fail to launch at all* on genuinely pre-Vista Windows (a missing import in a
   statically-linked DLL call is a load-time failure, independent of whatever the PE header's own
   declared subsystem version claims — confirmed still showing the old 5.2/XP-x64 default via
   `objdump -p` even with real Vista-only APIs already in use).

**Fixed in `cmake/toolchain-mingw64.cmake`**: set `_WIN32_WINNT`/`WINVER` to `0x0601` (Windows 7)
explicitly — grepped the whole source tree first for any Windows 8/10-only API
(`SetProcessDpiAwarenessContext`, `IFileDialog`, etc.) to confirm nothing already needs *more*
than this baseline; nothing did. Also added `--major-subsystem-version`/`--minor-subsystem-version`/
`--major-os-version`/`--minor-os-version` linker flags (`6`/`1`) so the PE header's own declared
minimum actually matches reality now, instead of the misleading inherited-XP-x64 default —
confirmed via `objdump -p qrtp_gui.exe` now showing `MajorSubsystemVersion 6` / `MinorSubsystemVersion
1`. Windows 7 chosen as the new floor specifically because it's the *honest* minimum given what
the code already does (comfortably past Vista), not a new capability being added — this is a
declared-metadata correctness fix, not a behavior change.

**Also identified, not yet actioned**: `wine`/`wine-core` are available via `dnf` (not installed
in this dev environment) — installing it would let `qrtp.exe`/`qrtp_gui.exe` actually be *executed*
for the first time in this project's entire history (every prior "verified" claim for Windows has
meant "compiles and links via MinGW," never "runs"), the same kind of upgrade in confidence the
per-distro Linux container builds just gave Linux. Recorded as the natural next step if genuine
Windows execution evidence (not just compile evidence) is wanted.

**Verified**: full clean MinGW cross-compile rebuild with the new toolchain flags, zero warnings,
`dist-windows.zip` refreshed. **Still not run on real Windows, or under Wine** — the toolchain-level
fix is a real, verifiable-by-inspection correctness improvement (the PE header now honestly
reflects the code's actual API floor), but doesn't by itself constitute execution evidence.

## Web/browser port (WebAssembly + zxing-cpp) — new platform target, additive only (2026-07-06, same day)

Asked for a plan for a possible web app port; while scoping it, the request changed to actually
building it, reusing the existing C++ as much as possible and touching nothing already working.
The key fact that made this practical: `ILink` (`include/qrtp/link.hpp`) is the *only* boundary
between the protocol logic and physical I/O — `Sender`/`Receiver` (protocol.cpp) and
`BroadcastSender`/`BroadcastReceiver` (broadcast.cpp) never depend on `OpticalLink` directly, the
same reason `LoopbackLink` could validate the whole protocol with no hardware from day one. That
meant `crc32.cpp`/`sha256.cpp`/`base64.cpp`/`framing.cpp`/`adaptive.cpp`/`link.cpp`/`protocol.cpp`/
`broadcast.cpp` compile into the WASM build completely unmodified — only two things needed a
browser-specific replacement, both new files, nothing existing edited:

- **`src/qr_encoder_wasm.cpp`**: same `qr_encode_bytes()` signature as `qr_encoder.hpp` already
  declares, but statically linking `libqrencode` instead of `qr_encoder.cpp`'s `dlopen` (which
  doesn't exist in WASM). CMake picks exactly one of the two `.cpp` files per target, same pattern
  as `gui_win32.cpp` vs `gui_gtk.cpp`.
- **`src/wasm_link.cpp`**: `JsLink : public ILink`, the browser's physical layer. `send()` base64-
  encodes the frame (identical wire format to `optical_link.cpp`'s own send()/try_receive(), same
  reasoning: protocol frames have embedded NUL bytes, base64 sidesteps whether the decoder's
  string handling is binary-safe), renders via a JS callback, then dwells `1000/fps` ms —
  mirroring `OpticalLink::send()`'s `cv::waitKey(1000/fps)`, since the ARQ timing depends on
  send() itself taking about that long. `try_receive()` is the one genuinely new problem: a
  browser's main thread can't block synchronously waiting for a camera frame without freezing the
  UI (and the capture loop that would deliver that very frame). Fixed with **Emscripten Asyncify**
  (`-sASYNCIFY`): `emscripten::val::await()` lets `try_receive()` suspend on a JS Promise and
  resume when it settles, so `Sender::send_file`/`Receiver::receive_file`'s existing blocking
  loops work completely unchanged. `WebSession` (embind class) exposes `runSend`/`runReceive`/
  `runBroadcastSend`/`runBroadcastReceive`/`requestCancel` — thin dispatch to
  `Sender`/`Receiver`/`BroadcastSender`/`BroadcastReceiver`, same role as `gui_common.cpp`'s
  `RunGuiTransfer` for the native GUIs, but `gui_common.cpp` itself is untouched (stays under
  `QRTP_WITH_OPENCV`, irrelevant to this target).

**Decode: zxing-cpp instead of OpenCV.js.** OpenCV.js needs a dedicated Python build script, a
full OpenCV source tree, manual module selection, and realistic 30-60+ minute builds with real
Emscripten-version compatibility risk — explicitly pre-authorized to skip in favor of zxing-cpp if
it looked complex, which it did. zxing-cpp is pure algorithm (no DNN/Caffe dependency like
`wechat_qrcode`), a direct C++ API (`ZXing::ReadBarcode(ImageView, options)` over a raw pixel
buffer), and was already confirmed packaged/mature on this distro (`zxing-cpp-devel` via `dnf`,
2.2.1) even though WASM needs it built from source. `decode_qr_rgba(ptr, width, height)` is
exposed via embind as a free function, fed a pointer JS has already written captured camera pixels
into (`Module._malloc` + `HEAPU8.set`) — avoids embind's per-element marshaling on a
multi-megapixel buffer every polled frame.

**New `web/` directory**: `index.html`/`main.js`/`style.css`, plain JS with no bundler (the
Emscripten module, built with `-sMODULARIZE=1 -sEXPORT_ES6=1`, imports directly as an ES module).
UI mirrors `GuiTransferParams` (role, bidirectional/broadcast mode, camera selection via
`enumerateDevices`, file input for send, Blob download for receive). File I/O needed no native-side
change at all: `Sender::send_file(path)` already just opens a `std::ifstream` at a path — MEMFS
(`-sFORCE_FILESYSTEM=1`) makes that transparent, so JS just does `Module.FS.writeFile('/input_file',
bytes)` from the picked `File` before calling `runSend`. `Receiver`/`BroadcastReceiver` already
return bytes in memory (no filesystem involved on that side either), returned to JS as a `Uint8Array`
for a download `Blob`.

**CMake**: new `option(WITH_WASM ...)` block, entirely additive — a new `qrtp_web` executable
target from a second listing of the same OpenCV-free sources plus the two new files above, built
with `emcmake cmake -B build-wasm -DWITH_WASM=ON && cmake --build build-wasm --target qrtp_web`
(the explicit `--target` matters: `qrtp`/`qrtp_tests`/`qrtp_gui` are still defined under an
Emscripten configure too, since none of that is gated behind `WITH_WASM`). `libqrencode` and
`zxing-cpp` are fetched as source into `third_party/` (gitignored, git-cloned, not committed —
same treatment as the Windows distribution's DLLs) and built once with `emcmake`/`emmake` ahead of
the main configure:
```
git clone https://github.com/emscripten-core/emsdk.git third_party/emsdk
cd third_party/emsdk && ./emsdk install latest && ./emsdk activate latest && source emsdk_env.sh
git clone --depth 1 https://github.com/fukuchi/libqrencode.git third_party/libqrencode
emcmake cmake -B third_party/libqrencode/build-wasm third_party/libqrencode -DWITHOUT_PNG=ON -DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build third_party/libqrencode/build-wasm
git clone --depth 1 https://github.com/zxing-cpp/zxing-cpp.git third_party/zxing-cpp
emcmake cmake -B third_party/zxing-cpp/build-wasm third_party/zxing-cpp -DZXING_READERS=ON -DZXING_WRITERS=OFF -DZXING_C_API=OFF -DZXING_EXAMPLES=OFF -DZXING_UNIT_TESTS=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build third_party/zxing-cpp/build-wasm --target ZXing
```

**Two real bugs found and fixed while getting the round-trip test to pass** (see
`web/roundtrip_test.mjs`, a Node-only hardware-free sanity check — same role `LoopbackLink` plays
for the native tests, two WASM instances standing in for two browser tabs with a fake "currently
displayed frame" channel instead of `getUserMedia`):
1. **Every embind-bound function returns a Promise under Asyncify, even purely synchronous ones**
   like `decodeQrRgba` — not just the ones that actually call `.await()` internally. Missed
   initially (`text = Module.decodeQrRgba(...)` silently held a Promise object, never a string,
   so `try_receive` never saw a decode); fixed by awaiting every call to it, both in `web/main.js`
   and the test harness.
2. **Asyncify allows only one suspended call in flight per WASM instance.** `Module` spends the
   whole transfer suspended inside `runSend()`/`runReceive()` awaiting `waitForFrame` — calling
   `decodeQrRgba` (also Asyncify-wrapped) on that *same* instance from the camera-polling loop
   collided with it. Fixed by giving decode its own dedicated WASM instance
   (`DecodeModule` in `web/main.js`) never shared with the one running the session.
3. **Default Asyncify stack (4KB) was too small** for this call depth (the ARQ loop through
   `std::function` callbacks, `std::string`/`std::vector`, exceptions) — aborted partway through a
   transfer with no useful error (`Aborted(undefined)`). Fixed with `-sASYNCIFY_STACK_SIZE=65536`.

**Verified**: `web/roundtrip_test.mjs` passes both scenarios end-to-end — full bidirectional
handshake/ARQ transfer and full broadcast/simplex transfer, both through the real WASM-compiled
`qr_encode_bytes` (libqrencode) → rasterize → `decode_qr_rgba` (zxing-cpp) round trip, byte-exact
output, hash-verified by the receiver. Native build/tests re-run clean after all changes (`make
clean && make test`, all checks still passing) — nothing existing regressed. **Not yet tested in
an actual browser** (no headless Chromium/puppeteer available in this dev environment; only native
Firefox, no browser-automation tooling) — the Node harness proves the whole WASM/Asyncify/protocol
chain is correct, but real `getUserMedia`/canvas/camera-pointed-at-a-screen behavior remains
unverified, the same standing gap this project has had for every native platform's camera/screen
testing throughout.

### Follow-up review pass, requested before committing (same day) — 4 more real bugs found and fixed

Asked to detail remaining bugs and how they'd be fixed before committing, plus a general "check for
more bugs" pass. Went back through `wasm_link.cpp`/`main.js` deliberately looking for exactly this;
found and fixed four more real issues, none caught by the round-trip test above (it doesn't exercise
a real getUserMedia camera loop, exception-triggering payloads, or a second camera device):

1. **A C++ exception thrown anywhere in the call graph permanently aborted the whole WASM
   instance**, not just the one call — confirmed empirically with a throwaway diagnostic function:
   a second call into the same module after a caught throw failed identically, proving the
   instance was left dead. Root cause: Emscripten disables exception unwinding by default. Fixed
   by compiling and linking with `-fwasm-exceptions` (both `libqrencode`/`zxing-cpp` rebuilt with
   the same flag for consistency — mixing exception-handling models across statically-linked
   objects in one wasm binary is asking for trouble). Verified: after the fix, a second call on
   the same instance following a throw succeeds normally.
2. **The exception, once no longer fatal, still wasn't usable**: it crosses into JS as an opaque
   `WebAssembly.Exception` with no `.message` — embind's usual exception-to-`Error` translation
   doesn't reliably apply under this newer exception-handling mode. Rather than depend on that,
   `WebSession::run_send`/`run_receive`/`run_broadcast_send`/`run_broadcast_receive` now each wrap
   their whole body in try/catch and convert any `std::exception` into an ordinary
   `{success:false, message: e.what()}`-shaped failure result — the same shape `Sender::send_file`
   already uses for "file not found," just one more flavor of "this attempt didn't work." Verified
   with a diagnostic reproducing the exact real shape (throw *after* an Asyncify await, caught by
   an internal try/catch) — message came through correctly, instance stayed healthy for further
   calls afterward.
3. **`web/main.js`'s camera-capture loop mixed two WASM instances' memory**: `Module._malloc`/
   `Module.HEAPU8.set` allocated the pixel buffer, but `DecodeModule.decodeQrRgba(ptr, ...)` read
   it from a *different* instance's separate linear memory — a pointer from one instance is
   meaningless in another's address space. Missed initially because `web/roundtrip_test.mjs`'s own
   `makeSide()` helper (written after this bug was introduced, while fixing the *"only one
   suspended Asyncify call per instance"* issue below) used the decode instance consistently for
   both, so the test never exercised the mismatch. Fixed by using `DecodeModule` for the malloc/
   free/`HEAPU8` calls too, consistently.
4. **A single decode exception could silently kill the whole camera-polling loop forever.**
   `tick()`'s `requestAnimationFrame(tick)` reschedule sits at the very end of the function body;
   an exception escaping the inner `try/finally` uncaught (nothing catches it) would skip past that
   line, and nothing would ever call `requestAnimationFrame` again — no crash, no visible error,
   just complete silent stalling for the rest of the page's life. `ZXing::ImageView`'s constructor
   throwing on a `0`-width/height frame (plausible for a stray tick right as the camera stream
   starts) is a realistic trigger. Fixed by wrapping the whole `tick()` body in try/catch, logging
   and continuing rather than letting an occasional bad frame kill the loop.

**Also fixed, a real (non-crashing) usability bug found in the same pass**: selecting a different
camera from the dropdown after permission had already been granted via the "Richiedi accesso"
button had no effect — `startCamera()`'s `if (mediaStream) return;` guard exited before ever
looking at the requested device id. Fixed by tracking the currently-open device id and tearing
down/restarting the stream when a different one is requested.

**Still open, and why it's deferred rather than "fixed"**: real-browser execution remains
completely unverified (same limitation as above — no browser automation tooling in this
environment). This round found and fixed everything a careful code-level review plus a
protocol-level round-trip test could surface; it cannot substitute for actually pointing a real
camera at a real screen in a real browser, which is the one thing left that only the user's own
hardware can verify.

### Optical zoom control, requested for phone cameras with multiple lenses (same day)

Asked to add camera zoom/multi-lens support like other QR scanner apps have, specifically *not*
relying on digital zoom. `setupZoomControl()` (`web/main.js`) uses the `zoom` MediaTrackConstraint
(part of the Image Capture / extended Media Capture spec) — a real hardware/optical zoom step,
not a canvas crop+scale: keeps the camera's full sensor resolution feeding the decoder instead of
throwing pixels away, and on some phones the OS/browser will switch to a longer physical lens
automatically past a given zoom threshold. Feature-detected via `track.getCapabilities().zoom`
(currently only reliably present in Chromium-based browsers, mainly Android Chrome — Firefox/
Safari don't expose it as of this writing) — the slider is hidden and an explanatory hint shown
instead when unsupported, rather than silently falling back to a digital zoom that would
contradict the whole point of asking for this. Re-run for whichever camera is currently active
(`startCamera()` calls it after each `getUserMedia()`), since different lenses on the same phone
can have different (or no) zoom ranges. Manual lens selection (e.g. explicitly picking an
ultra-wide if the phone/browser lists it as a separate device) already works via the existing
camera dropdown, `enumerateDevices()` — more so now that the camera-switch bug above is fixed.

**Not verified on real hardware** — same standing gap as the rest of the web port; the `zoom`
capability's actual availability/behavior varies enough by phone and browser version that this
needs to be tried on the user's own device to know whether it does anything.

**Published**: committed to `main` (commit `74cc815`), pushed, and released as `qrtp-web.zip`
under the existing rolling release tag (`v0.1.0-windows-preview`, title bumped to v56) —
deliberately binary-only (`index.html`/`main.js`/`style.css`/`qrtp_web.js`/`qrtp_web.wasm` plus a
usage `README.txt`), same convention as the Windows/Linux distributables: no C++ source, no
`third_party/` build toolchain, no dev-only `roundtrip_test.mjs` in the shipped asset — those stay
in the git repo only.

### First real-browser bug report — resizable ArrayBuffer vs. TextDecoder (same day)

The very first real-hardware feedback this web port has ever gotten (Chrome on the user's own
phone, receive side): `Failed to execute 'decode' on 'TextDecoder': The provided ArrayBuffer value
must not be resizable`, repeated once per polled frame. This is the exact gap flagged as
unverified throughout this whole feature (Node's V8 never exhibited it, only a real browser did)
— a genuinely useful confirmation that the "not yet tested in an actual browser" caveat wasn't
just caution for its own sake.

**Root cause**: `-sALLOW_MEMORY_GROWTH=1` makes Emscripten construct a *growable*
`WebAssembly.Memory`, which recent Chrome backs with a genuinely resizable `ArrayBuffer`
(`buffer.resizable === true`, confirmed directly via a small diagnostic). Any embind function
returning `std::string` — `decodeQrRgba()` on every single polled camera frame — needs Emscripten's
internal `UTF8ToString`-style helper to convert the C string to a JS string, which calls
`TextDecoder.decode()` on a view into that buffer. `TextDecoder` explicitly refuses a resizable
`ArrayBuffer` (spec-mandated, not a bug in the browser) — so every decode attempt failed, over and
over, forever, on the receive side specifically (the only place a `std::string` return crosses the
embind boundary from a hot polling loop).

**Fixed**: dropped `-sALLOW_MEMORY_GROWTH=1` in favor of a fixed `-sINITIAL_MEMORY=134217728`
(128MB, generous for what this app actually needs — a QR-code file transfer is inherently a
small/modest-file medium by nature, not something people push gigabytes through). A non-growable
`WebAssembly.Memory` is backed by a plain, non-resizable `ArrayBuffer` everywhere — confirmed
directly (`buffer.resizable === false`, `byteLength === maxByteLength` after the fix). Trade-off:
file transfers are now capped by this fixed ceiling instead of growing arbitrarily, judged an
acceptable, deliberate trade given the medium.

**Verified**: `web/roundtrip_test.mjs` still passes both scenarios after the fix; native
`make clean && make test` still clean. Rebuilt and re-uploaded `qrtp-web.zip` to the same release
(still v56 — a binary refresh, not a new feature, so no version bump). **Still not personally
confirmed by the user on their phone after this specific fix** — the original report is what
surfaced it, but this exact rebuild hasn't yet been tried against the same real hardware that
found it.

### Second real-device bug report — camera dropdown stuck on the first-granted camera (same day)

Reported: selecting a different camera from the dropdown, then re-clicking "Richiedi accesso
camera," left the stream stuck on whichever camera was granted first (front camera on the user's
phone), with no way to switch.

**Root cause, two compounding bugs**:
1. The `camPermBtn` click handler called `startCamera()` with no argument at all, every time --
   `deviceId` was always `undefined`, regardless of whatever the dropdown had selected.
   `startCamera()`'s own "already on this device" early-return guard
   (`deviceId === currentCameraDeviceId`) then compared `undefined === undefined` on every
   subsequent click (since the *first* call had also set `currentCameraDeviceId = undefined`) —
   true every time, so the stream never actually switched, no matter what was selected.
2. `populateCameraList()` rebuilds the whole `<select>` (`innerHTML = ""` + re-append), which
   resets the visible selection to the first option regardless of what's actually streaming --
   even once bug 1 was fixed, the dropdown's on-screen state would still have silently disagreed
   with the live camera feed after a switch.

**Fixed**: the click handler now passes `$("camSelect").value` to `startCamera()` (same pattern
`onStart()` already used), and `populateCameraList()` restores `sel.value = currentCameraDeviceId`
after rebuilding the option list.

**Verified**: JS syntax check only (`node --check`) — this doesn't touch the WASM module, protocol
logic, or anything `web/roundtrip_test.mjs` exercises, so that test wasn't re-run; nothing else
here could meaningfully verify camera-switching behavior short of a real device, which is exactly
what surfaced both bugs in the first place. Rebuilt `qrtp-web.zip` (still v56, binary refresh) and
re-uploaded. **Not yet re-confirmed by the user on their phone.**

## Selectable unidirectional FPS (3/5/7) + on-screen "STOP" button on the QR window (same day)

Two related requests, both extended explicitly to "the whole codebase" (Win32, GTK3, web — not
one platform): the unidirectional/broadcast FPS field was hard-locked at 3.0 everywhere (a
disabled control just displaying the fixed value); asked to make it a real choice among 3/5/7
instead. Separately: the native GUIs' QR-display window (`OpticalLink`'s `out_window_name_`) is
topmost and roughly full-screen (see the 2026-07-06 "QR window pinned topmost" entry above), which
can leave the control panel — and its "Ferma" button — hidden behind it; asked for a button
directly on the QR window itself (top-right), explicitly for both bidirectional and broadcast
modes, both sender and receiver.

**FPS selector**: same pattern across all three GUIs — the always-disabled field becomes a real,
constrained choice (not free text): `gui_win32.cpp`'s `IDC_FPS_BROADCAST_EDIT` from a
`WS_DISABLED EDIT` to a `COMBOBOX CBS_DROPDOWNLIST` (3/5/7, `GetDlgItemTextW` still works
unchanged); `gui_gtk.cpp`'s `fps_broadcast_entry` from an insensitive `GtkEntry` to a
`GtkComboBoxText` (new `ComboText()` helper wraps `gtk_combo_box_text_get_active_text()`'s
`g_free()`-needing return); `web/index.html`'s `broadcastFpsInput` from a disabled `<input
type=number>` to an enabled `<select>`. All three now toggle sensitivity with the mode radio
buttons exactly like the other broadcast-only fields (`IDC_CHUNKSIZE_EDIT`/`chunksize_entry`/
`broadcastChunkInput`), instead of being permanently locked. `BroadcastConfig::fps` itself needed
no change — it was always a plain `double`, the {3,5,7} constraint is purely a GUI choice, not a
library limit. CLI (`--fps` on `qrtp broadcast`/`listen`) untouched: it already accepts any value,
a superset of {3,5,7}, so there was nothing to "unlock" there.

**STOP button**: implemented once, in `optical_link.cpp`/`optical_link.hpp` — the single place
that draws to `out_window_name_`, called by `Sender` (SYN/DATA), `Receiver` (ACK/SACK), and
`BroadcastSender` alike, so this covers bidirectional+broadcast and sender+receiver automatically
with no changes to `protocol.cpp`/`broadcast.cpp`. New `OpticalLinkConfig::cancel` (`std::atomic
<bool>*`, default `nullptr`) reuses the exact cooperative-cancellation flag `ProtocolConfig::cancel`
/`BroadcastConfig::cancel` already check at their own checkpoints — no new signaling path needed.
`gui_common.cpp`'s `RunGuiTransfer()` sets `ocfg.cancel = cancel;` (one line) since it's the only
place that constructs `OpticalLink` for either native GUI. When set, the constructor computes a
fixed button rect (120x36px, 8px margin, top-right corner of the screen) and registers
`cv::setMouseCallback` on `out_window_name_`; `draw_stop_button()` (called after every
`show_padded()`, before `imshow()`, at both call sites — the classic and multi-QR-per-frame paths)
draws a light-gray "STOP" rectangle there every frame; a click inside it sets `cfg_.cancel->store
(true)`. Left undrawn/unwired entirely when `cfg_.cancel` is `nullptr` (e.g. plain CLI usage with
no cancel flag to control) rather than showing a button that would do nothing. Web app untouched
for this part — it already has a "Ferma" button reachable on the same page, no separate always-on-
top OS window to hide behind.

**Known trade-off, flagged not solved**: the button sits in the screen's absolute top-right
corner, which stays in the QR's white padding margin on realistic displays (the content is always
centered with a margin of at least ~5% of the screen's smaller dimension — `target = 0.9 *
min(screen_w_, screen_h_)` in `send()`), but could nick the outer quiet zone (not the finder
pattern itself, which sits further in) on an unusually small/square display. No adaptive
positioning logic added for this — consistent with the rest of the project's preference for
simplicity over handling a not-yet-observed edge case.

**Verified**: `make clean && make WITH_OPENCV=1 && make test` (native Linux, all checks pass),
`make bin/qrtp_gui WITH_OPENCV=1` (GTK3 GUI compiles/links clean), MinGW cross-compile rebuild of
`qrtp_gui.exe` (Windows GUI compiles/links clean — this is the first time `optical_link.cpp` was
recompiled for *both* GUI targets in the same session, since the stop-button code is shared).
`web/roundtrip_test.mjs` re-run clean (unaffected — the WASM core wasn't touched, only
`index.html`/`main.js`). **Not visually verified on either native GUI** — no display/Xvfb in this
environment, same level of verification already accepted for the rest of the native GUI code
throughout this project; the user will need to confirm on real Windows/Linux hardware that the
button is clickable where expected and the FPS selector shows/reads 3/5/7 correctly.

## Fountain code (LT code) replaces plain looping for the broadcast/unidirectional mode (same day)

After researching faster/more-reliable/more-innovative techniques for the protocol (fountain
codes, JAB Code color barcodes, zxing-cpp vs OpenCV benchmarks, Display-Field Communication — see
the sources cited in that discussion), chose to implement the highest value/effort option: replace
`BroadcastSender`/`BroadcastReceiver`'s old scheme (loop the whole file in order, a fixed number of
times) with a real **LT fountain code** (Luby, "LT Codes", FOCS 2002 — the same class of code
DVB-H/3GPP MBMS use for broadcast delivery). Explicitly requested to cover the whole codebase and
come with proper tests.

**Why this is a real fix, not just a theoretical one**: the old scheme suffered the "coupon
collector" problem — a receiver could see the same chunk dozens of times and still be missing the
same last 2-3 specific offsets, needing an entire extra pass to have any chance at exactly those.
A fountain code's sender instead emits a potentially endless stream of *encoded symbols* (XOR
combinations of the file's source blocks); the receiver stops the moment it has *any sufficient
number* of them, never specific ones.

**New module, `include/qrtp/fountain.hpp` + `src/fountain.cpp`** (isolated from `broadcast.cpp` so
the algorithm itself is unit-testable): `lt_degree_for_symbol()`/`lt_indices_for_symbol()` are
deterministic from `(symbol_index, num_blocks)` alone via a seeded PRNG — sender and receiver
independently compute the identical combination for a given symbol without exchanging anything
else. Symbols `0..num_blocks-1` are systematic (degree 1, the raw block itself — a clean channel
behaves exactly like the old scheme, zero overhead); only `symbol_index >= num_blocks` draws a
degree from a **Robust Soliton distribution** (the standard robustness fix over the "ideal"
Soliton distribution, which is optimal in theory but stalls in practice) and picks that many
distinct blocks via partial Fisher-Yates. `LtDecoder` is the matching peeling/belief-propagation
decoder: an iterative worklist (not recursion — a single symbol can cascade-resolve a long chain
for a large file, and recursion depth isn't worth risking), XOR-cancelling resolved blocks out of
every waiting equation and resolving any that drop to degree 1, repeating until nothing more
cascades.

**`broadcast.cpp` changes**: `BroadcastSender::send_file()` now splits the file into K
zero-padded blocks and emits `lt_encode_symbol()` output as DATA frames (`seq` = symbol index, not
a byte offset — a redefinition scoped entirely to broadcast.cpp; the bidirectional protocol's
`Frame::seq` is untouched). `loops` is reinterpreted from "N full passes" to "stop after ~N*K
symbols" — same order-of-magnitude redundancy as before, no config/GUI changes needed anywhere.
`BroadcastReceiver::receive_file()` replaces its offset map/set bookkeeping with an `LtDecoder`.

**Bonus that falls out for free**: DATA frames seen *before* the header arrives are now buffered
(`pending_pre_header`) and fed into the decoder retroactively once the header (and therefore K) is
known, instead of being discarded outright like the old scheme did — directly resolving the
"receiver tunes in mid-broadcast wastes everything seen before the header cycles back" concern
raised earlier in this session, as a natural consequence of the fountain code rather than a
separate fix.

**Whole-codebase coverage confirmed by construction, not assumption**: `BroadcastConfig`'s fields
are unchanged, and `BroadcastSender`/`BroadcastReceiver` are the *only* place any of this logic
lives — CLI, both native GUIs (via `gui_common.cpp`), and the web app (via `wasm_link.cpp`'s
`WebSession::run_broadcast_send`/`run_broadcast_receive`) all call these two classes with zero
knowledge of what's inside, so the new coding scheme applies everywhere automatically. Verified by
actually rebuilding all four targets, not just reasoning about it: native Linux CLI/tests, GTK3 GUI,
MinGW cross-compiled Windows GUI, and WASM (`qrtp_web`) all recompiled clean with no other changes
needed anywhere outside `broadcast.hpp`/`broadcast.cpp`/the new `fountain.hpp`/`fountain.cpp`.

**Tests** (`tests/test_main.cpp`): the pre-existing `test_broadcast_transfer()` (LoopbackLink, 5%
loss/2% corruption) passes unmodified — no test-visible behavior change for the common case. Three
new tests: `test_fountain_code()` (pure algorithmic — encode/decode with simulated 20% symbol loss,
no LoopbackLink/threads, fully deterministic); `test_broadcast_high_loss_reliability()` (30% loss,
only 3x redundancy budget — a scenario the *old* scheme would very likely have failed, since
`K*p^loops ≈ 100*0.3³ ≈ 2.7` specific chunks would be expected to miss every single pass; the
fountain code only needs *some* sufficient count, so it comfortably succeeds in the same budget);
`test_broadcast_late_join()` (hand-crafted frames, not a real `BroadcastSender`, to fully control
timing — sends the first half of the file's blocks *before* any header, the second half after,
proving decode needs *both* halves and therefore that the pre-header buffering actually
contributed the first half rather than the post-header symbols alone being sufficient). All pass,
deterministically (fixed seeds throughout), on the first run with no constant-tuning needed.

**Verified**: `make clean && make WITH_OPENCV=1 && make test` (native Linux, all checks including
the 3 new tests pass), `make bin/qrtp_gui WITH_OPENCV=1` (GTK3), MinGW cross-compile `qrtp_gui.exe`
(Windows) — all clean, zero warnings from the new code. WASM (`qrtp_web`) rebuilt clean;
`web/roundtrip_test.mjs`'s broadcast scenario re-run and still passes — its log now visibly shows
the fountain code's systematic phase resolving one block per symbol (this test file is small
enough, and the simulated channel clean enough, that the non-systematic/Robust-Soliton path never
even triggers — expected and correct, exactly the "clean channel behaves like the old scheme"
property by design).

## Color-multiplex QR — experimental, opt-in, web app only (same day)

Follow-up to the smartphone-mode design discussion: implemented as requested, for sender and
receiver, bidirectional and broadcast alike, gated behind an unchecked-by-default checkbox (never
affects the classic single-QR path when off). Encodes up to 3 independent QR codes into the R, G,
B channels of one color image instead of one QR per displayed frame, rather than inventing a new
barcode symbology (JAB Code) from scratch — reuses the exact same `qr_encode_bytes`/`ReadBarcode`
pipeline already validated for the rest of this port, just applied 3 times per frame.

**Zero changes outside the web target**, exactly as scoped: `protocol.cpp`, `broadcast.cpp`,
`adaptive.hpp`, both native GUIs untouched. This works the same way the existing native spatial
multi-QR-grid mode (`OpticalLink::send_multi()`/`try_receive_multi()`, `qr_codes_per_frame`)
already keeps `Sender`/`Receiver`/`BroadcastSender`/`BroadcastReceiver` unaware that batching is
happening at all — `JsLink` (`src/wasm_link.cpp`) got the identical treatment, batching by color
channel instead of screen position, entirely inside `ILink::send()`/`try_receive()`.

**Decode trick, no buffer copy**: `ZXing::ImageView`'s constructor accepts explicit `rowStride`/
`pixStride` — a new `decode_qr_channels_rgba()` treats an RGBA buffer's R/G/B bytes as 3
independent single-channel (`Lum`) images in place (`data+channel`, `pixStride=4` to skip the
other 3 bytes/pixel, `rowStride=width*4`), no manual pixel extraction loop needed.

**The batching-flush problem, solved once and reused**: `JsLink::send()` accumulates up to 3
encoded QRs before rendering; `try_receive()` flushes whatever's pending (even just 1-2) *before*
waiting for a decoded reply — correct because the caller is about to block on a response to
something it hasn't even shown yet. For `BroadcastSender` (which never calls `try_receive()` at
all): the fountain code's continuous symbol stream almost always fills a batch of 3 on its own;
only the last 1-2 symbols at the very tail of a bounded (`loops>0`) run could go unshown — accepted
as a minor, already-redundant edge case rather than adding teardown-flush machinery.

**Honest, stated (not hidden) limitation**: the real throughput benefit differs by mode. Broadcast
mode's continuous stream almost always has 3 symbols ready together — bidirectional mode's
send()/`try_receive()` interleaving often doesn't (a lone SYN/ACK, or the last DATA frame of a
window), so many bidirectional frames will flush as "1 of 3 channels used." Also stated plainly:
this makes the *opposite* bet from this project's own real-hardware finding that zeroing camera
saturation improves classic QR reliability ("color is noise") — color-multiplex mode assumes color
*carries* information instead, which is exactly why it stays opt-in and unverified rather than a
default.

**Verified**: `web/roundtrip_test.mjs` extended with `makeSideColor()`/`rasterizeQrColor()`/
`runScenarioColor()` (parallel to the existing single-QR helpers, mirroring `web/main.js`'s real
color-mode logic) and two new scenarios — bidirectional and broadcast, both with
`color_multiplex=true` — exercising the full mechanism end-to-end (encode 3 channels → composite
into one RGBA buffer → split back into 3 planes → decode independently → per-channel dedup →
deliver to the unmodified protocol layer) with no real hardware. All 4 scenarios (2 classic + 2
color) pass; the bidirectional color-multiplex run even triggered and correctly recovered from a
real AIMD retransmit mid-transfer, a genuine stress case, not just a happy path. Native test suite
re-run clean (unaffected — this feature is entirely inside `wasm_link.cpp`/`web/`). **Not tested
in an actual browser or with a real camera** — same standing gap as the rest of the web port, more
pronounced here given the deliberately unconventional (color-carries-data) assumption.

## Published: v57 (same day)

Committed (`dabbaa0`) and pushed to `main`, then republished as release v57 (tag
`v0.1.0-windows-preview`, title bumped) with **all six** distribution assets refreshed from a
clean rebuild — not just the ones directly touched — since the fountain code and the on-screen
stop button live in files shared by every platform target:

- `dist-windows.zip` — MinGW cross-compiled `qrtp.exe`/`qrtp_gui.exe`/`qrtp_tests.exe`
- `qrtp-linux-x86_64-fedora.tar.gz` — native build in this dev environment
- `qrtp-linux-x86_64-{ubuntu2204,ubuntu2404,debian12}.tar.gz` — rebuilt in podman containers,
  same technique as the original per-distro verification effort; hit the same SELinux `:Z`
  concurrent-relabel race from that earlier session again when three containers were started at
  once against the same mounted script/source paths — fixed the same way (dedicated script copy
  per container) plus running them sequentially this time instead of in parallel, since the race
  turned out to also affect the shared source-directory mount itself, not just the script file.
  Debian 12 additionally needed `-o Acquire::Check-Date=false` for `apt-get update` (a Release
  file "not valid yet" clock-skew rejection, unrelated to the SELinux issue).
- `qrtp-web.zip` — refreshed WASM build + web assets, still binary-only (no source, no
  `third_party/` toolchain, no dev-only test script)

All four Linux rebuilds' own test suites passed inside their respective containers/native
environment before packaging, not just compiled — same verification bar as every other release
this project has published.

## Investigated a real-hardware bug report: "hash verifies but the received file is corrupted, only on large files" (same day)

First genuine end-to-end usage feedback the web app has gotten (real browser + real camera):
sending an image in broadcast/unidirectional mode, the receiver reported the SHA-256 hash as
verified, yet the downloaded file was unusable — appearing as a generic, unrecognized file, "as if"
corrupted. Reported as size-dependent: never happened on small test files, only large ones (an
image, likely several hundred KB to a few MB — thousands of fountain-code blocks at the default
180-byte chunk size).

**Two separate things were going on, only one of which was an actual bug:**

1. **Real, confirmed bug (now fixed): every downloaded file was literally named `input_file`, no
   extension, regardless of size.** `Sender`/`BroadcastSender::send_file()` derive the transmitted
   filename via `path.filename().string()` (`protocol.cpp`/`broadcast.cpp`, unchanged, correct for
   every native platform, which pass the real picked file's path) — but the web app always writes
   the picked `File` to a *fixed* MEMFS path (`/input_file`) before calling into WASM, discarding
   the real name before it ever reaches that code. A file with no extension won't be recognized as
   an image by the OS/file manager regardless of whether its bytes are correct — this alone fully
   explains "appears as a generic corrupted input file," independent of size. Fixed in
   `web/main.js`: both send paths (`onStart()`'s bidirectional and broadcast branches) now write
   the upload to `"/" + file.name` instead of the fixed path, and pass that same path into
   `runSend`/`runBroadcastSend` — `path.filename().string()` then naturally recovers the real name
   and extension on the wire, no C++ change needed anywhere.

2. **Investigated as a possible fountain-code correctness bug, concluded it is not one.** The
   user's own hypothesis was reasonable on its face: if a specific packet (e.g. symbol/packet 234
   of 700) never arrives, how can the hash still verify? The answer is that this is the fountain
   code working exactly as designed, not a bug: `BroadcastReceiver::receive_file()` only accepts
   the assembled result once `LtDecoder::is_complete()` — meaning every source block has actually
   been resolved, whether directly (its own systematic symbol) or algebraically (XOR-cancelled out
   of a later, higher-degree symbol that also covered other already-resolved blocks). A "missing
   packet" in the everyday byte-offset sense the old (pre-fountain) scheme relied on no longer maps
   onto "missing information" the way it used to. `LtDecoder::assemble()` still throws if any block
   is truly unresolved (i.e. redundancy ran out before recovering it), and that exception is caught
   by `WebSession::run_broadcast_receive` and correctly reported as a *failed* transfer — so a
   genuinely-unrecoverable block cannot silently produce a "successful" download either.
   Re-verified this deterministically: a standalone harness driving the actual compiled
   `fountain.cpp` (not a reimplementation) ran 270 trials — 30 random seeds × loss rates
   {10%, 20%, 30%} × K ∈ {700, 2845, 5000} blocks (700 matching the user's own example; 2845/5000
   matching a several-hundred-KB-to-multi-MB image at the default 180-byte chunk size) — comparing
   every resolved block byte-for-byte against the original once `is_complete()` was true. All 270
   passed with zero mismatches, including the exact "one specific packet dropped, recovered purely
   via peeling" scenario from the bug report. No corruption-that-still-hashes-correctly was ever
   produced. Also re-read `framing.cpp`'s CRC32/frame-length/metadata-packing code end to end
   looking for a silent size-dependent truncation (e.g. a 16-bit field that could wrap for a large
   file) — `seq`/`ack_window` are full 32-bit wire fields, `filesize` is 64-bit, `sha256` is a fixed
   32-byte array compared in full; nothing there is size-dependent either.

**Conclusion**: no algorithmic hash-verification bypass exists in the fountain/broadcast code.
The reported corruption is attributed to bug (1) above (now fixed); the "packet 234 missing yet
verified" observation was the fountain code correctly doing its job, not evidence of a defect.
Native test suite (including the existing `test_fountain_code`/`test_broadcast_high_loss_reliability`
tests) still passes. **Residual caveat, stated honestly rather than swept under the rug**: a
full end-to-end reproduction attempt through the actual WASM/Asyncify/zxing-cpp/base64 stack (not
just the pure-C++ `fountain.cpp` logic) was started via a Node harness modeled on
`web/roundtrip_test.mjs`, simulating a 500KB file with 15% random per-poll loss — it was still
running after 30+ minutes of wall-clock time (WASM/Asyncify per-symbol overhead at this scale is
substantial) and was stopped before completion once the targeted native evidence above was judged
sufficient. If the corruption still reproduces after this fix on real hardware, that would point
to something specific to the real capture/decode pipeline (camera/canvas/zxing-cpp under real
lighting) rather than the fountain/broadcast logic itself — worth a fresh, size-matched real-camera
test rather than re-auditing the algorithm again.

## Selectable unidirectional FPS extended to 10/15/24 (same day)

Extends the earlier 3/5/7 selector (see "Selectable unidirectional FPS (3/5/7)..." above) with
three more choices, same pattern across all three GUIs — `gui_win32.cpp`'s `IDC_FPS_BROADCAST_EDIT`
combobox, `gui_gtk.cpp`'s `fps_broadcast_entry` combobox, and `web/index.html`'s
`#broadcastFpsInput` `<select>` — all still a fixed list (`CBS_DROPDOWNLIST` / no free text),
default still 3. No core/protocol change: `BroadcastConfig::fps` was already an unconstrained
`double`; this is purely a GUI-level choice of which values are easy to pick. Rebuilt and verified:
native Linux GTK3 (`make WITH_OPENCV=1 bin/qrtp_gui`), MinGW Windows cross-compile
(`cmake --build build-win --target qrtp_gui`), WASM web (`cmake --build build-wasm --target
qrtp_web` — no-op here since only `index.html`/no C++ changed for this part), and the native test
suite (`make WITH_OPENCV=1 bin/qrtp_tests && ./bin/qrtp_tests`, all passing, unaffected since this
touches no core files).

## Published: v58 (same day)

Committed (`517198b`) and pushed to `main`, then republished as release v58 (tag
`v0.1.0-windows-preview`, title bumped) with all six distribution assets rebuilt from clean:

- `dist-windows.zip` — MinGW cross-compiled `qrtp.exe`/`qrtp_gui.exe`/`qrtp_tests.exe`
- `qrtp-linux-x86_64-fedora.tar.gz` — native build in this dev environment, `make clean && make
  WITH_OPENCV=1 && make test WITH_OPENCV=1 && make bin/qrtp_gui WITH_OPENCV=1`
- `qrtp-linux-x86_64-{ubuntu2204,ubuntu2404,debian12}.tar.gz` — rebuilt in podman containers, run
  strictly one at a time this time (not just "not launched concurrently" but with *zero* other
  host-side activity touching the repo directory while each container held it `:Z`-mounted) —
  the Ubuntu 24.04 build hit a new variant of the SELinux relabel issue first: it failed with `cp:
  cannot open '/src-ro/build-wasm/qrtp_web.js': Permission denied` because a WASM rebuild was
  running on the host *at the same time*, against the very directory tree the container had
  exclusively relabeled via `:Z`. Confirms the earlier lesson from the v57 release needed
  restating more strongly: `:Z` isn't just unsafe between two containers sharing a mount, it's
  unsafe between a container and *any* other process (including the host shell) touching that
  same directory tree for the container's whole lifetime. Fixed by simply not running anything
  else against the repo until each container finished. Ubuntu 22.04 also hit the already-known
  apt "Release file ... not valid yet" clock-skew rejection on first attempt; reused the
  `-o Acquire::Check-Date=false` fix from the Debian 12 build in the v57 release.
- `qrtp-web.zip` — refreshed WASM build + web assets (the actual fix, `web/main.js`, plus the
  extended FPS `<select>`), still binary-only, README.txt's changelog updated for this version

All four Linux rebuilds' own test suites passed inside their respective containers/native
environment before packaging. `web/roundtrip_test.mjs`'s 4 scenarios (2 classic + 2 experimental
color-multiplex) re-run clean against the rebuilt WASM module.

## JAB Code — second EXPERIMENTAL barcode symbology for the web app, opt-in, additive (same day)

The RGB color-multiplex QR (previous entry) was validated on real hardware and works well.
Asked to look into further ways of raising per-frame data capacity, specifically for the
unidirectional/broadcast mode; picked **JAB Code** (`github.com/jabcode/jabcode`, Fraunhofer SIT)
out of the candidates from that research (JAB Code, HCC2D, Microsoft's old HCCB, COBRA/RDCode
screen-camera-streaming research) as the concrete next experiment — implemented as a **second,
independent opt-in**, explicitly required to leave the existing classic-QR and RGB-color-multiplex
paths completely untouched (web app only, same scope boundary as the color-multiplex feature —
native GUIs/CLI are not involved).

**Why this is a different kind of feature from the color-multiplex QR**: the color-multiplex mode
is 3 independent *monochrome* QR codes overlaid one per color channel — it reuses
libqrencode/zxing-cpp entirely, zero new symbology code. JAB Code is a genuinely different barcode
symbology: each module is directly one of up to 256 colors (not black/white), encoding log2(N)
bits/module with its own LDPC error correction (not Reed-Solomon) — no relationship to QR at all,
requires its own encode/decode library.

**API verified by reading the actual library source** (`jabwriter.c`/`jabreader.c`, the real call
sites, not just the header): `createEncode(color_number, symbol_number)` → `generateJABCode(enc,
data)` fills `enc->bitmap` → `decodeJABCode(bitmap, mode, &status)` returns a `jab_data*` or NULL.
Two properties turned out to make this an unusually clean fit for this project's existing web
architecture:
- `jab_bitmap`'s pixel layout (32bpp, RGBA, 4 channels) is bit-for-bit identical to the canvas
  `ImageData` format already used everywhere in `web/main.js` — encoding hands JS a ready RGBA
  buffer to `putImageData` directly, and decoding builds a `jab_bitmap` from the captured camera
  frame with one `memcpy`, no pixel format conversion in either direction.
  - `jab_data` (`{ int32 length; char data[]; }`) is raw bytes with a length, no text/NUL
  assumption anywhere in the library — unlike the QR paths here, which base64-encode frame bytes
  because zxing-cpp/JS strings aren't safe with embedded NULs (see `wasm_link.cpp`'s file header),
  JAB Code skips base64 entirely, saving its ~33% overhead on top of the color capacity gain.

**A real bug found and fixed during implementation, worth recording**: `generateJABCode` does
*not* follow this library's own `JAB_SUCCESS`(1)/`JAB_FAILURE`(0) convention used elsewhere (e.g.
`saveImage`) — verified by reading `encoder.c`'s actual `generateJABCode` body, not assumed from
the header comment: it returns **0 on success** and a specific non-zero error code per failure
path. The first implementation had this backwards (`if (!status) throw`), which made every encode
attempt fail immediately — caught by the very test written for this feature, fixed by checking
`status != 0` instead. A reminder that for a third-party C library, the header declaration alone
doesn't tell you the success convention — only reading the real implementation (or, as here,
finding it the hard way via a failing test) does.

**Verified**: `third_party/jabcode` fetched as source (MIT-relicensed — confirmed by reading the
actual `LICENSE` file, not assumed from secondary sources, which disagreed on LGPLv2.1 vs LGPLv3),
built for WASM by compiling the 10 needed `.c` files directly with `emcc` (excluding `image.c`,
whose only functions — `saveImage`/`readImage`, PNG/TIFF file I/O — are never called by the
in-memory encode/decode path used here, avoiding the need to also port libpng/libtiff/zlib to
Emscripten):
```
cd third_party/jabcode/src/jabcode && mkdir -p build-wasm && cd build-wasm
for f in encoder decoder detector binarizer interleave ldpc mask pseudo_random transform sample; do
  emcc -c -O2 -std=c11 -fwasm-exceptions -I.. -I../include "../$f.c" -o "$f.o"
done
emar cru libjabcode.a *.o
```
(`-fwasm-exceptions` for consistency with the other statically-linked libraries in this target,
even though jabcode's C code never throws — see CMakeLists.txt's own comment on why this must
match across everything linked into `qrtp_web`.) `jabcode.h` needed wrapping in `extern "C" { ...
}` in `wasm_link.cpp` (it has no `__cplusplus` guard of its own) to avoid a name-mangling
link failure against the plain-C static library. `web/roundtrip_test.mjs` extended with
`makeSideJab()`/`runScenarioJab()` (no rasterizer needed, unlike the QR paths — JAB Code's own
library already produces a ready RGBA bitmap) and two new scenarios (bidirectional + broadcast);
all 6 scenarios (2 classic + 2 color-multiplex + 2 JAB Code) pass. **Not tested in an actual
browser or with a real camera** — same standing gap as the rest of the web port, and a bigger
unknown here than for the color-multiplex mode: an 8-color palette per module is a materially
harder real-world camera discrimination problem than either black/white QR or 3-channel
saturated-or-not color detection, genuinely unverified until tried on real hardware.
