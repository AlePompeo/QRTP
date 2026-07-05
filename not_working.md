# Known Issues / Debug Log

Living log for real failures — paste actual error output/tracebacks here as they happen, with root
cause and fix once found, so the same failure mode doesn't get re-diagnosed from scratch next
session.

## Resolved

### One specific offset never got through no matter how much EC was raised — raising EC on a fixed-size stuck chunk makes its QR modules smaller, not more robust (2026-07-05)

**Symptom**: with the previous two fixes in place, real diagnostic logs finally showed the optical
channel genuinely working — the receiver's log showed offsets 0, 512, and 256 of a real file all
successfully decoded (`camera read a NEW QR... DATA received: offset=... len=256`). But one specific
offset (768, from the initial 4-frame handshake burst) never got through, cascading through every
EC level up to H and still failing, hard-failing the whole transfer.

**Root cause**: retransmitting a stuck offset always resends its *original* payload bytes (256
bytes, from before any downshift) — confirmed by the diagnostic log's `showing QR` lines: `ec=M` at
version 15 (module_px=11) succeeded for the other three offsets, but climbing to `ec=Q` (version
18, module_px=10) and `ec=H` (version 20, module_px=9) for the stuck offset made its QR *larger and
denser*, not smaller — because more error-correction redundancy for the *same* amount of source
data needs more total codewords, hence more modules, hence smaller modules at our fixed
90%-of-screen display size. Raising EC level was fighting itself: the "safer" setting was optically
harder to resolve than the one that had just failed.

**Fix**: when a stuck offset times out and the AIMD's current `chunk_size()` is smaller than that
offset's original payload, split it into pieces matching the *current* (smaller) chunk size instead
of blindly resending the original unchanged — each piece gets its own fresh retry budget. This
means chunk size actually shrinks for a struggling offset (as intended) before EC level has to climb
much, avoiding the smaller-modules-from-more-redundancy trap for anything that was queued early
(i.e. before any downshift had happened yet), which is exactly the case that broke here.

**Not yet re-confirmed by the user** — but this is the first fix in this whole debugging session
backed by direct evidence (not inference) that the surrounding mechanism works: 3 of 4 offsets in
the same file transfer attempt already succeeded before this fix.

### AIMD ec_level/fps were computed but never actually applied to the optical channel (2026-07-05)

**Symptom**: real log from the user — even after manually forcing 1 fps (a full second of dwell
time per QR) in the GUI, offset 0 of the file *still* never got through, after cascading all the
way to the AIMD's floor (window=1, chunk=32, EC=H) and hard-failing with `max retransmit attempts
exceeded at offset 0`. The user forcing fps themselves and it still failing ruled out dwell time as
the (remaining) bottleneck.

**Root cause**: `adaptive_.ec_level()` and `adaptive_.fps()` were real, correctly-computed AIMD
outputs — logged faithfully every retransmit — but nothing ever fed them back into the actual
`OpticalLink` instance doing the rendering. `OpticalLink::send()` always used
`OpticalLinkConfig::ec_level`/`fps`, fixed at construction time and never updated. So a log line
reading `ec=H fps=1.0` was accurately describing the controller's internal state while the QR being
rendered was still using the original `ec=M` the whole time — the "downshift" was real bookkeeping
that never touched reality. Compounding this: a retransmitted offset always resends the exact bytes
it was first queued with (`InFlight::bytes`, fixed at whatever `chunk_size()` was *when that offset
was first sent*) — so an early offset like 0, first queued before any downshift, keeps resending its
original (larger, denser) chunk forever regardless of how far `chunk_size()` has since dropped for
*new* offsets. Between these two gaps, a stuck offset's retries were, in every way that actually
matters optically, identical to its first failed attempt — no wonder retrying it dozens of times
never helped.

**Fix**: added `ILink::set_fps(double)` / `set_ec_level(ECLevel)` (default no-ops — `LoopbackLink`
has no physical display and ignores them; only `OpticalLink` overrides them meaningfully, updating
its `OpticalLinkConfig` in place). `Sender` now calls both before the handshake and before *every*
`link_.send()` in the data phase — new frames and retransmits alike — so a stuck offset's EC level
and display rate genuinely improve on each retry, even though its payload length itself stays
fixed (re-chunking an already-inflight offset into smaller pieces would be the further fix if EC/fps
alone turns out not to be enough — noted as the next lever, not yet implemented).

**Not yet re-confirmed by the user.**

### Handshake eventually worked, but the data phase always failed (own camera activity stomped the outgoing QR) (2026-07-05)

**Symptom**: real log from the user — `SYN` needed 5 attempts before a `SYN_ACK` came back, then
`ACK sent, connection established` — the handshake *did* eventually succeed. But every single
`DATA` frame afterward failed, with the AIMD controller cascading all the way down to its floor
(window=1, chunk=32, EC=H) and then hard-failing with `max retransmit attempts exceeded`. The
receiver's log showed only the initial handshake message and nothing else — it never got any DATA
frame far enough to respond with a SACK.

**Root cause**: `OpticalLink` used a *single shared window* for both showing the outgoing QR
(`send()`) and previewing the local camera feed (`try_receive()`). Both `Sender` and `Receiver` call
both methods on the same link — e.g. the sender shows a `DATA` frame, then immediately calls
`try_receive()` to poll for a `SACK`, and `try_receive()`'s camera-preview `cv::imshow` overwrites
that same window within about 1ms of the frame being shown, since it polls far more often than
`send()` is called. The outgoing QR was therefore only ever actually visible for its brief initial
`waitKey` duration (a fraction of the already-too-fast 100ms at the old fps=10 default) before being
replaced by the local camera's own feed — leaving almost no real window for the *other* machine's
camera to catch it. A single handshake frame succeeding after 5 tries is consistent with this: pure
luck, catching the brief flash once out of several attempts. The data phase needs many such
round-trips in a row to succeed, and the odds of that never materialized.

**Fix**:
1. Split into two separate `cv::namedWindow`s: one shows only outgoing QR content (`send()`) and is
   never touched by anything else, so it stays visible exactly as displayed until the *next* real
   frame needs to go out — not just for one brief `waitKey` call. The other is purely a local camera
   preview (`try_receive()`), independent of the first.
2. Lowered the default display rate from 10fps to 3fps (100ms → ~333ms dwell per frame) — even with
   the window-stomping bug fixed, 100ms is an aggressive assumption for how fast a real webcam can
   focus, expose, and have the QR decoder lock on. Changed in three places that all needed to agree:
   `AdaptiveConfig::fps_initial`, `OpticalLinkConfig::fps` (the CLI default), and the GUI's FPS
   field's pre-filled value.

**Not yet re-confirmed by the user.** If the data phase still fails after this, the sliding window's
interaction with a single physical display is the next thing to scrutinize: with `window_size() > 1`
(starts at 4), the sender's fill loop shows several *different* frames back-to-back before ever
checking for an ack, so only the last frame in that burst gets to stay on screen indefinitely — the
earlier ones get only one dwell cycle each before going quiet until their individual RTO fires a
retransmit. That's a slower but not fundamentally broken retry cadence given the above fixes, but if
throughput is still very poor, forcing a smaller initial window (`window_initial`) would be the
next lever to try.

### Camera never recognized the displayed QR at all; no ACK flashes ever seen on the receiver (2026-07-05)

**Symptom**: pointing a camera at the sender's displayed QR, `cv::QRCodeDetector` never decoded
anything — and consequently the receiver's screen never showed any outgoing QR flash either (SYN_ACK/
SACK are only sent once a SYN has actually been decoded, so if decode never succeeds, the receiver
never has anything to send back). "QR only ever appears on the transmitter" was a symptom of this,
not a separate bug — the receiver's `send()` calls for ACKs simply never happened because it never
got past waiting for a decodable SYN.

**Most likely root cause**: no `SetProcessDPIAware()` (or manifest-based DPI declaration) anywhere
in the app. On a display with Windows scaling enabled (125%/150% — the default on most modern
laptops), an unaware process's windows get silently bitmap-stretched by the OS compositor, which
blurs everything drawn, including a QR's module edges. A blurred QR is exactly the kind of input a
QR decoder can't parse, regardless of camera distance/focus/lighting — this would explain a
*complete, consistent* failure to decode rather than an intermittent one.

**Also fixed at the same time**: the QR was rendered at a fixed `scale=8` pixels/module regardless
of screen size, producing a fairly small on-screen image — small relative to typical camera-to-
screen distances hurts decode reliability even without the blur issue.

**Fix**:
1. Call `SetProcessDPIAware()` as the very first thing in both `wWinMain` (GUI) and `main` (CLI) —
   before any window is created, per Microsoft's guidance that DPI awareness must be declared
   before window creation to take effect.
2. `OpticalLink::send()` now computes an integer pixels-per-module scale so the rendered image
   fills ~90% of the screen (queried via `GetSystemMetrics(SM_CXSCREEN/CYSCREEN)`), and resizes/
   centers the display window to match exactly — sharp, pixel-exact rendering (no interpolation
   blur from stretching a small bitmap into a big window), large but still a normal decorated
   window (`WINDOW_NORMAL`), not OS-level fullscreen (`WND_PROP_FULLSCREEN`), per the user's
   explicit request.

**UPDATE**: the DPI/size fix alone did *not* resolve it — user confirmed the QR still wasn't
recognized even on the very first SYN attempt, after re-testing. That "fails from the very first
frame, every time" pattern pointed away from a physical/optical explanation (which would tend to be
intermittent) and toward something structurally wrong in the decode pipeline. See the Base64 fix
below, which addresses a much more likely root cause found on closer inspection.

### QR decode failing from the very first frame, every time: binary payload probably truncated at an embedded NUL (2026-07-05)

**Symptom**: even after the DPI-awareness and large/sharp-rendering fix above, the camera still
never recognized the very first QR shown (the sender's `SYN`) — not intermittently, every single
time.

**Root cause (best available diagnosis, not directly observable from this Linux dev environment)**:
QRTP's frames are dense binary data, not text — and are riddled with embedded zero bytes by
construction (e.g. a `SYN` frame's 4-byte `seq` field, always `0x00 0x00 0x00 0x00` since it's
unused for that frame type; `ack_window` for a small window size like 4 is mostly zero bytes too).
`cv::QRCodeDetector::detectAndDecode()` returns a `std::string`, and it is a known, common
limitation of QR/barcode decoder APIs built around C-string-style string handling to silently
truncate the decoded content at the first embedded `0x00` — treating binary data as if it were a
null-terminated C string. For a `SYN` frame specifically, that first zero byte arrives very early
(inside the header, well before the filename/size/hash payload), so the decoded string would come
back drastically truncated — short enough that `decode_frame()`'s size/CRC checks would reject it
outright as malformed, every single time, with no dependence on camera quality, lighting, or
distance. This fits the reported symptom far better than any physical/optical explanation.

**Fix**: stopped trusting any QR/barcode decoder's string handling to be binary-transparent.
`OpticalLink::send()` now Base64-encodes the frame bytes before generating the QR (`base64.cpp`,
new); `OpticalLink::try_receive()` Base64-decodes the detector's returned text after reading it.
Base64 output is pure printable ASCII with no embedded NUL ever, which sidesteps the question of
whether any particular decoder's internals are fully binary-safe — it doesn't matter anymore.
Costs ~33% more QR payload per byte of actual frame data (an acceptable, deliberate trade for
correctness — the alternative, hex encoding, would cost 100% for a marginal simplicity gain).

**Verified**: round-trip correctness of the Base64 implementation itself, thoroughly — RFC 4648's
standard test vectors, an explicit round-trip of binary data containing multiple embedded zero
bytes (the exact scenario hypothesized to break `cv::QRCodeDetector`), and all 65 lengths from 0 to
64 bytes of random data (catches off-by-one padding bugs). All pass (`make test`, native Linux
build). **Not yet verified**: that this actually fixes the real camera-to-screen decode failure —
the hypothesis is well-reasoned and specific (explains "fails from frame one, always," which a
physical/optical cause typically wouldn't), but this project has no way to directly drive a camera
against a real QR-rendering OpenCV window from this dev environment, so real-world confirmation is
still pending. If the camera *still* can't decode after this fix, the failure mode is not what's
diagnosed here — worth checking next: does `render-qr`'s output (a known-good QR, generated the
same way, saved to a file) scan correctly with an unrelated QR reader app (e.g. a phone) at all,
which would isolate whether the QR *generation* itself is sound independent of OpenCV's decoder.

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
