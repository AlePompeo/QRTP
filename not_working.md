# Known Issues / Debug Log

Living log for real failures — paste actual error output/tracebacks here as they happen, with root
cause and fix once found, so the same failure mode doesn't get re-diagnosed from scratch next
session.

## Resolved

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
- **The entire Windows port (`CMakeLists.txt`, `qr_encoder.cpp`'s `LoadLibrary` path,
  `optical_link.cpp`'s `CAP_DSHOW` path) has never been compiled.** This dev environment is itself
  a Linux VM with no Windows machine, no MSVC, and no local `cmake` to even dry-run the
  configuration step. The Win32 API usage (`LoadLibraryA`/`GetProcAddress`/`FreeLibrary`,
  `HMODULE`) is standard boilerplate and `libqrencode`'s vcpkg port was confirmed to actually exist
  before being written into the docs, but "should be correct by inspection" is not the same as
  "compiles." **First thing to do on a real Windows machine:** `cmake -B build -S .` (no OpenCV)
  and `qrtp.exe demo somefile.txt` — if that doesn't work, nothing past it will either. Likely
  first failure points if something's off: DLL name mismatch (`qrencode.dll` vs whatever vcpkg
  actually names it — the code tries 3 variants but a 4th real-world name is possible), or an MSVC
  calling-convention mismatch on the `QRcodeEncodeDataFn`/`QRcodeFreeFn` function pointer types
  (assumed `__cdecl`, the default for a portable C library, but not verified against the actual
  compiled DLL).
- **`gui_win32.cpp` (`qrtp_gui.exe`) has never been compiled or run.** Same underlying gap as
  above, plus its own specific risk areas worth checking first if it doesn't build/behave:
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
