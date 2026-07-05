# Known Issues / Debug Log

Living log for real failures — paste actual error output/tracebacks here as they happen, with root
cause and fix once found, so the same failure mode doesn't get re-diagnosed from scratch next
session.

## Resolved

### Fixed a real hash-mismatch bug on large files — stale chunk overlapping a re-split one (2026-07-05)

**Symptom**: user reported hash mismatches "often" on large files. Reproduced reliably in a
loopback demo transfer (2MB random file, `--loss 0.1 --corrupt 0.02` — the same rates used
successfully many times earlier this session on smaller files) — the receiver logged
"HASH MISMATCH" after a clean-looking transfer completion. Loopback rules out camera/optical
factors entirely: `LoopbackLink` (`src/link.cpp`) is a strict FIFO queue per direction (fixed
latency, no reordering), and a single-bit flip is mathematically always caught by CRC32 (not
probabilistic), so every frame that reaches `chunks[]` is byte-perfect on its own — this was a
pure protocol logic bug, not a decode/corruption issue.

**Root cause, traced from a real captured log**: when a "stuck offset" (an unacknowledged in-flight
DATA frame) times out, `Sender::send_file()` splits it into smaller sub-pieces — the *first*
sub-piece always reuses the original offset as its own key, but later sub-pieces land at *new*,
higher keys strictly inside the original's byte range (`sub_off = original_off + i`). If that
*first* sub-piece is the one that gets lost optically, the receiver's `chunks[original_off]` entry
never gets overwritten — it just sits there at its old, larger size — while the *later* sub-pieces
(which are new keys, not overwrites) get accepted normally, since the existing "smaller-or-equal
wins" check (`not_working.md`, various earlier entries) only ever compares an arrival against
whatever's *at that exact key*, never against a *different*, smaller key whose range the new
arrival falls inside. `advance_cumulative()` still reaches the correct total length by walking
straight through the stale, oversized entry (it only follows one specific chain of keys, jumping by
whatever size currently sits at each one) — but the final reassembly loop concatenates *every*
entry in the map regardless of which chain `advance_cumulative()` walked, so it includes *both* the
stale entry and the newer, finer-grained pieces that overlap part of its range, duplicating those
bytes in the output. Confirmed byte-for-byte in a real log: offset 93623 (1024 bytes) never got its
first replacement sub-piece; its second sub-piece landed at the new key 94305 (342 bytes); the
stale 1024-byte entry at 93623 and the new 342-byte entry at 94305 then both existed simultaneously,
overlapping by exactly the amount logged. Larger files simply have far more DATA frames and re-split
events, so far more chances for this specific race to land.

**Fix**: two changes in `Receiver::receive_file()` (`protocol.cpp`). (1) The real fix — right before
accepting a new arrival, check whether the immediate predecessor key in `chunks` (found via
`chunks.upper_bound(f->seq)`, stepped back one) has a range that extends past this new offset; if
so, truncate that predecessor's stored bytes down to just the correct prefix (`f->seq - pred->first`
bytes), ceding the rest to the finer-grained pieces now arriving separately, so the two ranges never
overlap in the map at all. (2) A diagnostic safety net in the final reassembly loop — logs loudly if
any *other*, not-yet-found inconsistency slips through (walking `chunks` while tracking the expected
running offset, reporting a gap or overlap by name and size instead of silently producing wrong
bytes) — this is what actually caught and pinpointed the bug above; left in place since a naive
"blindly concatenate every map entry" loop has no other way to notice this class of problem. Also
hardened the *sender's* own SACK-range handling (`Sender::send_file()`) to require the reported
length match what's currently in flight at that offset before erasing it — a SACK range citing a
stale, pre-split size at an offset shouldn't be treated as evidence the *current* (already
re-split) in-flight entry there was received.

**Verified**: `make test` (all checks pass), the *exact* 2MB loopback transfer that reliably
reproduced the bug before the fix, re-run 4 times consecutively after the fix — all 4 succeeded
with hash verified, byte-for-byte identical output, and zero reassembly-inconsistency log lines
(previously: reliably failed, with 5-34 logged overlaps per run). Also re-verified the smaller,
previously-passing demo case, native build, and the Windows cross-compile (all three .exe targets)
build/pass cleanly.

### Made AdaptiveConfig::chunk_size_max runtime-overridable (2026-07-05)

**Ask**: expose the QR payload density ceiling (`AdaptiveConfig::chunk_size_max`, default 1024
bytes/DATA-frame) as a runtime opt-in, without changing the current default.

**Fix**: added `--chunk-max N` to both `qrtp send`/`qrtp receive` (parsed into a local
`std::optional<size_t>` during argument parsing, applied to `cfg.adaptive.chunk_size_max` right
after `ProtocolConfig cfg` is constructed — mirrors how every other optical CLI flag is threaded
through) and a new "Chunk max:" field in the Win32 GUI (pre-filled "1024", the current default;
falls back to 1024 if emptied/zero, same pattern as the QR-per-frame field). GUI window grown by
23px to fit the new row. No change to the default itself — `AdaptiveConfig::chunk_size_max` stays
1024 in `adaptive.hpp`. Deliberately not added to `qrtp demo` (loopback, no camera constraints) —
this knob's whole rationale (QR version vs. module-size/decode-reliability trade) only applies to
the real optical link, unlike, say, `--qr-grid` which is exercised meaningfully either way.

**Verified**: `make test`, a full loopback demo transfer (byte-for-byte identical + hash match),
native build, and the Windows cross-compile (all three .exe targets, including the resized GUI
layout) build/pass cleanly.

### WeChatQRCode + 640x480 promoted from GUI-only to library/CLI defaults (2026-07-05)

**Ask**: after setting WeChatQRCode-on/640x480/30fps as GUI-only defaults (previous entry), the
user decided WeChatQRCode and 640x480 specifically should also become the library/CLI defaults —
fps was *not* included in this, so it stays GUI-only at 30 while the library/CLI default stays
3.0 (`AdaptiveConfig::fps_initial`/`OpticalLinkConfig::fps`, deliberately unchanged since raising
it at the code level has caused real regressions before — see the fps_initial=24 history earlier
this session).

**Fix**: `OpticalLinkConfig::camera_width`/`camera_height` now default to `640`/`480` (was
`1280`/`720`), and `OpticalLinkConfig::use_wechat_qrcode` now defaults to `true` (was `false`).
Since `use_wechat_qrcode` flipped its default, the CLI flag changed from `--wechat-qrcode` (turn
on) to `--no-wechat-qrcode` (turn off) — same rename pattern already used for `--mjpeg` ->
`--no-mjpeg` when `request_mjpeg`'s default flipped earlier this session. Updated the GUI's own
code comments for the resolution field and WeChatQRCode checkbox, since they're no longer
"GUI-only, differs from the library default" — they now simply match it; only the FPS field's
comment still describes a real GUI-vs-library divergence.

**Verified**: `make test`, a full loopback demo transfer (byte-for-byte identical + hash match),
native build, and the Windows cross-compile (all three .exe targets) build/pass cleanly. NOT
verified on real camera hardware — this is now the out-of-the-box experience for both the GUI and
`qrtp send`/`qrtp receive`/`qrtp_gui.exe` alike, so the first real transfer on this combination is
the one that matters most to hear back about.

### GUI-only defaults changed: WeChatQRCode on, 640x480, 30fps (2026-07-05)

**Ask**: set the GUI's pre-filled defaults to WeChatQRCode enabled, 640x480 resolution, and 30fps —
explicitly GUI-only, not a change to the library/CLI defaults.

**Fix**: in `gui_win32.cpp`'s `WM_CREATE` handler only — `IDC_FPS_EDIT` pre-fills "30" (was "3"),
`IDC_RES_EDIT` pre-fills "640x480" (was "1280x720"), and `IDC_WECHAT_CHECK` is now
`CheckDlgButton(...BST_CHECKED)` by default (was unchecked). `OpticalLinkConfig::fps` (3.0),
`camera_width`/`camera_height` (1280/720), and `use_wechat_qrcode` (false) — the values `qrtp send`/
`qrtp receive` and the AdaptiveController actually start from — are untouched; these are only the
text/checkbox state a GUI user sees before pressing "Avvia", same as any of these fields already
were. The `if (fps <= 0.0) fps = 3.0;` fallback (for an emptied/invalid field) is also left as-is,
since it's a safety floor, not itself a "default" in the sense asked for here.

**Verified**: `make test`, a full loopback demo transfer (byte-for-byte identical + hash match),
native build, and the Windows cross-compile (all three .exe targets) build/pass cleanly. NOT
verified on real camera hardware — this combination (WeChatQRCode + lower resolution + higher fps
than this project's own handshake-safe starting point) is untested; see the AdaptiveConfig::
fps_initial history for why starting fast has caused real regressions before. That risk applies
here too since this bypasses it by construction (GUI now opens with 30 pre-filled, not 3).

### Integrated WeChatQRCode as an opt-in decoder, chosen over zxing-cpp (2026-07-05)

**Context**: after comparing OpenCV's classic `cv::QRCodeDetector` against two alternatives
(zxing-cpp, a standalone library; `cv::wechat_qrcode::WeChatQRCode`, an opencv_contrib module —
an SSD-style detector CNN plus a super-resolution enhancement step for small/blurry crops), picked
WeChatQRCode: it needs no new third-party dependency (it's a module of the same OpenCV this project
already links against), and its detection reliability is reportedly strong exactly where this
project's real logs have repeatedly shown trouble — small/blurry/angled QR codes, which is a
decode-*reliability* problem, the dominant bottleneck this whole session's evidence has pointed at
(retries/downshifts cost far more than a few extra ms of per-attempt CPU).

**Build complication (the real cost of this integration)**: the Windows cross-compile's OpenCV was
not the Fedora system package (which already ships wechat_qrcode+dnn) but a separately-built,
deliberately minimal custom OpenCV (`BUILD_LIST=core,imgproc,imgcodecs,videoio,highgui,objdetect`,
`WITH_QT=OFF`, `WITH_GSTREAMER=OFF`, `WITH_GDAL=OFF`) — confirmed by testing: linking against the
Fedora package pulled in a ~90-DLL transitive closure (Qt5, GStreamer, GDAL, FFmpeg, and more),
completely at odds with this project's portable-single-folder distribution. Rebuilt the same custom
OpenCV tree from source instead, adding `opencv_contrib`'s `wechat_qrcode` module and enabling `dnn`
(with `WITH_PROTOBUF=ON`/`BUILD_PROTOBUF=ON` so protobuf is statically bundled into
`libopencv_dnn4110.dll` rather than pulling in a separate `libprotobuf-30.dll`) — replicating every
other `WITH_*`/`BUILD_*` flag from the original cache exactly, to avoid silently reintroducing any
of that bloat. Net new runtime dependency: just `iconv.dll` (verified via a full transitive
dependency-closure walk with `objdump`, not assumed).

**Real build-environment failure hit along the way**: the rebuild first crashed under `-j6` — not
from raw compiler memory alone (a `journalctl` OOM-killer log showed only ~300MB total across the
`cc1plus` processes at kill time) but because the entire OpenCV source+build tree lived under
`/tmp`, which is a 2GB **tmpfs** (RAM-backed) on this machine, not real disk — so the build's own
on-disk footprint was directly competing with process RAM. The specific failure
(`ar: unable to copy file 'objects.a'; reason: Success`) is the classic symptom of an ENOSPC hitting
a buggy errno-to-string path. Fixed by relocating the entire build tree to real disk before
resuming, and rebuilding with `-j1` to keep peak concurrent compiler memory low given this
machine's 3.8GB total RAM. Lost the ~168 already-compiled `.o` files for the `dnn` module in the
process (a full CMake reconfigure — unavoidable, needed to restore the toolchain file and the exact
`WITH_*` flag set — refreshes `flags.make`/`build.make`, which `make` treats as invalidating
everything depending on them), so `dnn` was recompiled from scratch single-threaded; slower, but it
completed cleanly with no further crashes.

**Fix**: added `OpticalLinkConfig::use_wechat_qrcode` (opt-in, default `false` — untested on real
camera hardware, unlike the MJPEG default which reflects a confirmed A/B) and
`wechat_qrcode_model_dir` (defaults to a `models/wechat_qrcode` folder next to the running
executable, resolved via `GetModuleFileNameW`, matching the existing "next to the .exe" convention
already used for `libqrencode.dll`). `optical_link.cpp` branches at both decode call sites
(`try_receive()`/`try_receive_multi()`) between the classic detector and
`cv::wechat_qrcode::WeChatQRCode::detectAndDecode()` (which conveniently already returns multiple
strings per call, mapping directly onto the existing multi-QR path with no separate "multi" method
needed). Wired through `--wechat-qrcode`/`--wechat-model-dir` on both `qrtp send`/`qrtp receive`,
and a new "Usa WeChatQRCode (CNN)" checkbox (unchecked by default) in the Win32 GUI. The 4 required
model files (`detect.prototxt`, `detect.caffemodel`, `sr.prototxt`, `sr.caffemodel`, from
github.com/WeChatCV/opencv_3rdparty branch `wechat_qrcode` — the canonical source
opencv_contrib's own module points at, Apache-2.0 licensed) are committed under `models/
wechat_qrcode/` and bundled into `dist-windows.zip` at the same relative path.

**Verified**: `make test` (all checks pass), a full loopback demo transfer (byte-for-byte identical
+ hash match), native build, and the Windows cross-compile (all three .exe targets, linked against
the newly-rebuilt custom OpenCV with `dnn`+`wechat_qrcode`) build cleanly — dependency closure
re-verified with `objdump` against the new binaries specifically, not assumed from the module list.
NOT verified on real camera hardware — whether WeChatQRCode's detector actually improves decode
success rate on this project's real optical channel, and what its added per-attempt CPU cost looks
like in practice, are both still open questions.

### Compared against jtalbott22/optical-data-bridge: fixed a real receiver-side fps gap, added decode downscaling (2026-07-05)

**Ask**: user pointed at github.com/jtalbott22/optical-data-bridge (a browser-based, single-HTML-file
optical transport protocol with a similar design goal — screen+camera file transfer with adaptive
rate control) and asked what it revealed as missing from this project. Cloned and read its source
(`optical-data-bridge.html`) alongside its README to compare architectures.

**What's architecturally similar (no gap)**: bidirectional link where each side both displays and
reads QR codes; per-frame CRC (they use CRC16, this project CRC32) plus a whole-file hash verified
end-to-end; selective retransmission driven by an explicit missing-range report; starting
conservative (their `Adaptive()` starts at "1 unit, 1 tile, 3fps" — the same 3fps starting point
this project converged back to independently) and backing off on errors. These are already-sound
choices, not gaps.

**Real gap found #1 — the Receiver's own display fps was never adapted.** Their sender ramps fps,
then density, then tiling, in strict sequence; more importantly, tracing through this project's own
`protocol.cpp` while making the comparison surfaced that `link_.set_fps()` is called only from
`Sender::send_file()` — `Receiver::receive_file()` never calls it at all. Since
`OpticalLink::send()` blocks on `cv::waitKey(1000/fps)` before returning, and
`ProtocolConfig::sack_every_n_frames` defaults to 1 (a SACK sent for *every* DATA frame), every
single SACK the receiver sends was blocking its *entire* receive loop (including reading further
incoming DATA) for up to `1000 / receiver_fps` ms — and `receiver_fps` was whatever fixed value the
GUI/CLI fps field held for the entire transfer, never increased. A receiver left at the safe
fps=3.0 default would stall for up to 333ms after *every* DATA frame it acknowledged, regardless of
how fast the sender had already ramped up. This plausibly explains part of the residual slowness
seen even after MJPEG/autofocus/resolution were settled, and is a real, code-verified structural gap
— not a guess.

**Fix**: gave `Receiver` its own `AdaptiveController adaptive_` (protocol.hpp/protocol.cpp),
constructed from the same `cfg_.adaptive`. Only its `.fps()` is consulted — `window_size()`,
`chunk_size()`, and `ec_level()` are irrelevant to the receiver and simply climb unused. Whenever a
DATA frame causes genuine forward progress (`cumulative` actually advances) *and* wasn't a
sender-side retransmit, `adaptive_.record_clean_round()` is called, mirroring exactly the check the
Sender already uses (`advanced && !any_was_retried`) — a frame the sender had to retry isn't
evidence this side's own display cadence is safe to push faster. `send_sack()` now calls
`link_.set_fps(adaptive_.fps())` right before every `link_.send()`, the same pattern the Sender
already uses. Deliberately only ever ramps up (no `record_loss_event()` call added) — keeps the
change narrow and low-risk, bounded by the same proven-safe `fps_initial=3.0` start and `fps_max=60`
ceiling already validated on the Sender side.

**Real gap found #2 — decode cost scaling with capture resolution, unaddressed.** An earlier entry
this session already noted "a higher-resolution frame is more expensive for cv::QRCodeDetector to
scan per attempt... its cost tracks the captured frame's size, not just the QR's own module count,"
but nothing acted on it. The reference project explicitly downscales every captured frame to a
bounded 800px (`SCAN_W`) before running its decoder (`jsQR`), regardless of the camera's actual
capture resolution (which it requests at up to 1920x1080 `ideal` — so it keeps capture sharp, but
bounds *decode* cost separately). This project had no equivalent: `detectAndDecode()` ran on
whatever the camera negotiated, now defaulting to 1280x720.

**Fix**: added `downscale_for_decode()` (`optical_link.cpp`, anonymous namespace) — bounds the
longest side of the grayscale frame to 800px (matching the reference's own value) via
`cv::resize(..., cv::INTER_AREA)` before either `detectAndDecode()` or `detectAndDecodeMulti()`.
`INTER_AREA` specifically because it's the recommended OpenCV interpolation for shrinking (avoids
aliasing/moiré that `INTER_LINEAR`/`INTER_NEAREST` can introduce on fine, high-contrast QR module
edges). Only ever shrinks — an already-small capture (e.g. an explicit lower `--width`/`--height`
override) passes through unchanged. The live camera preview window still shows the original,
unscaled frame; only the copy fed to the decoder is affected.

**Verified**: `make test` (all checks pass, including the full Sender/Receiver loopback test that
exercises the new Receiver-side `adaptive_`), a full loopback demo transfer at higher loss/corruption
than previous verification runs (0.15/0.03, byte-for-byte identical + hash match), native build, and
the Windows cross-compile (all three .exe targets) build/pass cleanly. NOT verified on real camera
hardware for either change — the receiver-fps fix in particular changes real protocol timing
behavior that this session's loopback tests can't observe (loopback has no display/capture latency
to begin with), so real-hardware feedback on both changes is the next useful signal.

**Ideas surfaced but deliberately not implemented, for later discussion**:
- **Sequential ordered-knob optimizer** (ramp fps fully, then density, then tiling, one at a time,
  in a fixed order; back off in reverse) instead of this project's AIMD adjusting window/chunk/
  EC/fps together every round. Could reduce the kind of correlated capture/decode-latency swings
  seen in earlier real logs by isolating one variable at a time — but it's a genuine rewrite of
  `adaptive.cpp`'s core model, not a tweak, and this project's current AIMD has a lot of validated
  tuning behind it already. Worth a dedicated design discussion, not a drive-by change.
- **Link watchdog / full reset on prolonged silence**: their sender resets its entire adaptive
  state to conservative defaults if no receiver report is seen for 6s. This project has
  per-offset RTO retransmission and `max_data_retries`, but nothing that resets `window_size_`/
  `chunk_size_`/`ec_level_`/`fps_` back toward their initial values after a long *total* stall
  (e.g. camera knocked out of focus mid-transfer). Would need `AdaptiveController::reset()` plus a
  "time since any successful SACK" tracker in `Sender::send_file()`. Reasonably contained to add
  later if real-hardware testing shows recovery from a bad state is too slow/grindy.
- **Decode throttle decoupled from capture rate** (their `SCAN_MS=55` bounds decode attempts to
  ~18/s regardless of how fast frames arrive): this project's `CaptureThread` already overlaps
  capture with decode on separate threads, and capture is driver-paced rather than free-running, so
  the benefit is less clear-cut here than in their single-threaded browser loop. Not implemented —
  would need real capture-rate-vs-display-rate data from this project's own hardware to tune a
  throttle interval without guessing, and a wrong guess risks *missing* genuinely new frames at high
  fps, unlike the downscale change above which has no such failure mode.
- CRC16 vs. this project's CRC32 per frame: ~2 bytes/frame difference, not meaningful at current
  chunk sizes — not worth changing.

### Best-known real-hardware config confirmed: MJPEG on, autofocus on, 1280x720, fps ramping 3->24 (2026-07-05)

**User report**: after testing the v38 toggles, the best-performing combination found on real
hardware is MJPEG on (already the default), autofocus left on/not locked (already the default),
resolution fixed at 1280x720, and fps effectively reaching ~24 — but explicitly *not* by hard-coding
`fps_initial = 24`, which the user separately confirmed behaves differently (worse) than reaching 24
by starting at 3 and letting it climb during the session. They asked for these settings to become
the defaults and for a review of any further optimization opportunities.

**On the fps point specifically**: this matches the project's own existing reasoning, not a new
finding — `AdaptiveConfig::fps_initial = 3.0` exists precisely so the handshake and first few rounds
happen before the camera's autofocus/auto-exposure has necessarily settled, and `upshift()`'s fps
growth (`fps = min(fps_max, fps*1.4 + 0.5)`) reaches ~24-30 within roughly 5-6 clean upshift events
from a starting point of 3 — which lines up with "it becomes fast soon after the program has been
running for a bit." No code change needed here: this is the AIMD already doing its job, and
confirms (rather than contradicts) the earlier decision to revert a hard-coded `fps_initial = 24`.

**Fix**: changed `OpticalLinkConfig::camera_width`/`camera_height` defaults from
`kAutoMaxResolution` (auto-detect the camera's highest mode) to a fixed `1280`/`720`. The GUI's
resolution field now pre-fills "1280x720" instead of "max" to match. Both remain fully overridable
(`--width -1 --height -1` / typing "max" opts back into auto-detection; `--width 0 --height 0` /
"0x0" disables the request entirely; any other WxH requests that specific mode) — this default is a
real per-camera preference from this user's testing, not a claim that it's universally optimal,
consistent with the project's existing caution that the exact same "auto-max" request was already
shown negotiating to a worse mode (1280x1024) on a different machine.

**Other optimization opportunities reviewed, deliberately left unchanged**:
- `chunk_size_max = 1024` sits well below what the current default EC level (M) or a lower one (L)
  could actually encode (real ceiling ~1723 bytes at M, ~2191 at L, per
  `max_chunk_for_ec_level()`/`kVersion40Capacity`) — so there's unused headroom on paper. Not
  touched: this ceiling has a documented history of real regressions when raised (2048 caused a
  severe render/decode lag reported earlier this session), specifically because larger QR versions
  shrink module pixels at a fixed on-screen size — a decode-reliability risk, not just a math
  question, and this session has no camera hardware to test whether that risk is still present at
  the new 1280x720 baseline. Changing it now would be exactly the kind of speculative, unverified
  change this project has repeatedly had to revert.
- The background `CaptureThread` (overlapping capture with decode), thread priority bumps, and the
  render loop's O(width) rebuild (vs. the old O(width²) per-module bug) were already addressed
  earlier this session and remain unchanged — no new evidence suggests revisiting them.
- `qr_codes_per_frame` (multi-QR-per-frame batching) remains off by default and untested on real
  hardware — worth trying as the next throughput lever now that camera-level settings (MJPEG,
  autofocus, resolution) are on a confirmed-good baseline, but not enabled by default since it's
  still unverified.

**Verified**: `make test`, a full loopback demo transfer (byte-for-byte identical + hash match),
native build, and the Windows cross-compile (all three .exe targets) all build/pass cleanly.

### Added opt-in autofocus lock to test the still-open capture/decode latency correlation (2026-07-05)

**Context**: the previous entry left an open question — in the MJPEG-off log, `avg_capture_ms` and
`avg_decode_ms` both dropped sharply and together (~103-134ms -> ~27-40ms) exactly when the
transfer fell into a low-fps single-offset retransmit loop, with two candidate explanations: camera
auto-exposure/autofocus hunting under fast-changing on-screen content, or CPU contention if sender
and receiver were sharing one machine's cores. Asked the user directly: confirmed **two separate
physical machines** for this test. That rules out the CPU-contention explanation — each process has
its own cores — leaving camera-side auto-exposure/autofocus hunting as the standing hypothesis
(cap->read() latency shouldn't depend on our own chunk/window/fps choices at all if it's purely
hardware-bound, so a correlation this tight with our own fps transitions points at the camera
reacting to the *content* those transitions produce, not at anything downstream of capture).

**Fix**: added `OpticalLinkConfig::disable_autofocus` (opt-in, default `false` — untested on real
hardware, unlike the MJPEG default flip above which reflected a confirmed A/B). When true, requests
`CAP_PROP_AUTOFOCUS=0` right after opening the camera, disabling continuous refocusing for the rest
of the session. Deliberately narrower than also touching exposure: a manual exposure value picked
blindly (no camera hardware available here to tune one) could make the picture too dark or too
bright on an unknown camera, whereas autofocus off is a simple on/off UVC control with no numeric
guess involved — if this doesn't change anything, exposure is the next, riskier knob to consider.
Wired through `--lock-focus` on both `qrtp send`/`qrtp receive`, and a new "Blocca autofocus"
checkbox (unchecked by default) in the Win32 GUI, threaded through `RunTransfer`. The "camera
opened" log line now also reports whether autofocus was left on or disabled.

**Verified**: `make test`, a full loopback demo transfer (byte-for-byte identical + hash match),
native build, and the Windows cross-compile (all three .exe targets, including the resized GUI
layout) all build/pass cleanly. NOT verified on real camera hardware — whether autofocus hunting is
actually the cause of the capture/decode latency correlation is still an open question; this only
makes it testable without a rebuild.

**Next step**: user should try `--lock-focus` (or the GUI checkbox) on the same kind of transfer
that showed the capture/decode latency dropping at a fps transition, and report whether the
avg_capture_ms/avg_decode_ms values stay flat across fps changes instead of tracking them. If yes,
this confirms autofocus hunting and the lock can likely become the default too, matching MJPEG's
path. If no, revisit the exposure-lock idea, or look at screen refresh/GDI paint timing instead.

### MJPEG A/B test concluded: re-enabled by default, JPEG-artifact theory did not hold up (2026-07-05)

**Symptom**: after making MJPEG opt-in (default off, previous entry), the user tested and pasted a
fresh log. `avg_capture_ms` was ~103-111ms per `cap->read()` call — and critically, this held
regardless of `polls_since_last` (same magnitude at polls=1, 2, 3, 4, 10), meaning it's a real
per-call cost, not a rolling average skewed by a few outliers. That's roughly 100x worse than the
<1.5ms measured with MJPEG on in the log from the previous entry. Meanwhile the reliability problem
this was suspected of causing (stuck-offset splits, retransmit chains, cascading AIMD downshifts)
showed up just as badly with MJPEG off as it did with MJPEG on, and total transfer time did not
improve (36.4s vs. 33.6s previously — if anything, slightly worse).

**Conclusion**: this is a direct, controlled comparison on the same real hardware and the same
class of transfer, and it points the opposite way from the previous entry's hypothesis. The
original USB-bandwidth reasoning for forcing MJPEG (raw YUY2 at this resolution exceeding what
USB/the driver can sustain per frame) is the one that's now directly confirmed, not just plausible.
The JPEG-artifact-hurts-decode-reliability theory is the one that didn't hold up: disabling MJPEG
didn't fix reliability, it just made every capture ~100x slower.

**Fix**: reverted `OpticalLinkConfig::request_mjpeg` default back to `true` (GUI checkbox now
checked by default too). Kept it as a genuine runtime toggle rather than hard-coding MJPEG back in
unconditionally — CLI flag renamed `--no-mjpeg` (was `--mjpeg`) since the default flipped — because
the resolution request already proved real cameras can disagree about what capture mode suits them
best; this only changes which side of that toggle new users start from, given the evidence so far.

**Verified**: `make test`, a full loopback demo transfer (byte-for-byte identical + hash match),
native build, and the Windows cross-compile (all three .exe targets) all build/pass cleanly.

**New open question, worth its own investigation**: in the MJPEG-off log, `avg_capture_ms` and
`avg_decode_ms` both dropped sharply (from ~103-134ms to ~27-40ms) at the exact moment the transfer
fell into a low-fps, single-offset retransmit loop (window=2, chunk=99, fps≈4.9) after a string of
"splitting stuck offset" events — then stayed low even as fps climbed back up afterward. Capture and
decode cost moving together, exactly at a fps/activity transition, doesn't fit a pure camera-
hardware explanation (cap->read() latency shouldn't care about our chunk/window size). Two
candidate explanations, not yet distinguished: (1) the camera's own auto-exposure/autofocus control
loop hunting under rapidly-changing on-screen content (different QR codes flashing by at higher fps
change average scene brightness/contrast quickly), which is a documented cause of variable capture
latency in UVC webcams — testable via `CAP_PROP_AUTO_EXPOSURE`/`CAP_PROP_AUTOFOCUS`, not yet used
anywhere in this codebase; (2) CPU contention if sender and receiver are both running on the *same*
physical machine for testing (webcam pointed at that machine's own screen) — higher fps means more
simultaneous render+capture+decode cycles/sec competing for the same cores, which would inflate
both capture and decode wall-clock time under load and relax immediately once fps drops. Need to
confirm the test topology (one machine or two) before picking which of these to chase next.

### MJPEG (forced since v33) made opt-in — fresh log shows a reliability problem, not a speed one (2026-07-05)

**Symptom**: user asked whether "~200 bytes per QR" was normal, then pasted a real diagnostic log
(13110 bytes transferred in 33.6s). `avg_capture_ms` was consistently fast (~0.58-1.5ms) and
`avg_decode_ms` was stable (~31-34ms) — capture and per-attempt decode cost were *not* the
bottleneck. What stood out instead: `polls_since_last` climbing as high as 22, and a long chain of
"splitting stuck offset" / "retransmit (severity=...)" events driving cascading AIMD downshifts
(window 3->1, fps 11.06->6.94, chunk shrinking repeatedly) that never stabilized. That's a decode
*reliability* problem (many frames never resolving at all, or only after many attempts), not a raw
per-frame speed problem — a different failure mode than what MJPEG was originally forced for.

**Reconsidered MJPEG (v33) under that lens**: it was forced unconditionally based on a USB-bandwidth
argument (raw YUY2 at high resolution can exceed sustainable USB throughput, causing camera/driver
stalling). That reasoning is still plausible for *capture* cost, but capture was already shown to be
fast in this log — so if MJPEG is contributing to the slowness now, it's more likely via a completely
different, previously-unexamined mechanism: JPEG's lossy compression is well known to introduce
ringing/blur artifacts specifically on sharp, high-contrast edges — exactly what a QR module boundary
is. That would directly explain poor decode reliability (many retries needed per frame) without
touching the measured per-attempt decode or capture timings at all.

**Fix**: same treatment already applied to camera resolution when it showed opposite effects on
different hardware (see the COBS/resolution entries below) — made MJPEG opt-in instead of forced.
Added `OpticalLinkConfig::request_mjpeg` (default `false`). `optical_link.cpp` now only calls
`cap->set(CAP_PROP_FOURCC, MJPG)` when this flag is set; the "camera opened: resolution ... fourcc="
log line now also reports whether MJPEG was requested, so a real per-machine comparison is possible.
Wired through `--mjpeg` on both `qrtp send`/`qrtp receive`, and a new "Richiedi MJPEG" checkbox
(unchecked by default) in the Win32 GUI, threaded through `RunTransfer`.

**Verified**: `make test` (all checks pass), a full loopback demo transfer (byte-for-byte identical
output + hash match), native build, and the OpenCV-enabled Windows cross-compile (all three .exe
targets) all build/pass cleanly. NOT verified on real camera hardware (none available in this dev
environment) — whether disabling MJPEG actually fixes the reliability problem is unconfirmed; this
change only removes it as a forced, un-testable variable so the user can compare both ways.

**Open question**: if throughput/reliability recovers with MJPEG off, that confirms the JPEG-
artifact hypothesis and MJPEG should probably stay opt-in permanently (perhaps auto-enabled only at
resolutions high enough that raw capture would genuinely throttle). If it doesn't recover, the
`polls_since_last`/cascading-downshift pattern points somewhere else entirely — lighting, camera
autofocus hunting, or screen refresh/GDI paint timing not yet instrumented.

### Still very slow after reverting fast retransmit — reverted fps_initial back to 3.0 too (2026-07-05)

**Symptom**: after reverting fast retransmit (previous entry), the user reported the transfer was
still very slow, and floated a new hypothesis: maybe the sender is changing QR codes too fast for
the camera to keep up.

**This lines up directly with the project's own oldest real-hardware evidence**: `fps_initial` was
3.0 for essentially this entire project's history, specifically because v6 (very early in this
project) found that even 10fps was already too fast for a real webcam to focus/expose/lock onto
reliably. Raising the *default starting point* to 24 (a few entries back, by request) skipped the
"prove the channel can sustain it first" step the whole AIMD design is built around — jumping
straight to a fast display rate is riskiest right at the very start of a session (the handshake),
before the camera has had any chance to settle focus/exposure at all. In hindsight, the framing
that shipped alongside that change ("based on real hardware testing showing that range works well")
overstated the evidence behind it — the fps=24 request addressed a symptom (occasional missed
frames), but the actual fix attempted for that symptom was fast retransmit, not the starting fps
itself; raising fps_initial to 24 was a separate, less-examined change riding along with it.

**Fix**: reverted `AdaptiveConfig::fps_initial` and `OpticalLinkConfig::fps` back to 3.0 (GUI field
and CLI fallback too). `fps_max` stays at 60 — that ceiling *is* real-hardware-confirmed (v12); a
channel that's actually working well still climbs there quickly via `upshift()`'s existing
multiplicative growth (further sped up by the fer_window/fer_low and growth-rate changes from
earlier this session) — this change only affects where the AIMD is required to start, not how fast
it's still allowed to go once it's proven itself.

**Verified**: `make test`, a full loopback demo transfer (byte-for-byte + hash match), native
build, and the OpenCV-enabled Windows cross-compile all pass/build cleanly.

**Still open**: if throughput remains poor even back at fps_initial=3, the next thing to
reconsider is the MJPEG capture request (v33, still active) — per the controlled-comparison plan
from the previous entry, that's the next variable to isolate once fps is no longer a confounder.

### Throughput got much worse after fps=24 + fast retransmit shipped together — found a real design flaw in fast retransmit, reverted it (2026-07-05)

**Symptom**: real user report — transfer speed got a lot worse, unsure whether the new MJPEG
capture request or the fast-retransmit change from the same session was responsible.

**Analysis (no diagnostic log available, reasoned from the code)**: re-examined the fast-retransmit
mechanism (added alongside fps=24 in the same commit) and found a real flaw I'd argued past too
quickly when I wrote it. The justification at the time was "this channel has no multi-path
reordering like general IP networks do, so a single SACK gap is already reliable evidence of loss,
unlike TCP which needs several duplicate ACKs first." That's true for *network*-path reordering —
but it misses a different, real source of apparent reordering *specific to this channel*: two QR
codes can take genuinely different amounts of time to capture and decode (motion blur, a marginal
focus moment, an awkward angle on just one of them), so a later offset can legitimately get decoded
before an earlier one *without the earlier one being lost at all* — just slower to resolve. Treating
every such gap as confirmed loss, on the very first observation, retransmits offsets that might have
arrived on their own moments later.

That extra retransmit isn't just wasted — it counts toward `severity` in
`record_loss_event(severity)`, which downshifts window/chunk/fps. Combined with fps=24 (raised in
the *same* commit): a faster display rate means more capture attempts per unit time, which means
more opportunities for this specific kind of transient, non-lost gap to appear, which (with no
corroboration requirement) means more self-inflicted "loss events" triggering AIMD downshifts that
wouldn't have fired at all under the original, more patient RTO-based-only retransmit logic. This
is a plausible, mechanism-level explanation for a real, significant slowdown — exactly matching
what was reported — rather than something MJPEG capture (a real but separate change from the same
release) obviously explains as cleanly.

**Fix**: reverted the fast-retransmit logic in `Sender::send_file()` (`protocol.cpp`) back to pure
RTO-timeout-based retransmission — the same code as before v31's fast-retransmit addition.
`fps_initial`/`OpticalLinkConfig::fps` stay at 24 (the user hasn't flagged fps itself as an issue,
and it's an independent change with its own, separate rationale). The MJPEG capture request (v33)
is left as-is for now — deliberately, as a controlled comparison: if throughput recovers with just
this revert, fast retransmit was the (or a major) cause; if it's still slow, MJPEG becomes the next
thing to reconsider, and now there's a real basis for whether it was one, the other, or both,
instead of reverting everything from this session at once and learning nothing.

**If fast retransmit is revisited later**: the fix would be to require corroboration before acting
— e.g., only retransmit early once the same gap has persisted across 2-3 consecutive SACKs, or only
once a minimum fraction of the RTO has already elapsed for that specific offset — closer to what
TCP itself actually does (duplicate-ACK counting), rather than acting on the very first observation
of an ambiguous signal.

**Verified**: `make test`, a full loopback demo transfer (byte-for-byte + hash match), native build,
and the OpenCV-enabled Windows cross-compile all pass/build cleanly, matching the pre-fast-
retransmit baseline (protocol.cpp's SACK-handling code is now identical to before v31).

### Camera lags badly at high resolution in this app but not in Windows' own Camera app — OpenCV was capturing raw/uncompressed instead of MJPEG (2026-07-05)

**Symptom**: real user report — the camera lags a lot in this program specifically at higher
resolutions, but not when opened at the same resolution in Windows' native Camera app.

**Root cause**: this is the textbook symptom of a USB webcam being driven in a raw/uncompressed
capture mode instead of MJPEG. A 1080p raw YUY2 frame is ~4MB — at any real frame rate that
exceeds what USB can sustain (especially USB 2.0), so the camera/driver ends up throttling or
stalling to cope. Native camera apps (Windows' included) request MJPEG — hardware-compressed by
the camera's own sensor/ISP, roughly 10-20x smaller per frame — for exactly this reason once
resolution climbs past a low threshold. OpenCV's DirectShow (`cv::CAP_DSHOW`) backend doesn't do
this automatically; it was left to negotiate whatever pixel format it defaults to for the
requested resolution, which at the higher resolutions this app now requests by default (see the
auto-max-resolution entry above) can mean raw capture and the exact lag described.

**Fix**: `OpticalLink`'s constructor now explicitly requests `cv::CAP_PROP_FOURCC` =
`'MJPG'`, set before the resolution request (some backends only unlock a camera's higher-
resolution modes once the pixel format is already negotiated). Unlike the resolution request
itself, this isn't made opt-in — MJPEG is the near-universal standard high-resolution capture mode
for UVC webcams specifically because of this USB bandwidth constraint, not a per-camera quirk; a
camera lacking it would already struggle to offer high resolutions in any application, native or
not. Also added the negotiated FOURCC to the existing "camera opened: resolution ..." log line (a
`set()` request can be silently ignored by a camera/driver that doesn't support it, same caveat as
the resolution request — this reports what's actually in effect, not just what was asked for).

**Verified**: `make test`, native build, and the OpenCV-enabled Windows cross-compile all
pass/build cleanly. **Not verified on real hardware** — same caveat as every camera-facing change
in this log; the negotiated `fourcc=...` log field is exactly what to check on the next real
transfer to confirm this took effect.

### Experimental multi-QR-per-frame mode, added as a runtime toggle instead of a hard swap (2026-07-05)

User asked whether showing 2+ QR codes on screen at once (instead of one at a time) could help
throughput — a real, different lever from just growing chunk size: the current display already
wastes space on widescreen monitors (it sizes to 90% of the *smaller* screen dimension, a square
area, leaving much of a 16:9 screen's width unused), so multiple QR codes could plausibly fit at
close to the same per-module pixel density as one, rather than trading density for size the way a
bigger single QR does. OpenCV's `cv::QRCodeDetector::detectAndDecodeMulti()` (confirmed present in
the bundled 4.11.0 headers) makes decoding several codes from one captured frame directly
supported, no extra dependency needed.

Then, specifically to avoid repeating the COBS experience (a hard swap that needed a full git
revert + rebuild + re-release once it broke on real hardware), the user asked for this to be two
cleanly separated code paths behind a runtime toggle, not a replacement of the working path —
exactly the pattern already used for `camera_width`/`camera_height`.

**Implementation**: `OpticalLinkConfig::qr_codes_per_frame` (default `1`, the classic, extensively
tested behavior — completely unmodified). `OpticalLink::send()`/`try_receive()` each start with a
one-line dispatch (`if (cfg_.qr_codes_per_frame > 1) { return send_multi(...)/try_receive_multi(...); }`)
and otherwise run their exact original bodies, untouched. All the new logic lives in three new
private methods (`send_multi`, `flush_send_batch`, `try_receive_multi`) that the classic path never
calls and can't be affected by:

- `send_multi()` queues each Base64-encoded QR into `pending_send_batch_`; once it reaches
  `qr_codes_per_frame`, `flush_send_batch()` lays them out in a simple square-ish grid (each QR
  independently sized to its own module count via the same per-module-pixel logic as the classic
  path, since a retransmit queued alongside fresh sends can carry an different-sized payload than
  today's chunk size) and displays them as one combined image for one dwell period.
- `try_receive_multi()` flushes any not-yet-full pending batch first (so whatever's been queued is
  actually on screen before waiting for a reply — the same invariant the classic path gets for
  free by rendering immediately), then decodes via `detectAndDecodeMulti()` and dedups against a
  *set* of hashes (`last_decoded_hashes_`) representing the current on-screen batch's content,
  since a capture can legitimately return anywhere from 0 to N decoded strings — a bad angle or
  motion blur can cost just one QR in the grid, not necessarily all of them, so each is judged
  independently rather than requiring the whole batch to match before accepting any of it. Returns
  one decoded payload per call (queuing any extras) so `try_receive()`'s existing one-frame-per-call
  contract — and therefore all of `protocol.cpp`'s Sender/Receiver logic — needs zero changes.

Exposed as `--qr-grid N` (CLI, both `send`/`receive`) and a new "QR per frame:" GUI field (default
"1"). Must be set the same on both sides of a transfer (mismatched grid sizes were not designed for
and aren't validated).

**Verified**: `make test` (the native, non-OpenCV build doesn't compile any of this at all, since
it's gated behind `QRTP_WITH_OPENCV` same as the rest of `optical_link.cpp` — confirms the classic
path's test coverage is completely unaffected), a full loopback demo transfer, and the OpenCV-
enabled Windows cross-compile (including the resized/relaid-out GUI) all pass/build cleanly.

**Not verified on real hardware** — this is a substantial, first-of-its-kind feature (spatial
rather than temporal multiplexing) with real open questions the loopback tests can't exercise
(camera field-of-view resolution across a wider capture area, whether `detectAndDecodeMulti`
behaves as reliably as `detectAndDecode` on this hardware, whether the grid layout is legible at
real camera-to-screen distances). Given it's a runtime toggle defaulting to off, the expected
failure mode this time is graceful: set `--qr-grid 1` (or clear the GUI field back to "1") to fall
back to the classic path instantly, no code revert needed — the entire point of building it this
way per the user's own request.

### Default fps raised to 24; added TCP-style fast retransmit for isolated frame losses (2026-07-05)

User request: default fps to 24 at startup, and address the real bottleneck they'd observed —
occasional missed QR frames during display refresh forcing a full retransmit-and-wait cycle that
noticeably slows the whole transfer. They floated reconstructing a partially-read/fragmented QR as
one option, and asked for other solutions to be investigated if that didn't make sense.

**Why "reconstruct a fragmented QR" isn't practical here**: QR already has Reed-Solomon error
correction built in — if `cv::QRCodeDetector::detectAndDecode()` returns a decoded string at all,
it already recovered from whatever partial corruption was within the EC level's threshold. A
completely failed decode (empty string) means either the finder patterns couldn't be located at
all, or more than the EC threshold was corrupted — and OpenCV's public API doesn't expose a
partial/soft-decision result to work with in that case (no "here's the 80% of modules I did
manage to read"). Implementing that from scratch would mean writing a custom QR decoder with
soft-decision combining across multiple failed camera frames — a research-scale undertaking, not
a reasonable scope here.

**What actually causes the described cost**: this channel already recovers from isolated losses
correctly (selective-repeat ARQ, already tested extensively) — the "slowdown" is the *cost* of
that recovery: a lost frame's own retransmit only fires once its individual RTO timer expires.
At a fast fps (esp. 24, see below), a full RTO wait is many multiples of the actual round-trip
time for what's usually just one isolated miss out of many successful frames.

**Fix, matching a well-established technique (TCP's fast retransmit, RFC 5681) instead of a novel
guess**: when a SACK arrives, if it shows the receiver already has something *ahead* of an offset
still sitting unacked in the sender's own in-flight map, that's direct evidence of a gap. TCP
requires several duplicate ACKs before acting on this because general IP networks can reorder
packets across paths; this channel can't (one SACK per DATA frame, essentially in display order),
so a single gap is already a reliable loss signal here. `Sender::send_file()` (`protocol.cpp`) now
backdates that offset's `sent_at` timestamp so the *already-existing, already-tested* RTO-timeout
retransmit path fires on the very next poll instead of waiting out a full RTO — reusing that logic
entirely rather than duplicating a second retransmit code path.

**Also**: `fps_initial`/`OpticalLinkConfig::fps` raised from 3.0 to 24.0 (`AdaptiveConfig`
default, GUI field, CLI fallback) — the AIMD's existing severity-scaled downshift still protects
this exactly as it protects any other starting value if 24 turns out too fast for a given
camera/lighting setup.

**Verified**: `make test` (including the full randomized loopback transfer), native build, and the
OpenCV-enabled Windows cross-compile all pass/build cleanly. Additionally ran the loopback demo
three more times at *higher* loss/corruption than the standard test (8%/5% vs. 5%/3%) to stress
the new fast-retransmit path specifically — all three completed correctly, byte-for-byte. **Not
verified on real hardware** for the actual throughput improvement (does fast retransmit measurably
reduce the cost of isolated real-world misses) — this change is lower-risk than the COBS attempt
since it's pure protocol logic fully exercisable via the synthetic-loss loopback tests, unlike
anything touching the camera/OpenCV decode path directly.

### COBS broke the transfer starting from the very first SYN_ACK — reverted to Base64 (2026-07-05)

**Symptom**: real user report right after the COBS switch shipped — the transfer no longer works
at all, failing from the very first SYN_ACK (the handshake's second step). This is exactly the
specific failure mode flagged as the open risk when COBS shipped: "if real hardware shows a full
decode failure ... this is the first thing to revert."

**Root cause (most likely, not directly confirmed — user reported the symptom, not a diagnostic
log)**: every previous real-hardware confirmation of the optical channel was validated with
Base64's output specifically — pure 7-bit printable ASCII (A-Z, a-z, 0-9, +, /, =). COBS's output
is genuinely arbitrary binary (any byte 0x01-0xFF, no printable-text restriction) — that's exactly
where its efficiency gain over Base64 comes from, but it means `cv::QRCodeDetector`'s internal
`std::string` construction was being asked, for the first time, to round-trip byte values well
outside anything previously exercised on real camera hardware. The project's own diagnosis of the
*original* Base64-motivating bug was specific to embedded `0x00` truncation — COBS correctly
eliminates those — but evidently something about the wider byte range itself (values >=0x80, or
some other OpenCV-internal string-handling detail) isn't safe either, on this hardware. This
remains a hypothesis, not confirmed by a diagnostic log — if COBS is revisited later, getting a
real log of exactly what `detectAndDecode()` returns for a known COBS-encoded SYN would settle it.

**Fix**: reverted to Base64 — restored `base64.hpp`/`base64.cpp` (from git history, unchanged from
before the switch), reverted `OpticalLink::send()`/`try_receive()` back to
`base64_encode`/`base64_decode`, and reverted `max_chunk_for_ec_level()`'s safety-ceiling formula
back to Base64's `4*ceil(n/3)` expansion (using COBS's much smaller overhead there while actually
encoding with Base64 would have under-counted the real per-frame cost, risking the exact
unhandled-`qr_encode_bytes()`-exception failure that safety check exists to prevent). Removed
`cobs.hpp`/`cobs.cpp`/`test_cobs()` again — git history has them if COBS is ever revisited with a
real diagnostic log to work from instead of a hypothesis. `chunk_size_max` stays at 1024 (an
independent, separate request from the same session, unaffected by the encoding scheme — SYN/
SYN_ACK don't involve chunking at all, so there's no reason to believe that value contributed to
this failure).

**Lesson reinforced**: this is the second time in this session a change flagged in advance as
"genuinely unverified, here's exactly what to check" turned out wrong on real hardware (the first
being the forced camera resolution). The explicit up-front risk disclosure is what made this a
fast, clean, one-step revert instead of a fresh diagnosis from scratch — worth continuing to do
for anything that can't be verified without a camera.

**Verified**: `make test`, a full randomized loopback transfer (byte-for-byte + hash match), native
build, and the OpenCV-enabled Windows cross-compile all pass/build cleanly, matching the exact
pre-COBS baseline.

### Replaced Base64 with COBS: ~33% more real file bytes per QR at the same chunk size (2026-07-05)

User asked for alternative techniques to embed more bytes per QR *without* enlarging chunk size —
i.e., reduce wire-format overhead rather than push chunk size (and QR density/reliability risk)
further.

**Root cause of the "waste"**: `OpticalLink::send()` Base64-encoded every frame before rendering,
solely because `cv::QRCodeDetector::detectAndDecode()` returns a `std::string` built in a way
that isn't guaranteed safe for embedded `0x00` bytes (real hardware testing established this
empirically — see the Base64 introduction entry further down this log). Base64 solves that by
restricting the output to printable ASCII text, at a real cost: ~33% expansion (`base64_len(n) =
4*ceil(n/3)`). But the actual failure mode is specifically about *zero bytes*, not binary content
in general — Base64 was solving a narrower problem with a much bigger hammer than it needed.

**Fix**: replaced Base64 with COBS (Consistent Overhead Byte Stuffing, Cheshire & Baker 1999) —
`cobs.hpp`/`cobs.cpp`, new. COBS eliminates every zero byte from arbitrary binary data at a
bounded cost of at most 1 extra byte per 254 input bytes (~0.4% worst case), a small fraction of
Base64's ~33%. Same QR capacity, same chunk size, same EC level → meaningfully more real file
bytes per frame, or equivalently a smaller/less dense (more reliable) QR for the same chunk size.
`libqrencode`'s `QRcode_encodeData` already accepts raw binary directly (unchanged) — COBS's
output just goes straight in, no intermediate text representation needed at all.

**A necessary follow-up fix, easy to miss**: `AdaptiveController::max_chunk_for_ec_level()` (added
earlier this session as a safety cap — see the chunk_size_max entry above) computed its ceiling
assuming Base64's `4*ceil(n/3)` expansion. Left unchanged, it would have under-used the real
headroom COBS provides (overly conservative, not unsafe) — updated to COBS's actual worst-case
bound (`cobs_len(n) <= n + n/254 + 1`), raising the safe per-EC-level ceiling by roughly the same
~33% (e.g. EC=L: 2191 -> 2917 bytes).

**Deliberately not implemented, with reasons**: a further, larger win exists via Base45 (or
similar) combined with `libqrencode`'s `QRcode_encodeString` entry point instead of the currently-
used `QRcode_encodeData` — QR's own Alphanumeric mode packs 2 chars/11 bits, and Base45's alphabet
was specifically designed to match it (used for exactly this reason in the EU's digital COVID
certificate QR codes), which would beat even COBS's efficiency. Not pursued this pass: it requires
adding a new dlopen'd libqrencode ABI surface (a new function signature to get exactly right across
both Linux and Windows, the same category of thing that's bitten this project before — see the
Windows port's MSVC calling-convention caveats), on top of a new encoding scheme, whereas COBS
needed neither.

**Cleanup**: removed `base64.hpp`/`base64.cpp` and `test_base64()` entirely — no longer called
anywhere (verified via full-codebase grep before deleting); git history has the old implementation
if Base64 ever needs to come back.

**Verified**: added `test_cobs()` with the same rigor `test_base64()` had — RFC-free since COBS
has no equivalent standard test vectors, but explicit zero-byte/empty/single-byte cases, the
254/255/256-byte block-boundary edge cases specifically (where COBS must split a run even without
an actual zero present), malformed/truncated-input safety (mirroring `decode_frame()`'s defensive
posture), and a round-trip of every length 0..1600 of random bytes with an explicit assertion that
the encoded form *never* contains a zero byte. `make test`, a full randomized loopback transfer
(byte-for-byte + hash match), native build, and the OpenCV-enabled Windows cross-compile all
pass/build cleanly.

**Not verified on real camera hardware** — same as the auto-max-resolution entry below, this is
new territory: every previous real-hardware confirmation of the optical channel was validated
with *Base64* output (pure 7-bit-ASCII), never genuinely arbitrary 8-bit binary. The specific,
narrower open question is whether `cv::QRCodeDetector`'s `std::string` construction is safe for
the *full* byte range (0x01-0xFF) or only empirically confirmed safe for printable ASCII — COBS is
reasoned to be safe based on the documented failure mode being specifically about zero bytes, not
a guess, but "reasoned" is not the same as "confirmed," and if real hardware shows a full decode
failure (not just slowness) rather than the expected improvement, this specific change is the
first thing to revert.

Also applied in the same pass: `chunk_size_max` lowered 1500 -> 1024 by explicit request. Combined
with the COBS switch, 1024 today produces a meaningfully smaller/less dense QR than 1024 did under
the old Base64 encoding (COBS needs far fewer QR-capacity bytes for the same chunk), so this is a
larger reliability margin than the same-looking constant provided before this pass, not merely a
repeat of an earlier value.

### User request: auto-detect the camera's highest-quality mode, and speed up AIMD ramp-up (2026-07-05)

Decoding was reported as still very slow. Two explicit asks:

**1. Camera resolution: automatic, at the highest available quality.** OpenCV's base
`VideoCapture` has no portable "list supported modes" call (that's backend-specific — e.g.
DirectShow's `IAMStreamConfig`, real COM interop this codebase otherwise avoids). Used the
common workaround instead: request an intentionally oversized resolution (10000x10000) and read
back whatever the driver actually clamps to, which is normally its true native maximum.
`OpticalLinkConfig::camera_width`/`camera_height` now default to a new `kAutoMaxResolution` (-1)
sentinel that triggers this; `0`/`0` still means "don't touch it," and any specific positive WxH
still requests exactly that (CLI `--width`/`--height`, GUI field now shows "max" instead of a
fixed "1280x720").

**Flagged directly rather than implemented silently**: this is a real, deliberate trade, not a
clear win. Real timing data from this session showed `avg_decode_ms` roughly flat across QR
*module* count (versions 3-15) but didn't rule out — and some evidence suggests — that
`cv::QRCodeDetector`'s cost tracks the *captured frame's own resolution*. Requesting the camera's
maximum quality means a bigger frame for the detector to scan on every single attempt, which could
make each decode slower even if a sharper image reduces how often decoding fails and needs
retrying (neither side of that trade is verified yet). The escape hatch (`0x0` or a specific lower
WxH) is what makes this safely reversible per machine if it turns out to hurt more than help.

**2. Speed up how fast the AIMD ramps up.** Moved two dials together instead of just one:
- *How often it's allowed to check*: `fer_window`/`fer_low` 4/0.25 -> 3/0.34 — continues the same
  halving pattern already used twice before (8/0.15 -> 4/0.25), keeping the same "~1 bad round in
  N is fine" tolerance ratio while cutting the re-qualification window further.
- *How much each upshift grows*: chunk size growth ~10%+1 -> ~20%+1 per event; fps growth
  `*1.25+0.5` -> `*1.4+0.5`; window size growth changed from a flat `+1` (which lagged chunk/fps's
  already-multiplicative climb, taking up to ~60 events to reach `window_max=64`) to the same
  proportional shape (`+25%+1`) chunk/fps already use.

Both dials were moved together deliberately — only widening the re-qualification window without
also increasing the per-event growth (or vice versa) would have been a half-measure.

**Verified**: `make test` (including `test_adaptive_aimd`, which checks growth/shrink direction
rather than exact values, so it isn't sensitive to the specific rates changed here), a full
randomized loopback transfer (byte-for-byte + hash match), native build, and the OpenCV-enabled
Windows cross-compile all pass/build cleanly. **Not verified on real hardware** for either change
— same outstanding confirmation as the rest of this arc, and this time genuinely two-sided for the
resolution change specifically (it's not obviously a win, unlike most of the other fixes here).

### Revisited chunk_size_max with the considerations it deserved this time — raised to 1500, plus a real latent bug fixed along the way (2026-07-05)

User asked whether more bytes/frame could be embedded to speed up large-file transfers — the same
lever (`chunk_size_max`) that already caused two real regressions this session (2048, then a
receiver-side fps scheme). Asked explicitly for "the right considerations" this time, so worked
through what's actually changed since the original failure, instead of re-guessing:

**Why the original 2048 attempt likely failed for a different reason than assumed at the time**:
the render loop had a real O(width²) bug back then (one `cv::Rect`+`Mat::setTo` per module, ~31k
calls for a large QR) — fixed in v19, verified 2.6-5x faster for larger QR codes specifically.
The *render* cost was very plausibly the dominant cause of "large QR = lag" reported right after
2048 shipped, not chunk size itself. Real `avg_decode_ms` data gathered since (QR versions 3-15)
also shows no strong scaling with QR size in that range — `cv::QRCodeDetector`'s cost looks tied
more to the captured frame's own size than the QR's module count. Both original reasons to avoid
a larger chunk are weaker than they looked when 2048 was reverted.

**Why not push all the way back to 2048 anyway**: no real data covers QR versions much above
~15-20 — beyond that, physical module pixels shrink enough at a fixed on-screen size to plausibly
hurt decode *reliability* even if raw decode time doesn't scale much (the v9 finding, same
mechanism as raising EC level). Untested territory, not disproven safe. Raised to 1500 instead —
the value with an actual multi-version real-hardware track record (v1-18), predating any of the
render/decode bugs found since.

**A real latent bug found and fixed along the way, independent of the chosen ceiling**:
`AdaptiveController::upshift()` grew chunk size toward `chunk_size_max` without checking whether
the *current* EC level could actually encode a payload that large. `downshift()` correctly shrinks
chunk to its floor before EC level is allowed to get worse (so chunk is always small by the time EC
reaches H) — but `upshift()` had no matching guarantee in the other direction: chunk size grows
first and EC level only improves once chunk is *already* at its ceiling, so a chunk size climbing
back up while EC level is still elevated from an earlier downshift was entirely possible. If
`chunk_size_max` (or even the previous 1024) ever exceeded what the *current* EC level's version-40
capacity allows (2953/2331/1663/1273 bytes for L/M/Q/H respectively, ISO/IEC 18004, after Base64
expansion and frame overhead), `qr_encode_bytes()` would throw — an unhandled exception, hard-
failing the transfer, regardless of what `chunk_size_max` was configured to.

**Fix**: added `max_chunk_for_ec_level()` (`adaptive.cpp`) and made `upshift()` cap chunk growth by
`min(chunk_size_max, max_chunk_for_ec_level(current_ec_level))` — chunk size can now never outgrow
what's actually encodable, and EC level only gets to improve once chunk has hit *that* ceiling
(which may be lower than `chunk_size_max` if EC is still elevated), naturally unlocking more room
on the next upshift. This makes any `chunk_size_max` value safe to try, not just this particular
one.

**Verified**: `make test` (including `test_adaptive_aimd`), a full randomized loopback transfer
(byte-for-byte + hash match, 5% loss/3% corruption), native build, and the OpenCV-enabled Windows
cross-compile all pass/build cleanly. **Not verified on real hardware** for the throughput
question itself (does 1500 actually transfer large files faster without reintroducing lag) — same
outstanding confirmation as everything else in this arc.

### Follow-up hardware/CPU sweep specifically targeting capture time (2026-07-05)

User asked, after the build-optimization + capture/decode pipelining + resolution changes, for
one more pass specifically at reducing capture time and any other CPU/GPU/thread/core lever left.
Went through the remaining candidates systematically:

**Implemented (all narrow, uniformly-safe — not hardware-negotiation-dependent like resolution
was, so not made opt-in):**
- `cap->set(CAP_PROP_BUFFERSIZE, 1)` — asks the backend to keep at most one frame queued
  internally, so `cap->read()` returns the freshest captured frame instead of dequeuing one the
  driver already had buffered. This is one of the most common, well-established fixes for "camera
  feed feels laggy/behind" in OpenCV apps specifically, and unlike the resolution request, it
  doesn't change the capture format/pixel count (where different camera hardware diverged
  sharply) — just an internal queue depth a backend either honors or silently ignores.
- Modest thread-priority bump (`THREAD_PRIORITY_ABOVE_NORMAL`) on the background `CaptureThread`
  specifically, and a modest whole-process priority bump (`ABOVE_NORMAL_PRIORITY_CLASS`) at
  `OpticalLink` construction — reduces how often the OS scheduler lets unrelated background
  activity delay a time-sensitive capture/render/decode round trip. Deliberately `ABOVE_NORMAL`,
  not `HIGH`/`REALTIME`, which can genuinely starve the rest of the system; this is the
  commonly-recommended safe middle ground for latency-sensitive desktop/media apps.

**Investigated, not pursued (with reasons, not just skipped):**
- **GPU/OpenCL for capture itself**: doesn't apply — camera capture is bound by USB transfer and
  driver-side format conversion (e.g. MJPEG decode), not parallel compute a GPU accelerates. There
  is no "GPU-accelerated frame grab" for a UVC webcam.
- **More capture threads**: wouldn't help — the bottleneck is the camera/driver/USB pipeline
  itself (fundamentally one `read()` call at a time from the OS's perspective), not CPU-side work
  our own code could parallelize further. `cv::cvtColor` (the only other per-frame work in the
  capture thread) is already cheap enough that splitting it off wouldn't move the needle.
- **Forcing a specific pixel format (FOURCC, e.g. preferring raw YUY2 over MJPEG)**: same category
  of risk as the resolution request — some cameras are *faster* with MJPEG at higher resolutions
  (raw YUY2 can exceed USB 2.0 bandwidth at 720p+, forcing the camera to only offer MJPEG there
  anyway), others may behave oppositely. Not implemented blind; if capture time is still the
  dominant cost after this pass, this is the next thing to expose as an opt-in experiment (same
  pattern as `--width`/`--height`), not a forced default.
- **`cv::wechat_qrcode`** remains the largest not-yet-pursued lever for the *decode* side
  specifically (see the previous optimization-pass entry) — unrelated to capture time, noted here
  again since it's still the natural next step if this pass isn't enough.

**Verified**: `make test`, native build, and the OpenCV-enabled Windows cross-compile all build
and pass cleanly. **Not verified against real hardware** — same caveat as every change in this
optimization arc; the actual effect on `avg_capture_ms` is the thing to check in the next log.

### Made 1280x720 the automatic default again, with manual override still available (2026-07-05)

After the previous entry explained *why* the camera defaults to 640x480 without a request, the
user clarified what they actually wanted: automatic high-resolution behavior *by default* (like
before it was reverted), plus the manual per-machine override on top — not manual-only. Both at
once, not one or the other.

**Change**: `OpticalLinkConfig::camera_width`/`camera_height` now default to `1280`/`720` instead
of `0`/`0`. The GUI's "Ris. (LxA)" field is now pre-filled with `1280x720` instead of blank; the
CLI's `--width`/`--height` flags simply override this default same as before. To fully disable
the request on a specific machine (if it turns out to be one where forcing anything backfires, as
happened in the v21/v22 saga), pass `--width 0 --height 0` or clear the GUI field to `0x0` — the
existing "only request if both > 0" check already treats that as "leave the camera alone,"
requiring no new logic, just a new default value.

This knowingly re-accepts the specific risk documented in the v21/v22 entries above (the exact
same request can help one camera and hurt another) — the user was already shown that evidence
directly and asked for the automatic-by-default behavior back anyway, so this is an informed
choice, not a re-guess. The manual escape hatch (0x0, or any other WxH) is what makes it
recoverable per-machine if needed, rather than an unconditional, un-overridable default like the
original v21 attempt was.

**Verified**: `make test`, native build, and the OpenCV-enabled Windows cross-compile all build
and pass cleanly.

### After making resolution opt-in, the camera always opens at 640x480 — not a bug, OpenCV/DirectShow's own conservative default (2026-07-05)

**Symptom**: user report right after the resolution field became opt-in (blank = don't touch
anything) — the camera now consistently opens at 640x480 "automatically."

**Investigated, not a bug**: re-read the GUI/CLI parsing logic carefully — when the field/flag is
left blank, `camera_width`/`camera_height` both stay `0`, and `OpticalLink`'s constructor only
calls `cap->set(CAP_PROP_FRAME_WIDTH/HEIGHT, ...)` when *both* are `> 0`. No path forces 640x480.
What's actually happening: OpenCV's DirectShow backend (`cv::CAP_DSHOW`, used on Windows) is
well known to negotiate a conservative default resolution — often exactly 640x480 — when no
format is explicitly requested, *regardless of what the camera actually supports*. This was
always true, on every previous version too; it only became *visible* once the "camera opened:
resolution ..." log line was added in v21, so it looks new but isn't. It's also consistent with
already-seen evidence: in the v21 test, the sending machine's camera *did* successfully negotiate
1280x720 the moment it was explicitly requested — proof the camera itself isn't limited to
640x480, OpenCV just doesn't reach for anything higher on its own.

**No code fix needed** — this is exactly the scenario the opt-in `--width`/`--height` (CLI) /
"Ris. (LxA)" (GUI) fields exist for. Clarified the CLI help text and the GUI field's code comment
to say this explicitly, so the next person hitting the same "why is it always 640x480" question
finds the answer already written down.

### General CPU/hardware optimization pass: unoptimized build discovered, capture/decode pipelined across threads, camera resolution made opt-in (2026-07-05)

User asked for a broader sweep: anything else optimizable at the OpenCV/CPU/GPU/hardware level to
minimize lag, now that rendering, grayscale conversion, and chunk size have each been addressed.
Investigated systematically before changing anything:

**1. This project's own build had zero compiler optimization the entire time.** `CMakeLists.txt`
never set `CMAKE_BUILD_TYPE`, so every Windows build from v14 through v22 compiled with bare
`-std=c++17` — no `-O2`/`-O3` at all. Checked whether OpenCV itself (where the actual decode
bottleneck lives) had the same problem: it didn't — its own build tree has `CMAKE_BUILD_TYPE=Release`
and `opencv_objdetect` (the module `cv::QRCodeDetector` lives in) compiles with
`-O3 -DNDEBUG -msse3` confirmed directly from its `flags.make`. So this fix doesn't touch the
confirmed decode bottleneck, but it's an unambiguous, zero-risk correctness gap regardless — fixed
by defaulting `CMAKE_BUILD_TYPE` to `Release` when the invoker doesn't set one.

**2. GPU/OpenCL — investigated, not pursued.** The bundled OpenCV does have `WITH_OPENCL=ON` /
`HAVE_OPENCL=1` (T-API/UMat dispatch is available), but `cv::QRCodeDetector`'s finder-pattern
search and decode are not known to have an OpenCL-accelerated path in this OpenCV version — passing
it a `cv::UMat` would very likely just add a GPU-readback round-trip for no benefit, since the
render (`cv::resize`) and grayscale conversion (`cv::cvtColor`) steps that *could* use it are
already down to sub-millisecond cost and aren't where the time goes. Not implemented: no credible
evidence it would help the actual bottleneck, only the confirmed-cheap steps.

**3. A genuinely faster QR detector exists (`cv::wechat_qrcode`, OpenCV's contrib module) but was
not pursued this pass** — it requires rebuilding OpenCV with an additional module plus shipping
neural-net model files alongside the exe, meaningfully increasing both build complexity and the
distributed package size, and cannot be verified from this dev environment (no camera at all).
Left as a documented option for later if the pipelining below turns out not to be enough.

**4. Capture and decode were sequential in the same thread — pipelined across two.** Real timing
data showed `avg_capture_ms` (up to ~45ms) and `avg_decode_ms` (~10-90ms) as two comparable,
*sequential* costs in `try_receive()`'s loop: capture, then decode, one full cycle at a time. Added
a background `CaptureThread` (`optical_link.cpp`) that continuously runs `cap->read()` + the
grayscale conversion on its own thread, publishing the latest frame (with a generation counter)
behind a mutex/condition-variable; `try_receive()` now just picks up whatever's freshest instead of
blocking on the camera read itself. On any multi-core CPU (virtually all modern hardware), this
overlaps capture time behind decode time instead of paying both back-to-back, which should reduce
the effective per-attempt cost toward `max(capture, decode)` rather than their sum.

This is the highest-risk change in this entire optimization arc — it's the first one that touches
threading in code with a hard-won debugging history specifically around HighGUI/Win32 window
thread-affinity (see the v3 and v6 entries below). Mitigated deliberately: **every** `cv::imshow`/
`cv::waitKey`/`cv::namedWindow` call still happens only on the thread that owns the `OpticalLink`
(unchanged from before) — the new background thread *only* ever touches `cv::VideoCapture::read()`
and `cv::cvtColor()`, neither of which is a windowing call with a thread-affinity requirement.
`std::mutex`/`std::condition_variable` hand-off of a `cv::Mat` between threads relies on `cv::Mat`'s
own reference-counted copy semantics (a shallow copy taken under the lock stays valid after the
lock is released and the source is reassigned — standard, well-understood behavior, not something
new being invented here). Verified: compiles cleanly (native + OpenCV-enabled Windows
cross-compile), `make test` passes. **Not verified against a real camera** — no way to do that from
this dev environment — so real hardware confirmation that this doesn't introduce a new freeze/crash
is the outstanding item, on top of confirming the expected speedup.

**5. Camera resolution made opt-in instead of automatic** (per explicit request): `OpticalLinkConfig`
gained `camera_width`/`camera_height` (both 0 = default, don't touch the camera's own resolution).
`qrtp send`/`qrtp receive` gained `--width`/`--height` CLI flags; the GUI gained a "Ris. (LxA)"
field (blank = auto, or e.g. "1280x720"). Deliberately not defaulted to any specific value — the
prior attempt at forcing one automatically for everyone backfired on one of the two test machines
(see the entry above), so this is left as an explicit, per-machine, opt-in experiment rather than a
default this software imposes.

### The forced 1280x720 camera resolution request helped one machine and hurt the other — reverted (2026-07-05)

**Data** (real two-machine log, same transfer, both sides logging "camera opened"): the sending
machine's camera negotiated exactly what was requested (`1280x720`) and got much faster captures
(`avg_capture_ms` down to 0.5-1.5ms). The *receiving* machine's camera **ignored the requested
height** and snapped to a different native mode (`1280x1024` — more total pixels than requested,
not fewer), and its `avg_decode_ms` came back roughly **4-5x worse** (56-91ms) than the ~10-19ms
baseline from before this request existed. Overall transfer time (33.6s) was actually slightly
*better* than the prior run despite this, most likely because retransmit/AIMD variance between
real runs is large enough to mask a single machine's decode regression — but the receiver's own
decode numbers are unambiguous and directly comparable to its own prior baseline.

**Conclusion**: forcing a specific capture resolution is camera/driver-dependent in ways that can't
be predicted or tested from this dev environment (no camera hardware available here at all) —
some cameras have a fast native mode near the request and handle it well; others fall back to a
different, sometimes *worse*, mode entirely when the exact request can't be met. This is the third
time in this session a proactive tuning attempt reasoned soundly from data still needed reverting
once tested against real, asymmetric hardware (after the receiver-side fps controller and the
fps-mirroring attempt) — same lesson each time: verify before generalizing a fix across two
machines that don't necessarily behave the same.

**Fix**: removed the `cap->set(CAP_PROP_FRAME_WIDTH/HEIGHT, ...)` calls entirely. Kept the
diagnostic log line (now just reporting whatever resolution the camera's own default turns out to
be, via `cap->get(...)`) since that's harmless, useful information regardless of whether a
resolution is ever forced again.

**Verified**: `make test`, native build, and the OpenCV-enabled Windows cross-compile all build
and pass cleanly.

### Real diagnostic log confirmed decode time (~10-19ms/attempt), not rendering, is now the dominant cost — requested a capped camera capture resolution (2026-07-05)

**Data** (real user log, 13110-byte transfer, ~38s): `encode_ms` 0.2–2ms, `render_ms` 0.2–0.7ms
(both confirm the previous rendering fix worked — negligible now), `avg_capture_ms` widely variable
(0.2–17ms), and **`avg_decode_ms` consistently 10–19ms per `detectAndDecode()` call, across QR
versions 3 through 12 with no strong size-dependence visible in this range** (version 3, 29×29
modules: ~12–18ms; version 12, 65×65 modules: ~12–19ms). That last point is the key finding: decode
cost not scaling much with the QR's own module count suggests the dominant cost is scanning the
*whole captured camera frame* for finder patterns, not decoding the QR's payload once found — which
points at camera capture *resolution* rather than QR density as the next lever, a materially
different conclusion than "make the QR smaller again."

**Change**: `OpticalLink`'s constructor now requests `1280x720` from the camera
(`cv::CAP_PROP_FRAME_WIDTH`/`HEIGHT`) right after opening it, and logs the actually negotiated
resolution (`cap->get(...)`, since a `set()` request can silently be ignored by an unsupported
camera/backend — this reports reality, not the request). Many webcams default to their highest
supported resolution (1080p or above) when nothing is requested; asking `detectAndDecode()` to scan
a smaller frame is a standard, low-risk technique in barcode/QR-scanning applications, and 720p
still leaves comfortable margin to resolve module edges at typical camera-to-screen distances for
the QR versions actually seen in this transfer (up to ~65×65 modules).

**Not yet confirmed** — this is reasoned directly from the user's own timing data (not a blind
guess like the earlier chunk_size_max/fps-mirroring attempts), but the actual before/after
`avg_decode_ms` on real hardware, and the negotiated-resolution log line itself, are the two things
to check on the next transfer. If 720p turns out to already be at or below what this camera
natively provides, the log will show that directly and this change will be a no-op rather than a
guess masquerading as a fix.

**Verified**: `make test`, the native build, and the OpenCV-enabled Windows cross-compile all build
and pass cleanly.

### Down to 35s from 97s after the rendering/grayscale fixes — added real timing diagnostics instead of guessing the next tuning constant (2026-07-05)

**Context**: user confirmed the render + grayscale fixes helped a lot (97s → 35s) and believes the
remaining bottleneck is entirely in the optical QR processing (render/capture/decode). That's a
reasonable hypothesis given the history here, but it's still a guess — every previous *blind* guess
in this specific area (chunk_size_max twice, the receiver-side fps mirroring twice) turned out
wrong or made things worse once real hardware evidence came in. Rather than pick another constant
to adjust without data, added the measurements needed to know which specific piece (QR encoding,
rendering, camera capture, or `cv::QRCodeDetector`'s decode) is actually dominant.

**Added**:
- `OpticalLink::send()` now logs `encode_ms` (the `qr_encode_bytes`/libqrencode call) and
  `render_ms` (the 1px/module build + `cv::resize`) alongside the existing per-frame "showing QR"
  log line.
- `OpticalLink::try_receive()` now accumulates capture time (`cap->read`) and decode time
  (`detectAndDecode`) across every polling attempt since the last successful read, and logs the
  averages (`avg_capture_ms`, `avg_decode_ms`, over `polls_since_last` attempts) on the existing
  "camera read a NEW QR" log line — the only already-non-spammy log point in that hot loop, so this
  adds real signal without adding new log volume (relevant after the v10 GUI-log-overflow bug).

**Also**: throttled the local camera-preview `imshow()` in `try_receive()` to every 3rd captured
frame — it's a usability aid (aiming the camera), not something decoding depends on, and imshow-ing
every single frame from a fast camera is avoidable render cost in the same hot loop. `waitKey` and
`detectAndDecode` still run on every captured frame (unchanged cadence), so this doesn't touch
either the decode-latency path or the v3 anti-freeze fix (which depends on `waitKey` pumping
messages every iteration, not on `imshow` specifically).

**Next step**: once the user shares the next log with these fields populated, the actual split
between render/capture/decode cost will be visible directly instead of inferred — that split
determines whether the next lever is `chunk_size_max` again, camera capture resolution, or
something in the decode path, instead of guessing among them.

**Verified**: `make test` and the native (non-OpenCV) build pass; the OpenCV-enabled Windows
cross-compile builds clean with these changes. Not verified against a real camera — there's no way
to do that from this dev environment — so the timing numbers themselves are unconfirmed until the
user's next real-hardware run.

### Large QR codes visibly lagging the whole app — a real O(width²) rendering cost, fixed directly instead of just tuned around (2026-07-05)

**Symptom**: user report — large QR codes specifically make the whole app laggy; smaller QR codes
run noticeably smoother. Previously addressed only indirectly, by lowering `chunk_size_max` (which
lowers how large a QR *can* get, but does nothing about the actual cost of rendering/decoding one
when it does get large).

**Root cause (rendering side, now fixed)**: `OpticalLink::send()` drew the QR by looping over every
module and, for each dark one, constructing a `cv::Rect` and calling `Mat::setTo()` on that
sub-region — for a version-40 QR (177×177 modules) that's up to ~31,000 individual OpenCV calls
*per displayed frame*, cost growing with the square of the QR version. This is exactly the kind of
cost that would show up as "bigger QR = more lag" independent of anything about window size, chunk
size, or the adaptive controller's tuning.

**Fix**: build the QR at 1 pixel/module first with a plain pointer-write loop (no OpenCV call per
module), then a single `cv::resize(..., INTER_NEAREST)` scales it up to the on-screen size.
Verified via a standalone script (numpy/cv2) that this produces **byte-identical output** to the
old per-module fill across QR widths 21–177 and scales 4–13 (nearest-neighbor upscaling by an exact
integer factor always maps each destination pixel back to exactly one source module pixel — same
sharp block edges, not an approximation). Benchmarked 1.2×–5.1× faster depending on QR size (bigger
win for bigger QR codes, as expected since the fixed per-call overhead that scaled with module count
is what got removed).

**Also addressed (decode/preview side)**: `try_receive()` fed the camera's full 3-channel color
frame directly to both the live preview (`cv::imshow`) and `cv::QRCodeDetector::detectAndDecode()`
on every polled frame. QR content is pure black/white, so nothing downstream needs color channels —
converted to grayscale once right after capture and reused for both, on a smaller (1-channel)
image, in a loop that polls continuously. This doesn't eliminate whatever color conversion the
camera backend itself does internally to produce a BGR frame in the first place (backend/driver
territory, out of reach without hardware to test against), but does guarantee we're not also
letting the detector redundantly re-derive grayscale from a full color frame on top of that.

**Additionally**: `chunk_size_max` lowered from 1500 to 1024. The rendering fix addresses the CPU
cost of *drawing* a large QR; it does nothing about the cost or reliability of *decoding* one
(`cv::QRCodeDetector`'s internals are an opaque library dependency) — that side still gets more
expensive and less reliable as QR version climbs. Lowering the reachable ceiling is the direct lever
available for that half of the problem.

**Not yet confirmed by the user** — the rendering equivalence and speedup are verified in isolation
(script + benchmark above, not this app's actual camera/display loop), and `make test` plus a full
loopback demo transfer still pass, but real improvement on real hardware is the outstanding
confirmation, same as every constant/behavior change in this log.

### The "fix" for the previous regression made throughput twice as bad — mirroring the sender's fps onto the receiver defeats a manually-configured fast receiver (2026-07-05)

**Symptom**: real user report on the v17 build (rate-mirroring fix) — throughput got *worse*, not
better: 97s, up from the already-bad 47.24s the mirroring was meant to fix.

**Root cause**: v17 made every `DATA` frame carry the sender's current fps/EC level and had the
receiver apply it directly to its own reverse (SACK) channel. This looked strictly safer than the
v14 approach (reusing a rate the forward channel had already proven, instead of a separate guess)
— but it missed something more basic: the *sender's* fps starts low (`fps_initial=3.0`) and only
climbs gradually via AIMD over many clean rounds (~12 upshift events to approach `fps_max`). Before
any of this optimization work existed, the receiver's reverse channel ran at whatever fps the user
configured for *that side* — completely independent of the sender's ramp-up curve. If the user set
a high fps on the receiving side too (as this user had — see the "48 vs 60" report), that channel
used to run fast the *entire* time. Mirroring throws that away: it forcibly holds the receiver's
channel down to match the sender's own (still-ramping, possibly never-fully-ramped for a short
file) fps, on every single frame — actively defeating a manually-configured fast receiver rather
than helping a slow one. For a small file that completes before the sender's AIMD ever reaches its
ceiling, this makes the receiver strictly slower than before the "fix," which matches the reported
2x regression.

**Fix**: removed the mirroring entirely (`encode_rate_hint`/`decode_fps_hint`/`decode_ec_hint` and
all call sites) along with the v14 independent-controller approach it had replaced. `Receiver` is
now back to exactly its pre-optimization-pass behavior: its link's fps/EC level is whatever it was
constructed with (the user's own `--fps`/GUI setting for that side), untouched by anything the
sender reports. Two consecutive attempts at automatically tuning the reverse channel's speed each
made real-hardware throughput measurably worse — the safe, evidence-respecting move is to stop
touching this dimension automatically and leave it under direct user control, exactly as it was
before any of this session's throughput work started.

**Practical implication for the user**: since the reverse channel no longer self-tunes, make sure
the *receiving* side's `--fps` (CLI) or FPS field (GUI) is also set high if fast transfers matter —
setting it only on the sending side no longer has any effect on how fast SACKs come back.

**Verified**: `make test` and a full loopback demo transfer (byte-for-byte + hash match) both pass.
`protocol.hpp` is now byte-identical to its pre-optimization-pass state (commit `9afdf53`); the only
remaining diff in `protocol.cpp` against that baseline is the unrelated size/duration logging added
separately (and not implicated in either regression).

### Still very slow after the chunk_size_max revert — 13110 bytes in 47.24s at fps=48 despite fps_max=60 — the receiver's own reverse-channel AIMD was penalizing itself for forward-channel loss (2026-07-05)

**Symptom**: real user report on the v16 build (chunk_size_max already reverted to 1500) — a
13110-byte transfer took 47.24s, with the sender's fps only reaching 48 despite `fps_max=60` being
configured. ~277 bytes/sec is far too slow to be explained by forward-channel display rate alone.

**Root cause**: the v14 fix gave `Receiver` its own `AdaptiveController` driving its own SACK
display fps/EC, fed by treating any incoming `DATA` frame's `RETRANSMIT` flag as a "loss event" for
the *reverse* channel (severity defaulting to a full 1.0 — the maximum pullback the class supports).
This conflated two different things: a `DATA` retransmit fires whenever the *sender's* RTO expires
without an ACK, which happens just as often (in practice, more often) because the original `DATA`
frame itself never reached the receiver at all — pure forward-channel loss, with nothing to do with
whether the receiver's own SACK replies are getting through. Every such event still slammed the
reverse channel's fps down by a full `*0.8`, and recovery required several consecutive genuinely
clean SACK rounds (`fer_window=4`) to earn back — on a real, moderately lossy optical channel this
meant the reverse channel spent much of the transfer near its `fps_min` floor (1.0, i.e. ~1 full
second per SACK), regardless of how well the forward channel was actually doing. Every `DATA` frame
requires exactly one SACK reply, so a reverse channel parked near 1fps caps the *entire* transfer at
roughly one round trip per second — consistent with the reported numbers.

**Fix**: removed the receiver's independent `AdaptiveController` entirely (was solving a real
problem — the reverse channel needing to speed up — with a signal too weak and confounded to trust).
Replaced it with direct mirroring: every `DATA` frame now piggybacks the sender's *current* fps/EC
level in fields that carry no other meaning for that frame type (`ack_window` as `fps*100`
fixed-point; the unused upper 2 bits of `flags_byte` for EC level) — see `encode_rate_hint`/
`decode_fps_hint`/`decode_ec_hint` in `protocol.cpp`. The receiver applies these directly to its own
link the instant a `DATA` frame arrives, before replying with a SACK. This reuses a rate the forward
channel has *already just proven* works on this exact camera/lighting setup, for a channel carrying
a smaller payload besides — no separate inference, no risk of the reverse channel being penalized
for a forward-channel problem it had no part in.

**Not yet confirmed by the user** — reasoned from the reported numbers and the confounded-signal
analysis above, not from fresh diagnostic logs. `make test` and a full loopback demo transfer
(byte-for-byte + hash match) both pass after the change.

### Throughput got much worse (not better) after raising chunk_size_max to 2048 — capacity math ignored real decode/render cost (2026-07-05)

**Symptom**: real user report after the v14 "optimize throughput" pass — performance dropped a lot
instead of improving, and specifically: large QR codes make the program lag, small/medium QR codes
perform best.

**Root cause**: `chunk_size_max` had been raised from 1500 to 2048 bytes on the reasoning that a
version-40 QR at EC=L has the raw byte capacity for it (`base64_len(chunk + 23) <= 2953` per
ISO/IEC 18004). That math checks whether the data *fits* in a symbol — it says nothing about how
long that symbol takes to actually render (more modules to draw) or, far more importantly, decode
(`cv::QRCodeDetector::detectAndDecode()` on a much denser image) on real camera/CPU hardware. This
is the same category of mistake as the v9 finding ("raising EC level for a fixed chunk size shrinks
module pixels, making decode *harder* not easier") — a constant that looked safe by a structural/
capacity argument turned out to be dominated by a real-world cost the math didn't model at all.

**Fix**: reverted `chunk_size_max` to 1500 (`include/qrtp/adaptive.hpp`) — the last value with
confirmed-working real-hardware behavior behind it, per the user's direct A/B observation (worse
with large QR, better with small).

**Open question, not yet addressed**: `AdaptiveController::upshift()` decides to grow chunk size
based purely on rolling frame-error-rate (`rolling_fer()`) — it has no visibility into how long each
round trip actually took, even though `Sender` already feeds real RTT samples into
`on_rtt_sample()`. A QR that's large enough to decode slowly but still successfully (i.e., FER stays
low) would keep getting grown by the current logic even at whatever ceiling is configured, since
"slow but correct" and "fast and correct" look identical to a purely error-rate-based signal. If
lag reappears even at chunk_size_max=1500 on some hardware, the next lever to try is feeding RTT
magnitude (not just pass/fail) into the upshift decision, not just raising/lowering the ceiling
again.

### Throughput alternated between fast bursts and slowdowns after the v12 tuning pass — AIMD sawtooth from a flat, maximal decrease on every loss (2026-07-05)

**Symptom**: after the v12 fix (fps ceiling raised to 60, faster upshift), the user reported
transfers speeding up in short bursts, then slowing down, then speeding up again — repeating.
Classic AIMD "dente di sega" (sawtooth) behavior.

**Root cause**: `AdaptiveController::record_loss_event()`/`downshift()` treated every retransmit
round identically, regardless of how much of the in-flight window actually timed out. A single
straggler frame out of a window of, say, 20 in-flight frames triggered the exact same maximal
multiplicative decrease as the entire window vanishing at once: window halved, chunk size shrunk by
~1/3, fps multiplied by 0.8. Combined with v12's faster, more aggressive upshift, the controller
would climb quickly, hit one isolated hiccup, crash hard, then have to climb all the way back up
again — the sawtooth the user described.

**Fix**: added a `severity` parameter (0.0–1.0) to `record_loss_event(double severity)` and
`downshift(double severity)`, equal to the fraction of the currently in-flight window that timed
out in that round. `Sender::send_file()` computes it in the retransmit branch as
`timed_out.size() / inflight.size()`, captured before the retransmit loop below it starts
splitting/erasing entries. The window shrink, chunk-size shrink, and fps pullback all now scale
with `severity` (full old behavior at severity=1.0, near-zero pullback for an isolated single-frame
timeout in a large window). EC-level escalation — which v9 showed can backfire by shrinking QR
module pixels for a fixed chunk size — is now gated behind `severity >= 0.5` instead of firing on
every single timeout.

**Not yet re-confirmed by the user on real hardware** — reasoned from the AIMD sawtooth pattern
described in their report, not from fresh diagnostic logs. `make test` (including
`test_adaptive_aimd`, which still calls `record_loss_event()` at its default `severity = 1.0` and
continues to pass) and the full loopback suite both pass after the change; a real transfer showing
smoother throughput is the outstanding confirmation.

### A completed transfer reported a hash mismatch — stale duplicate of a since-split offset silently corrupted reassembly (2026-07-05)

**Symptom**: a real transfer ran all the way to `all N bytes acked, tearing down` on the sender
side — every byte offset got a confirmed ack — but the receiver reported `HASH MISMATCH`. All the
data arrived; the reassembled content was wrong anyway.

**Root cause**: introduced by the offset-splitting fix above. When a stuck offset gets split into
smaller pieces, the *first* sub-piece reuses the exact same starting byte offset as the original
(unsplit) frame — it has to, since that's genuinely where that piece of the file begins. If a
stale/delayed copy of the *original* (larger) frame is still captured by the camera *after* the
sender has already moved on to displaying the smaller replacement pieces (plausible: camera driver
buffering, or the display not having visually updated yet at the exact instant of the capture),
`chunks[offset] = f->payload` blindly overwrote the already-correct smaller piece with the stale
larger one. No CRC catches this — the stale frame is perfectly well-formed and internally
consistent, just outdated relative to the sender's current (finer) chunking decision. The final
reassembly then concatenated the wrong bytes at that position, corrupting the whole file's hash
despite every offset technically being "acked."

**Fix**: the receiver now rejects two categories of stale data for `DATA` frames:
1. Anything at a byte offset already folded into the confirmed cumulative prefix (that data is
   locked in and must never change again).
2. For any offset not yet confirmed: if something is already stored there and a *new* arrival is
   **larger** than what's already stored, reject it. Re-chunking only ever produces smaller pieces,
   never grows one back — so a larger arrival after a smaller one is definitionally a stale
   straggler, never a legitimate update.

**Not yet re-confirmed by the user** for a large file specifically (this was diagnosed from a log
where the corruption's exact origin — the `splitting stuck offset 43841` event — aligned with where
the receiver's log excerpt didn't show corresponding activity, consistent with but not 100%
pinpointed to this exact race by direct before/after comparison). The fix is verified sound by
exhaustive manual reasoning through the offset-locking + prefer-smaller interaction (see the commit
message), and the full randomized end-to-end loopback test continues to pass, but a dedicated
regression test reproducing this exact race directly would be a good addition — not done yet, since
it requires hand-driving frame sequencing past the normal Sender/Receiver API.

### First confirmed successful end-to-end transfer (small file) — but large files appear to freeze partway through (2026-07-05)

**Good news first**: with the offset-splitting fix, the user completed a full, successful transfer
of a small file — the protocol genuinely works end-to-end on real Windows hardware now.

**Symptom**: large files take a very long time, and at some point *both* sender's and receiver's
logs stop showing any new activity — looks like a freeze, on both sides simultaneously.

**Root cause (log freeze specifically)**: the GUI's log `EDIT` control never had `EM_LIMITTEXT`
raised, so it was silently capped at Win32's default ~32K-character limit. The verbose diagnostics
added a few commits back (logging every single QR shown and every camera read) generate a lot of
text for a large file — easily enough to hit that default limit partway through a long transfer,
at which point the control just stops accepting more text. The underlying transfer was most likely
still running; the log display had just stopped reflecting it, on *both* machines independently
(same GUI code, same bug, same default limit, hit at roughly the same point since both sides log at
a similar rate).

**Fix**: `EM_LIMITTEXT` raised to the control's maximum at creation, plus periodic trimming (drop
the oldest third once the log passes 200K characters) so the control doesn't just grow forever and
get progressively slower to update on a very long transfer either.

**Separately, not yet addressed**: large files genuinely being slow. This needs real data (a log
excerpt from a large-file attempt, now that the log won't silently stop) before tuning anything
further — enough AIMD/rendering parameters have already turned out to have non-obvious, sometimes
counterproductive interactions (see the entries above) that changing more of them without evidence
would just be more guessing.

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
