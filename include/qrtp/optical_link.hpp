#pragma once
#ifdef QRTP_WITH_OPENCV

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "qrtp/link.hpp"
#include "qrtp/qr_encoder.hpp"

namespace qrtp {

struct OpticalLinkConfig {
    int camera_index = 0;
    // Reverted 24 -> 3 (see AdaptiveConfig::fps_initial for the full reasoning — starting fast
    // skips the "prove the channel first" step the AIMD is built around, especially risky right
    // at the handshake before the camera has settled focus/exposure at all). Matches
    // AdaptiveConfig::fps_initial — this is only the value the link is constructed with before any
    // Sender/Receiver ever calls set_fps(); the AIMD takes over from there.
    double fps = 3.0;
    QrEcLevel ec_level = QrEcLevel::M;
    // Defaults to a fixed 1280x720 as of 2026-07-05, per real hardware testing across two separate
    // machines that identified this (together with MJPEG on, autofocus left on, and the fps ramp-up
    // starting from 3 — see AdaptiveConfig::fps_initial) as the best-performing combination found so
    // far. This is a real, deliberate departure from the previous default (kAutoMaxResolution,
    // requesting the camera's own highest available mode automatically): an earlier entry in
    // not_working.md documented the exact same auto-max request negotiating cleanly to 1280x720 on
    // one machine but to a different, worse-performing mode (1280x1024) on another — so "auto-max"
    // itself was never a guaranteed-safe default, just an automatic one. 1280x720 is a genuine
    // per-camera preference from real testing, not a universal law; still fully overridable — set
    // both to kAutoMaxResolution to opt back into auto-detecting the camera's highest mode, both to
    // 0 to leave the camera alone entirely (its own default, without any request), or any other
    // positive WxH for a specific mode. The actual negotiated size a camera settles on (which can
    // still differ from any of these) is always logged, never just assumed.
    static constexpr int kAutoMaxResolution = -1;
    int camera_width = 1280;
    int camera_height = 720;
    // Was briefly made false-by-default (2026-07-05) on the theory that JPEG compression artifacts
    // might be degrading QR module edges enough to explain a decode-reliability problem seen in a
    // real log. Reverted back to true after a direct controlled A/B on the *same* problematic
    // transfer: with this off, avg_capture_ms measured 103-111ms per cap->read() call, consistently,
    // regardless of how many polls the average was over (i.e. a real per-call cost, not an
    // average distorted by outliers) — versus <1.5ms with it on. That's the exact symptom the
    // original USB-bandwidth reasoning predicted (raw YUY2 at this resolution saturating what USB/
    // the driver can sustain per frame), now directly confirmed rather than just plausible. Meanwhile
    // the reliability problem this was suspected of causing (stuck offsets, cascading retransmits)
    // showed up identically in both the MJPEG-on and MJPEG-off logs, and total transfer time did not
    // improve with it off (if anything, slightly worse) — so the JPEG-artifact theory is the one that
    // didn't hold up, not the bandwidth one. Left as a runtime toggle (not hard-coded back to
    // unconditional) since the resolution request below already demonstrated real cameras can
    // disagree about what capture mode works best for them — this just changes which side of that
    // toggle is the default given the evidence so far.
    bool request_mjpeg = true;
    // Opt-in, default false (2026-07-05): a real log showed avg_capture_ms and avg_decode_ms
    // dropping together sharply (~103-134ms -> ~27-40ms) exactly when the transfer fell into a
    // low-fps single-offset retransmit loop after a string of stuck-offset splits, then staying low
    // even after fps climbed back up — on a two-separate-machines setup (confirmed with the user),
    // which rules out CPU contention between the sender and receiver process sharing one machine's
    // cores as the explanation. cap->read() latency shouldn't depend on our chunk/window/fps choices
    // at all if it's purely camera-hardware-bound, so a correlation this tight with our own fps
    // transitions instead points at the *camera's own* auto-exposure/autofocus control loop hunting
    // under rapidly-changing on-screen content (different QR codes flashing by at higher fps shift
    // average scene brightness/contrast quickly) — a well-documented source of variable capture
    // latency in UVC webcams. Untested here (no camera hardware in this dev environment): when true,
    // requests CAP_PROP_AUTOFOCUS=0 right after opening the camera, disabling continuous refocusing
    // for the rest of the session. Deliberately narrower than also touching exposure: a wrong manual
    // exposure value picked blindly could make the picture too dark/bright on an unknown camera,
    // whereas autofocus off is a simple on/off UVC control with no numeric value to get wrong — if
    // this doesn't help, exposure is the next, riskier knob to consider.
    bool disable_autofocus = false;
    // Optional diagnostics from the optical layer itself (distinct from ProtocolConfig::log):
    // whether the camera is detecting any QR content at all, and whether decoded text survives
    // frame validation. Wire it to the same sink as ProtocolConfig::log to see both in one place —
    // essential for telling "camera never sees a QR" apart from "sees one, gets rejected."
    std::function<void(const std::string&)> log;
    // Classic default: exactly one QR per displayed frame — the original, extensively real-
    // hardware-tested behavior, completely untouched by this option. >1 batches that many
    // send()'d frames into a single on-screen grid (e.g. 2 shows side by side) and decodes
    // multiple QR codes from one captured frame via cv::QRCodeDetector::detectAndDecodeMulti() —
    // an experimental, opt-in mode (see not_working.md, 2026-07-05). Deliberately a runtime
    // toggle rather than a hard swap of the existing path, specifically so it can be turned off
    // again (set back to 1) without needing a code revert if it doesn't hold up on real hardware
    // — see send_multi()/try_receive_multi() in optical_link.cpp, which are the *only* code this
    // touches; send()/try_receive() run the exact original code, unmodified, whenever this is 1.
    int qr_codes_per_frame = 1;
    // Opt-in, default false — untested on real camera hardware (2026-07-05). When true, decodes
    // using cv::wechat_qrcode::WeChatQRCode (opencv_contrib: a small SSD-style detector CNN plus a
    // super-resolution enhancement step for small/blurry crops) instead of the classic
    // cv::QRCodeDetector. Chosen over an alternative under consideration (zxing-cpp, a separate
    // third-party library) specifically because it adds no new dependency or build pipeline: it's a
    // module of the same OpenCV this project already links against, and the Windows cross-compile's
    // system mingw64-opencv package already ships libopencv_wechat_qrcode + its libopencv_dnn
    // dependency. The classic path (this flag false) is completely untouched — see
    // wechat_qrcode_model_dir below for the 4 model files this needs that OpenCV itself doesn't
    // bundle. Motivation: this project's whole session of real-hardware logs pointed at decode
    // *reliability* (frames that never resolve, driving retries/downshifts), not raw per-attempt
    // decode cost, as the dominant remaining bottleneck — a stronger detector attacks that directly,
    // even though each attempt likely costs somewhat more CPU than the classical approach.
    bool use_wechat_qrcode = false;
    // Directory containing detect.prototxt, detect.caffemodel, sr.prototxt, sr.caffemodel (from
    // github.com/WeChatCV/opencv_3rdparty, branch wechat_qrcode — the canonical source
    // opencv_contrib's own wechat_qrcode module points at). Empty (default) resolves to a
    // "models/wechat_qrcode" folder next to the running executable, matching how libqrencode.dll
    // is expected next to qrtp.exe (see qr_encoder.cpp) — set explicitly to point elsewhere.
    std::string wechat_qrcode_model_dir;
};

// The real physical layer: send() renders a QR frame to an on-screen window; try_receive() polls
// the camera and decodes any QR code currently visible to it, de-duplicating repeated reads of a
// frame that hasn't changed yet. Requires OpenCV — build with `make WITH_OPENCV=1` after
// installing opencv-devel. Not exercised by this project's automated tests (those run against
// LoopbackLink), since no OpenCV/camera is available in the environment this was written in.
//
// Two separate windows, deliberately: both Sender and Receiver call send() (for DATA/handshake or
// for ACKs/SACKs) *and* try_receive() (waiting for the other side) on the very same OpticalLink,
// often interleaved in a tight loop. A single shared window would have try_receive()'s camera
// preview overwrite the outgoing QR within milliseconds of it being shown, since try_receive()
// polls far more often than send() is called — leaving the other machine's camera only a ~100ms
// window per attempt to catch it, rather than the frame staying visible until the next real send.
class OpticalLink : public ILink {
public:
    explicit OpticalLink(OpticalLinkConfig cfg);
    ~OpticalLink() override;

    void send(const std::vector<uint8_t>& frame_bytes) override;
    bool try_receive(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) override;
    void set_fps(double fps) override;
    void set_ec_level(ECLevel level) override;

private:
    // The experimental multi-QR-per-frame path (see OpticalLinkConfig::qr_codes_per_frame) —
    // send()/try_receive() dispatch to these only when that's >1; otherwise they run their
    // original bodies untouched. Kept as clearly separate methods rather than branches threaded
    // through the classic code, precisely so the classic path can't be affected by this at all.
    void send_multi(const std::vector<uint8_t>& frame_bytes);
    void flush_send_batch();
    bool try_receive_multi(std::vector<uint8_t>& out, std::chrono::milliseconds timeout);

    OpticalLinkConfig cfg_;
    std::string out_window_name_;    // shows the QR we're sending — what the other side's camera reads
    std::string camera_window_name_; // our own camera preview — purely local, for aiming/feedback
    void* capture_ = nullptr;  // cv::VideoCapture* (kept opaque so this header stays OpenCV-free)
    void* detector_ = nullptr; // cv::QRCodeDetector*
    void* wechat_detector_ = nullptr; // cv::wechat_qrcode::WeChatQRCode*, only if cfg_.use_wechat_qrcode
    // Runs cap->read() + the grayscale conversion continuously on its own thread, independent of
    // whatever try_receive() happens to be doing (see optical_link.cpp's CaptureThread) — real
    // timing data showed capture and decode as two comparable, sequential costs (up to ~45ms and
    // ~90ms respectively) in the old single-threaded loop; overlapping them across CPU cores hides
    // capture time behind decode time instead of paying both on every attempt. All HighGUI calls
    // (imshow/waitKey/window creation) still happen only on the thread that owns this OpticalLink,
    // never on this background thread — it only ever touches the camera and does color conversion,
    // neither of which is a windowing operation with a thread-affinity requirement.
    void* capture_thread_ = nullptr; // CaptureThread* (opaque, see optical_link.cpp)
    uint64_t last_seen_generation_ = 0; // last capture-thread frame generation this side consumed
    std::vector<uint8_t> last_decoded_hash_;
    int screen_w_ = 1000;
    int screen_h_ = 1000;
    int last_window_side_ = 0; // avoids re-resizing the window on every single frame
    int preview_frame_counter_ = 0; // throttles the local camera-preview imshow (see try_receive)

    // Accumulated since the last successful decode, so the "camera read a NEW QR" log line can
    // report where the wall-clock time between one successful read and the next actually went
    // (capture vs. decode) instead of leaving that a guess — see not_working.md, 2026-07-05.
    double capture_ms_accum_ = 0.0;
    double decode_ms_accum_ = 0.0;
    int poll_count_since_last_decode_ = 0;

    // multi-QR mode only (qr_codes_per_frame > 1) — untouched, unused whenever it's 1.
    std::vector<QrMatrix> pending_send_batch_;       // accumulated by send_multi(), flushed as a grid
    std::deque<std::vector<uint8_t>> pending_decoded_; // decoded payloads not yet returned to the caller
    std::vector<uint32_t> last_decoded_hashes_;      // hashes of the currently-on-screen batch's content
};

} // namespace qrtp

#endif // QRTP_WITH_OPENCV
