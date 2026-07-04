# Known Issues / Debug Log

Living log for real failures — paste actual error output/tracebacks here as they happen, with root
cause and fix once found, so the same failure mode doesn't get re-diagnosed from scratch next
session.

## Resolved

### Receive mode: the camera window stopped responding and crashed (send worked fine) (2026-07-05)

**Symptom** (real user report, on the v2 build after the OpenEXR fix): starting a transfer in
"Ricevi" (receive) mode opened the camera window as expected, but it then froze — Windows reported
it as not responding, and it crashed. Sending worked correctly with no such issue.

**Root cause**: an OpenCV HighGUI window (`cv::namedWindow`) only stays alive because something
periodically pumps its Win32 message loop — that pump happens *inside* `cv::waitKey()` (and
`cv::imshow()`). `OpticalLink::send()` calls both every frame, so the send-side window was always
fine. `OpticalLink::try_receive()`, however, only ever called `cap->read(frame)` in a loop with a
plain `std::this_thread::sleep_for` — it never once called `cv::waitKey`, so the receive-side
window's message queue was never serviced at all. Exactly the asymmetry the user described: broken
only on the side whose loop doesn't touch HighGUI's event pump.

**Fix**: `try_receive()` now calls `cv::imshow(window_name_, frame)` on every captured frame (a
live camera preview, which doubles as a real usability improvement — it helps the user aim the
camera at the sender's screen) and `cv::waitKey(1)` once per loop iteration, replacing the old
`sleep_for` (waitKey(1) already blocks ~1ms while pumping messages, so no separate sleep is
needed). Rebuilt, re-verified `qrtp_gui.exe` still links and runs the same way, repackaged, and
uploaded as v3 to the GitHub release.

**Not yet re-confirmed by the user** as of this writing — the fix directly addresses the documented
mechanism (Win32 windows need their message loop pumped or the OS marks them unresponsive), but
real confirmation is still pending.

### qrtp_gui.exe crashed on launch: missing OpenEXR entry point (2026-07-05)

**Symptom** (real user report, first actual run of the packaged Windows build):
```
Impossibile trovare il punto di ingresso della procedura
_ZN7Imf_3_314ChromaticitiesC1ERKN9Imath_3_14Vec2IfEES5_S5_S5_
nella libreria di collegamento dinamico C:\qrtp\libopencv_imgcodecs4110.dll
```

**Root cause**: the mangled symbol demangles to
`Imf_3_3::Chromaticities::Chromaticities(Imath_3_1::Vec2<float> const&, ...)` — an OpenEXR/Imath
constructor. `libopencv_imgcodecs4110.dll` (from Fedora's `mingw64-opencv` package) was built
against Imath's `3_1` inline-versioned namespace, but the `libImath-3_2.dll` actually installed on
the build machine (via `mingw64-imath`) uses the `3_2` namespace — an ABI-incompatible version skew
between two independently-updated Fedora mingw sub-packages. Not something fixable by re-copying a
different DLL: no `libImath-3_1.dll` existed anywhere to copy, since Fedora only ships the current
version.

**Fix**: stopped depending on Fedora's prebuilt `mingw64-opencv` package entirely. Downloaded
OpenCV 4.11.0's own source (`dnf download --source mingw64-opencv`) and cross-compiled a minimal
build directly (`-DBUILD_LIST=core,imgproc,imgcodecs,videoio,highgui,objdetect
-DWITH_OPENEXR=OFF -DWITH_GDAL=OFF -DWITH_QT=OFF -DWITH_GSTREAMER=OFF -DWITH_FFMPEG=OFF`, PNG/JPEG/
ZLib built statically-bundled via `-DBUILD_PNG=ON -DBUILD_JPEG=ON -DBUILD_ZLIB=ON`). This removes
the OpenEXR/Imath dependency chain entirely (verified with `nm -D` — zero Imath/OpenEXR symbol
references anywhere in the packaged DLLs) and, as a direct side effect, fixed the earlier-known
bloat problem too: the shipped dependency set went from 91 DLLs / 210MB to 13 DLLs / 106MB, then to
26MB after `strip`ping debug symbols (10MB zipped, down from 70MB).

**Verification**: `find . -iname opencv2/opencv.hpp` on an installed (not just built-in-tree) copy
of the minimal OpenCV was needed to get `find_package(OpenCV)` to resolve include dirs correctly —
a raw, uninstalled CMake build tree doesn't have the aggregated `opencv2/opencv.hpp` header
`OpenCVConfig.cmake` implicitly expects, so `cmake --install` is a required step, not optional, when
pointing `OpenCV_DIR` at a from-source build rather than a system package. Still not run-verified on
Windows after this fix (no Wine/Windows machine this session) — the crash is provably gone
(no Imath/OpenEXR symbols anywhere in the binary or its dependencies), but that's the extent of
what could be confirmed from Linux.

### The protocol appeared to hang forever on any lossy transfer (2026-07-04)

**Symptom:** `./bin/qrtp demo <file> --loss 0.05 --corrupt 0.03` (or `make test`'s end-to-end case)
would run for 90+ seconds without finishing on a 20KB file, continuously logging
`retransmit — window=1 chunk=32 ec=H fps=2.000000` forever, never hitting the 10-retry hard-failure
ceiling and never completing.

**Looked like** a deadlock at first (process alive, near-zero CPU, no forward progress observable
in short observation windows). It was not — verbose per-round instrumentation showed
`cumulative_acked` genuinely advancing, just extremely slowly (~80 bytes/sec).

**Root causes (two, compounding):**
1. `Receiver` batched one SACK per 4 `DATA` frames by default. Once the AIMD controller shrank the
   sender's window to 1 (which happens fast under any real loss), only one frame is ever in flight,
   so the receiver's "4 arrivals" condition could never be met by fresh data alone — every offset
   paid a full wasted retransmission timeout before a `RETRANSMIT`-flag-triggers-immediate-SACK
   fallback rescued it. ~20x throughput hit.
2. Even after fix #1, the AIMD upshift condition (`fer_low = 0.01` over an 8-round rolling window)
   required a statistically near-impossible perfect 8-in-a-row clean streak once the test's
   simulated loss (5%) + corruption (3%) compounded across both a `DATA` frame and its ACK to a
   ~15% real per-round failure rate — so the controller degraded to its floor and never recovered.

**Fix:** default `sack_every_n_frames` to 1 (ack every frame; batching above the current window
size is never safe), and raise `fer_low` to `0.15` (a reachable bar under realistic noise).
Verified: the same 20KB/5%-loss/3%-corruption scenario now completes correctly, with the window
observably recovering from 1 back up to 6 over the course of the transfer.

**Why it took a while to find:** the Makefile had no header-dependency tracking. A fix applied
only to `adaptive.hpp` (the `fer_low` change) silently failed to trigger a rebuild — `make`
reported "Nothing to be done for 'all'" — so the *first* attempt at this fix appeared not to work
at all, when actually it was never compiled in. Fixed by adding `-MMD -MP` and `-include $(DEPS)`
to the Makefile.

## Known limitations / not yet exercised

- **`optical_link.cpp` (the real screen+camera backend) is written but not build-tested.** OpenCV
  isn't installed in this environment (`opencv-devel` unavailable, no sudo/passwordless install
  path). It's gated behind `make WITH_OPENCV=1` and uses standard, stable OpenCV APIs
  (`cv::VideoCapture`, `cv::QRCodeDetector`, `cv::imshow`), but has never actually been compiled or
  run against a real camera. Build and smoke-test this before relying on it.
- **UPDATE 2026-07-05: the Windows port now compiles — verified via MinGW-w64 cross-compilation
  from this same Linux machine** (`cmake/toolchain-mingw64.cmake`), not just "should compile by
  inspection" anymore. `qrtp.exe`, `qrtp_tests.exe`, and `qrtp_gui.exe` all build and link cleanly
  as real PE32+ Windows binaries. Two real bugs surfaced and were fixed this way (see PROGRESS.md's
  "MinGW cross-compilation" session): a missing `#define UNICODE`/`_UNICODE` before `<windows.h>`
  in `gui_win32.cpp`, and a missing `-municode` link flag (MinGW-specific; MSVC doesn't need it).
  **UPDATE 2026-07-05, later same day**: a real user *did* run `qrtp_gui.exe` on actual Windows —
  it launched far enough to start resolving its DLL dependency chain (progress! — the exe itself,
  `LoadLibraryA`, and the basic Win32/GUI plumbing are all fine) before hitting the OpenEXR/Imath
  crash documented above, now fixed. Camera behavior (`CAP_DSHOW`) and full GUI interaction are
  still unconfirmed pending the user trying the fixed v2 build. The MSVC calling-convention
  assumption (`__cdecl` for `QRcodeEncodeDataFn`/`QRcodeFreeFn`) remains unverified against a real
  MSVC-built `qrencode.dll` specifically — the cross-compiled MinGW build used a MinGW-built
  `libqrencode.dll`, a different compiler than a vcpkg/MSVC one, so this doesn't fully retire that
  risk if you're using vcpkg's MSVC-built package instead.
- ~~The MinGW cross-compiled dependency set is bloated (~210MB / 91 DLLs)~~ — **fixed 2026-07-05**,
  see the OpenEXR crash resolution above; now 13 DLLs / 26MB unpacked (10MB zipped), via a
  from-source minimal OpenCV build instead of Fedora's kitchen-sink `mingw64-opencv` package.
- **`gui_win32.cpp` (`qrtp_gui.exe`) compiles cleanly now but has never been *run*.** Its own
  specific risk areas worth checking first if it doesn't behave once it's actually launched:
  - Control layout uses fixed pixel coordinates tuned by eye, not measured against actual Windows
    DPI scaling — on a high-DPI display, text may clip inside the edit/button boxes. If so, the fix
    is `SetProcessDpiAwarenessContext` at startup plus either scaling the layout constants or
    switching to `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` and handling `WM_DPICHANGED`; neither
    is done yet.
  - The `RunTransfer` worker thread constructs and destroys a `qrtp::OpticalLink` (which owns an
    OpenCV `cv::imshow`/`cv::VideoCapture` window and camera handle) entirely on a background
    thread, never the Win32 UI thread. This is a common pattern for OpenCV+native-GUI hybrids but
    hasn't been confirmed to behave cleanly on Windows specifically — if the display/camera window
    misbehaves (doesn't repaint, doesn't close cleanly), that thread ownership is the first thing
    to check.
  - `SHBrowseForFolderW`'s classic tree-view folder picker still works today but is visually dated
    next to `IFileDialog`'s modern picker; purely cosmetic, not a correctness risk, but worth
    knowing it was a deliberate simplicity/risk tradeoff (less code, no COM object lifetime to get
    wrong), not an oversight.
  - The "Ferma" (Stop) button's responsiveness depends on `Sender`/`Receiver` reaching one of their
    cancellation checkpoints — this part *is* verified (see `test_cancellation` in
    `tests/test_main.cpp`, passes on Linux), so if Stop feels unresponsive on Windows the more
    likely cause is something else blocking (e.g. a slow/stuck `cv::VideoCapture::read()` call,
    which isn't itself interrupted by the cancel flag — it's checked between camera reads, not
    during one).
- **No calibration burst.** The design called for a `CALIB` frame exchange at handshake time to
  auto-tune starting FPS/QR density; the frame type is reserved but the burst itself isn't
  implemented. The controller currently starts from fixed defaults and adapts from there, which is
  fine for a loopback/LAN-speed test but untested against real camera-latency variance.
- **No half-duplex fallback.** If only one machine has a working camera, there's currently no
  stop-and-wait mode — both sides need a monitor and a camera today.

## Anticipated failure modes to validate once the real optical link is tested against hardware

- **Camera autofocus hunting** — continuous autofocus refocusing mid-sequence can blur several
  consecutive frames; may need to force fixed focus or detect and back off FPS when this happens.
- **Rolling shutter / display refresh tearing** — a rolling-shutter camera can capture a
  partially-updated LCD frame if capture and refresh aren't loosely synced, corrupting the QR even
  though nothing was "lost." Watch for correlated bursts of CRC failures at a fixed rate.
- **Ambient lighting / glare** — screen reflections or uneven room lighting reduce the effective
  contrast QR decoding needs.
- **Camera/display resolution mismatch** — a QR sized for a 4K display rendered too small for a
  720p webcam to resolve at distance.
- **Frame-boundary de-duplication** — `OpticalLink::try_receive` de-dupes identical consecutive
  reads via a CRC32 of the decoded text; verify this doesn't accidentally swallow a legitimate
  retransmit whose payload happens to be byte-identical to the previous frame (same offset,
  re-sent unchanged — this is actually the common case for a retransmit, so double check the
  de-dup logic doesn't block real retransmits from being re-processed by the protocol layer).
- **USB webcam buffering latency** — some UVC drivers buffer several frames internally, so
  `cap->read()` can return a stale frame; may need to flush the capture buffer before reading if
  RTT estimates look implausibly high.
