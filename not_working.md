# Known Issues / Debug Log

Living log for real failures — paste actual error output/tracebacks here as they happen, with root
cause and fix once found, so the same failure mode doesn't get re-diagnosed from scratch next
session.

## Resolved

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
