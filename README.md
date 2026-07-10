# QRTP — QR Transfer Protocol

Send files — and now encrypted text messages — between two computers or phones **using nothing
but a screen and a camera**. No Wi-Fi, no Bluetooth, no cable, no shared network of any kind: one
device flashes a live sequence of QR codes, the other reads them back with its camera. If you can
point a camera at a screen, you can move data across an air gap.

Two things live in this repository:

- **QRTP itself** — a real, TCP-like transfer protocol (handshake, sequence numbers, sliding
  window, retransmission, adaptive rate control) built from scratch on top of QR codes as the
  physical medium, implemented in C++17 with a native CLI/GUI (Linux, Windows) and a browser build
  (WebAssembly).
- **A browser-based, end-to-end encrypted messaging tool** (in `web/`, adapted from the companion
  [qr-messenger](https://github.com/AlePompeo/qr-messenger) project) for the simpler case of just
  swapping a short secret note — no file transfer protocol needed, just AES-256-GCM and a shared
  passphrase.

Both run in any modern browser with no install, and both are useful in the same situations: no
network available, don't trust the network that is available, or you just want two devices that
have never met before to exchange something with a physical, visual guarantee of what's being sent.

## At a glance

- **Air-gapped by construction** — the only path between the two devices is optical; there is no
  socket to intercept, because there is no socket.
- **A real transport protocol, not a slideshow** — 3-way handshake, selective-repeat ARQ,
  cumulative + selective ACKs, per-frame CRC32, end-to-end SHA-256, and an AIMD controller that
  adapts QR density/error-correction/frame rate to the actual optical error rate in real time.
- **Runs everywhere** — native CLI and GUI for Linux and Windows, plus a WebAssembly build that
  runs the identical protocol core in any browser, phone included.
- **Encrypted messaging, same medium** — the web app's "Messaggi cifrati" tab lets two people
  swap short encrypted notes via QR (AES-256-GCM, PBKDF2-derived key) without touching the file
  transfer protocol at all.
- **Experimental higher-throughput symbologies** — optional RGB color-multiplexed QR and JAB Code
  modes in the web app, for up to ~3x the data per frame.

The design mirrors the classic **TCP/IP layering** (Physical → Framing → Transport → Application):

| Layer | Module | Role |
|-------|--------|------|
| Physical (Optical) | `link.hpp`/`link.cpp` (loopback), `optical_link.hpp`/`.cpp` (real, optional) | Drive the display and camera, or simulate the channel for hardware-free testing |
| Framing | `framing.hpp`/`.cpp`, `crc32.hpp`/`.cpp` | Pack/unpack the fixed binary frame header, compute/verify CRC32 |
| Transport | `protocol.hpp`/`.cpp`, `adaptive.hpp`/`.cpp` | Handshake, sliding window, selective-repeat ARQ, retransmission, adaptive rate control |
| Application | `main.cpp` | CLI: `render-qr` / `demo` / `send` / `receive` |

**Status:** the protocol core (framing, CRC32, SHA-256, sliding window + selective-repeat ARQ,
AIMD adaptive control, real QR generation) is implemented, built, and passing an automated test
suite that includes a full end-to-end transfer under simulated loss/corruption. The real
screen+camera backend (`optical_link.cpp`) is implemented but requires OpenCV, which isn't
installed in the environment this was built in — see [Requirements](#requirements).

---

## How it works

Two computers face each other, each with a screen and a webcam:

```
 [Computer A]                              [Computer B]
  screen  ───── forward data channel ─────▶  camera
  camera  ◀──── reverse ACK/control ───────  screen
```

Both directions run the same optical link in reverse, giving a **full-duplex** connection out of
two independent simplex channels — exactly like TCP is full-duplex over two independent IP flows,
except here each flow physically is "one screen facing one camera."

A file is never just "shown as QR codes" — it's segmented into a **sequence of framed, numbered,
checksummed chunks**, sent under a sliding window, acknowledged, retransmitted on loss, and
verified end-to-end. That's what makes it a protocol and not just a slideshow.

---

## Why optical: built for RF-denied and RF-restricted environments

Every other way two nearby devices normally exchange data — Wi-Fi, Bluetooth, NFC, cellular —
depends on a radio transmitting into the air. QRTP doesn't: the only thing that ever crosses the
gap between the two machines is **visible light on a screen**. That one property is what makes it
useful in places where RF-based transfer isn't just inconvenient, it's not allowed or not safe:

- **Communications-denied / contested environments.** In a jammed or actively contested spectrum
  (electronic warfare, hostile signals intelligence), an RF transmitter is a liability twice over:
  it can be jammed into uselessness, and merely keying up gives away your position to anyone doing
  direction-finding. A screen showing QR codes emits nothing a radio receiver can detect — the only
  way to intercept it is to physically see the screen, which is a far easier thing to control
  (line-of-sight, distance, shielding, room layout) than an RF footprint that can travel through
  walls and be picked up from well outside the room.
- **EMCON / emissions-controlled operations.** Some environments mandate radio silence outright —
  ships and aircraft under EMCON, tactical units maintaining low observability. QRTP moves real
  data (files, and now encrypted messages) between two devices without switching on a single radio,
  so it doesn't violate that silence at all, rather than needing an exception carved out for it.
- **Classified or RF-restricted facilities (SCIFs and similar).** Many secure facilities ban RF
  transceivers entirely, precisely because *any* radio — even one that's "supposed" to be
  short-range or encrypted — is a potential exfiltration channel or an attack surface (rogue
  Bluetooth/Wi-Fi pairing, RF side-channel leakage). A laptop and a webcam showing QR codes at each
  other satisfies "no RF in the room" by construction, with no radio to audit, disable, or trust.
- **RF-sensitive or hazardous physical environments.** Hospitals with equipment sensitive to
  interference, aircraft cabins/cockpits, and industrial sites with explosive atmospheres (ATEX/
  IECEx-classified zones, fuel depots, mines) all restrict or ban RF transmitters for safety
  reasons — a spark from a poorly-shielded radio, or interference with life-critical equipment, is
  a real risk a light-based link simply doesn't carry.
- **Air-gapped networks.** The classic reason to air-gap a network is to keep it off the internet
  and off any wireless bridge that could reach it. QRTP is a way to still move data in or out of
  that network without punching an RF (or wired network) hole in the air gap to do it — the
  physical medium enforces the boundary, it isn't just a policy asking people not to plug in a
  cable or turn on hotspot.

None of this requires special hardware: any screen and any camera work, which is exactly why this
also runs as a browser build (see [Web app](#web-app-browser-no-install) below) — a laptop and a
phone are enough.

---

## Requirements

- A C++17 compiler and `make` (developed against GCC 15; no CMake needed)
- The protocol core has **no external library dependencies** — CRC32, SHA-256, and the framing/ARQ
  logic are all hand-implemented against the C++ standard library only
- Real QR generation uses `libqrencode`, loaded at runtime via `dlopen` (works against the plain
  runtime `.so` — no `-devel`/header package required):
  ```bash
  sudo dnf install qrencode-libs   # usually already present; or: apt install libqrencode4
  ```
- The real screen+camera backend (`send`/`receive` commands) additionally needs OpenCV, and is
  compiled in only when explicitly requested:
  ```bash
  sudo dnf install opencv-devel   # or: apt install libopencv-dev
  make WITH_OPENCV=1
  ```
  Without OpenCV, everything else builds and runs (`render-qr`, `demo`, and the full test suite) —
  `send`/`receive` just print a message telling you to rebuild with `WITH_OPENCV=1`.

---

## Build & test

```bash
make            # builds bin/qrtp
make test       # builds and runs bin/qrtp_tests (see tests/test_main.cpp)
```

`make test` covers: CRC32 against the standard check value, SHA-256 against NIST test vectors,
frame encode/decode round-trip (plus rejecting corrupted/truncated frames), AIMD window
grow/shrink behavior, real QR generation via `libqrencode` (checked against the ISO/IEC 18004
version↔size formula), and a full **send+receive transfer over a lossy loopback channel** (5%
frame loss, 3% bit corruption, verified byte-for-byte and by SHA-256 against the original file).

---

## Building on Windows

The protocol core is portable C++17 with no platform-specific code; the two pieces that talk to
the OS (`qr_encoder.cpp`'s dynamic library loading and `optical_link.cpp`'s camera/window code)
both have a Windows branch (`#ifdef _WIN32`) alongside the Linux/macOS one. **This has not been
build- or run-tested on an actual Windows machine** — the code below follows documented,
well-established Windows/OpenCV/vcpkg APIs, but treat the first build on real hardware as a
verification step, not a formality.

Use **CMake** instead of the Makefile (the Makefile is Unix-only):

### 1. Prerequisites

- Visual Studio 2022 (Desktop development with C++ workload) — includes MSVC, CMake, and Ninja
- [vcpkg](https://vcpkg.io) — install once, e.g. `git clone https://github.com/microsoft/vcpkg` then `.\vcpkg\bootstrap-vcpkg.bat`
- For the real screen+camera link, also install:
  ```powershell
  .\vcpkg\vcpkg install opencv libqrencode --triplet x64-windows
  ```
  (`libqrencode` is a genuine vcpkg port, maintained by Microsoft/community — confirmed via
  [vcpkg.io/en/package/libqrencode](https://vcpkg.io/en/package/libqrencode). Only its DLL is
  needed at runtime — `qr_encoder.cpp` loads it dynamically via `LoadLibrary`, the same way it uses
  `dlopen` on Linux, so no compile-time linking or headers from this package are required.)

### 2. Configure & build

```powershell
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE\vcpkg\scripts\buildsystems\vcpkg.cmake" -DWITH_OPENCV=ON
cmake --build build --config Release
```

Drop `-DWITH_OPENCV=ON` (and the `opencv` vcpkg install) if you only want the protocol
core/loopback demo — it needs nothing beyond the compiler.

### 3. Make sure `qrencode.dll` is next to `qrtp.exe`

With the vcpkg toolchain file, `VCPKG_APPLOCAL_DEPS` (on by default) normally copies dependency
DLLs next to the built executable automatically. If `qrtp.exe render-qr` fails to find the library,
manually copy `qrencode.dll` from vcpkg's install tree (e.g.
`vcpkg\installed\x64-windows\bin\qrencode.dll`) into the same folder as `qrtp.exe`.

### 4. Run it — same commands as Linux

```powershell
.\build\Release\qrtp.exe demo myfile.pdf --out .\received\
.\build\Release\qrtp.exe render-qr "hello" out.pbm

# Real transfer between two Windows PCs (needs step 1's opencv install on both):
.\build\Release\qrtp.exe receive --out .\received\ --camera 0    # start this side first
.\build\Release\qrtp.exe send myfile.pdf --camera 0 --fps 10     # camera facing the receiver's screen
```

`optical_link.cpp` opens the camera with the `cv::CAP_DSHOW` backend on Windows specifically —
OpenCV's auto-selected default (Media Foundation) is well-documented as slower to open and less
reliable with some webcam drivers than explicitly requesting DirectShow.

### 5. The GUI app (`qrtp_gui.exe`)

Step 2's build also produces `qrtp_gui.exe` — a native Win32 GUI (no extra framework/runtime; the
single most portable option) with:

- **Invia file / Ricevi file** mode switch
- A file picker (send) / folder picker (receive)
- Camera index and FPS fields
- **Avvia** / **Ferma** buttons — Ferma cooperatively cancels an in-progress transfer within about
  150ms, it doesn't just kill the thread
- A live log of the handshake/adaptive/retransmit messages

Run it by double-clicking `qrtp_gui.exe` — no command line needed. The actual QR codes (while
sending) or camera feed (while receiving) appear in a separate OpenCV-managed window, since
embedding OpenCV's rendering inside the Win32 window would add real risk for no real benefit here.

**Making it installable:** `windows/installer.iss` is a ready-to-use [Inno Setup](https://jrsoftware.org/isinfo.php)
(free) script. Since `VCPKG_APPLOCAL_DEPS` (on by default with the vcpkg toolchain) already copies
every DLL `qrtp_gui.exe` needs next to it in `build\Release\`, that folder is already a complete,
self-contained payload — just make sure `qrencode.dll` is in there too (see step 3), open
`installer.iss` in the Inno Setup Compiler, and press Ctrl+F9 to produce `QRTP-Setup.exe`.

**Just want to copy it to another PC without an installer?** `build\Release\` (once it also has
`qrencode.dll` in it) is already fully portable — zip that folder, copy it to the other Windows
machine, and run `qrtp_gui.exe` directly. No installer needed; that's the "as simple/portable as
possible" option.

**Not build- or run-tested** — see [What broke during testing](#what-broke-during-testing-and-why)
and `not_working.md` for the specific things most likely to need a fix on first real build (DLL
naming, calling convention).

---

## Usage

### Hardware-free: loopback demo

```bash
./bin/qrtp demo myfile.pdf --loss 0.05 --corrupt 0.03 --out ./received/
```

Runs a real `Sender` and `Receiver` in two threads, connected by an in-process `LoopbackLink` pair
(optional synthetic loss/corruption), and prints the handshake/retransmit/adaptive log as it
happens. No camera or monitor needed — this is the same protocol engine `send`/`receive` use, just
with the physical layer simulated.

### Prove the QR encoder works

```bash
./bin/qrtp render-qr "hello qrtp" out.pbm
```

Renders real QR modules via `libqrencode` to a portable-bitmap image you can open in any image
viewer (or `convert out.pbm out.png`).

### Real optical link (requires `make WITH_OPENCV=1`)

```bash
./bin/qrtp receive --out ./received/ --camera 0     # start the receiving side first
./bin/qrtp send myfile.pdf --camera 0 --fps 10       # on the other machine, camera facing its screen
```

---

## Web app (browser, no install)

`web/` is a static site — no server-side code, no build step to distribute it — that runs the same
protocol core compiled to WebAssembly, plus a second, independent encrypted-messaging tool. Grab
`qrtp-web.zip` from [Releases](https://github.com/AlePompeo/qrtp/releases), unzip it, and serve it
over plain HTTP (opening
`index.html` directly as a `file://` URL won't work — browsers block WebAssembly/ES module loading
from local files):

```bash
cd qrtp-web        # the unzipped folder
python3 -m http.server 8000
```

Open `http://localhost:8000/` on both devices. The page has two independent tabs:

- **Trasferimento file** — the WASM port of QRTP itself: pick a role (send/receive), a mode
  (bidirectional or one-way broadcast), grant camera access, and go. Same ARQ/AIMD logic as the
  native build (`src/wasm_link.cpp` reuses the protocol core unmodified), plus the experimental
  color-multiplex QR and JAB Code toggles for higher throughput.
- **Messaggi cifrati** — encrypted point-to-point text messaging, integrated from
  [qr-messenger](https://github.com/AlePompeo/qr-messenger): set the same shared passphrase on
  both devices, write a message, and it's encrypted (**AES-256-GCM**, key derived via **PBKDF2**
  with 100,000 iterations and a random salt) and shown as a QR code for the other device to scan
  (or decode from an uploaded photo). Anyone who scans the QR without the passphrase — even with a
  different app — sees only opaque ciphertext; a wrong passphrase makes AES-GCM's own
  authentication tag fail instead of showing corrupted text. This isn't meant for high-risk
  communications (no forward secrecy, and the passphrase itself must be agreed on over a channel
  you already trust), but it's robust against anyone who only ever sees the QR code itself.

The two tabs are fully independent — different encoding, different libraries (the file-transfer
tab uses the WASM protocol core; the messenger tab uses `qrcode-generator` and `jsQR`, vendored
under `web/vendor/` with their original licenses), and switching tabs releases whichever camera
stream was active.

---

## Architecture

```
Makefile           Linux/macOS build rules (make / make test / make WITH_OPENCV=1)
CMakeLists.txt      Cross-platform build (Linux/macOS/Windows) — see "Building on Windows" above
include/qrtp/       Public headers
src/                Implementation
  crc32.*           CRC-32/ISO-HDLC (frame integrity)
  sha256.*          SHA-256 from scratch (end-to-end file integrity)
  framing.*         Binary frame header pack/unpack, bounds- and CRC-checked
  adaptive.*         AIMD controller: window size, chunk size, EC level, FPS, RTT/RTO estimation
  link.*            ILink interface + LoopbackLink (hardware-free, thread-safe, lossy channel sim)
  qr_encoder.*       Real QR generation via libqrencode, loaded with dlopen (no -devel needed)
  optical_link.*     Real screen+camera ILink, gated behind WITH_OPENCV=1 (uses OpenCV)
  protocol.*         Sender/Receiver: handshake, sliding window, selective-repeat ARQ, teardown
  main.cpp           CLI (render-qr / demo / send / receive)
  gui_win32.cpp      Native Win32 GUI (qrtp_gui.exe), Windows + WITH_OPENCV only
tests/test_main.cpp  Self-contained assertions, no external test framework
windows/
  installer.iss      Inno Setup script — packages qrtp_gui.exe into an installer
web/                 Static browser app (see "Web app" above), built via WITH_WASM
  index.html          Two tabs: file transfer + encrypted messaging
  main.js             File-transfer tab: camera capture, QR render/decode bridge to the WASM module
  messenger.js         Encrypted-messaging tab: AES-256-GCM/PBKDF2, independent of the WASM module
  vendor/              Third-party libs for the messenger tab (qrcode-generator, jsQR) + licenses
```

### Frame format

Every QR code carries one binary frame:

| Offset | Size | Field | Description |
|--------|------|-------|--------------|
| 0 | 1 B | Version | Protocol version (`0x01`) |
| 1 | 1 B | Type | `SYN` `SYN_ACK` `ACK` `DATA` `SACK` `FIN` `FIN_ACK` `CALIB` `PAUSE` `RESUME` |
| 2 | 2 B | Session ID | Random per-transfer identifier, rejects stale/foreign frames |
| 4 | 4 B | Sequence Number | For `DATA`: the **byte offset** within the file (TCP-style byte sequencing, not a frame counter — needed because chunk size varies as the controller adapts) |
| 8 | 4 B | Ack/Window field | Cumulative ACK (next expected byte offset), or proposed window size at handshake time |
| 12 | 1 B | Flags | `MORE_FRAGMENTS` `LAST_FRAME` `RETRANSMIT` `CALIBRATION_PATTERN` |
| 13 | 2 B | Payload Length | Bytes of payload that follow |
| 15 | var | Payload | File chunk (`DATA`), transfer metadata (`SYN`), a list of received `(offset, length)` ranges (`SACK`), ... |
| 15+N | 4 B | CRC32 | Checksum over header + payload |

19 bytes of fixed overhead per frame (23 with CRC). `decode_frame` rejects anything short,
size-inconsistent, or CRC-mismatched *before* any field is trusted — the decoded QR payload is
treated as untrusted external input, same as bytes off a socket.

### Handshake (3-way, like TCP)

1. Sender → `SYN`: session ID, filename, filesize, SHA-256, proposed window.
2. Receiver → `SYN_ACK`: its own window hint.
3. Sender → `ACK`: negotiated window, then `DATA` frames begin.

An `ACK`/`CALIB` calibration burst to auto-tune starting FPS/QR density is designed for (frame
types reserved) but not implemented yet — the receiver treats the very first `DATA` frame as
implicit proof the handshake completed, so a lost `ACK` doesn't stall the transfer.

### Sliding window & ARQ

Selective-repeat, not go-back-N: optical read failures are expected to be isolated drops rather
than a hard cutoff, so re-sending only the specific missing byte ranges is cheaper than re-sending
everything after the first gap. The receiver's `SACK` carries a cumulative ACK (next contiguous
expected offset) plus a list of `(offset, length)` ranges it has buffered ahead of that point
(mirrors TCP's SACK option, RFC 2018). **The receiver ACKs every `DATA` frame** (no batching by
default) — see [What broke during testing](#what-broke-during-testing-and-why) for why batching is
unsafe here.

Retransmission timing follows the same Jacobson/Karels estimator TCP uses: `SRTT` and `RTTVAR` are
updated from each ACK's round-trip time, and `RTO = SRTT + 4*RTTVAR`.

### Adaptive performance control

An AIMD controller (`adaptive.cpp`) tracks a rolling clean/dirty history over the last 8 rounds:

- **Clean stretch** (rolling failure rate ≤ 15%): window size +1; once chunk size is maxed, steps
  the EC level down a notch (less redundancy, more payload) instead.
- **Loss event** (a retransmission timeout fires): window size halved, and chunk size shrinks
  (once chunk size is at its floor, EC level steps up a notch instead — smaller, more redundant
  frames), and FPS backs off.

This is the same intuition as TCP congestion control, just re-targeted: "congestion" here is
optical read failure, and the knobs tuned are window size, QR payload density, EC level, and
display rate instead of a byte-oriented congestion window.

### Integrity

Two independent layers, deliberately redundant:

- **Per-frame CRC32** — cheap, catches any corruption a decoded QR still passed along.
- **End-to-end SHA-256** — computed over the whole file before sending, recomputed by the receiver
  after reassembly, compared before the transfer is reported successful.

### Teardown

Once the sender's last `DATA` frame is covered by a cumulative ACK, it sends `FIN`. The receiver
verifies the reassembled file's SHA-256 and answers `FIN_ACK` with a success/failure flag.

---

## What broke during testing (and why)

Two real bugs surfaced only once the full protocol ran end-to-end under simulated loss — worth
recording since neither was obvious from reading the code in isolation:

1. **SACK batching + a collapsed window starves the sender.** The first version batched SACKs
   every 4 `DATA` frames to reduce reverse-channel traffic. Once the AIMD controller shrank the
   window down to 1 (which happens quickly under any real loss), the sender only ever has *one*
   frame in flight — so the receiver's "wait for 4" batching condition could never be reached from
   fresh arrivals alone, and every single frame paid a full wasted retransmission timeout before an
   immediate-SACK-on-retransmit fallback rescued it. Net effect: correct, but ~20x slower than it
   should be. Fix: default to acking every frame (`sack_every_n_frames = 1`); batching above the
   sender's window size is never safe, and the sender's window can shrink to 1 at any time.
2. **An unreachable recovery threshold pinned the controller at its floor.** Upshifting required a
   *perfectly clean* rolling window of 8 rounds (`fer_low = 0.01`, and even one failure in 8 gives
   a rate of 0.125 — already over that bar). Under a realistic ~15% effective per-round failure
   rate (5% loss + 3% corruption compounding across both the `DATA` frame and its ACK), the
   probability of ever seeing 8-in-a-row clean is low enough that the controller degraded to its
   floor (window=1, minimum chunk size, max EC level) and then statistically never recovered — a
   20KB transfer took 90+ seconds and counting before this was found. Fix: `fer_low = 0.15` ("at
   most ~1 bad round out of 8 is still fine to upshift on"), which let the same test recover and
   finish in well under a second.

Both were confirmed via `make test`'s end-to-end case (5% loss / 3% corruption / 20000 random
bytes) going from "runs for 90+ seconds without finishing" to "completes, byte-for-byte correct."

---

## On throughput

Measured (not estimated) with `make test`'s scenario — 20000 bytes, 5% loss, 3% corruption, 2ms
simulated one-way latency: the AIMD controller starts at window=4/chunk=256, degrades under the
injected loss down to window=1/chunk=32/EC=H within the first ~10 rounds, then recovers back up to
window=6/chunk=68 by the time the transfer completes — self-correcting in both directions, as
designed. Real screen+camera throughput will be dominated by camera capture/decode latency and
display refresh sync rather than QR capacity itself; the adaptive controller's job is to find the
actual achievable operating point for a given camera/lighting setup rather than assume one.
