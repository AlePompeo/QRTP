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
