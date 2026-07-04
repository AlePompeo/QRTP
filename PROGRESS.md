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
| 14 | Windows port: `LoadLibrary`-based `qr_encoder.cpp`, `CAP_DSHOW` camera backend, `CMakeLists.txt` | ✅ Written, ⬜ **not build-tested** — no Windows machine/CMake available in this environment |
| 15 | Cooperative cancellation (`ProtocolConfig::cancel`) so a UI "Stop" button can interrupt a transfer safely | ✅ Done — tested via `make test`'s `test_cancellation` |
| 16 | Native Win32 GUI (`gui_win32.cpp` → `qrtp_gui.exe`): mode switch, file/folder picker, Avvia/Ferma, live log | ✅ Written, ⬜ **not build-tested** — same Windows-machine gap as step 14 |
| 17 | Installer (`windows/installer.iss`, Inno Setup) | ✅ Written, ⬜ **not tested** — needs a built `qrtp_gui.exe` + Inno Setup Compiler to try |

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
