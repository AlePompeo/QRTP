#pragma once
#ifdef QRTP_WITH_OPENCV

#include <atomic>
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
    // Reverted 24 -> 3, then explicit user request 3 -> 10 (2026-07-06) — see
    // AdaptiveConfig::fps_initial for the full reasoning, including why 10 specifically reopens a
    // once-documented real-hardware finding rather than building on new evidence. Matches
    // AdaptiveConfig::fps_initial — this is only the value the link is constructed with before any
    // Sender/Receiver ever calls set_fps(); the AIMD takes over from there.
    double fps = 10.0;
    QrEcLevel ec_level = QrEcLevel::M;
    // Was 1280x720 (2026-07-05, per real hardware testing across two separate machines), then
    // lowered to 640x480 (still 2026-07-05) to match a GUI-only preference the user later decided
    // should be the library/CLI default too, paired with use_wechat_qrcode below. This is a real,
    // deliberate departure from the even earlier default (kAutoMaxResolution, requesting the
    // camera's own highest available mode automatically): an earlier entry in not_working.md
    // documented the exact same auto-max request negotiating cleanly to 1280x720 on one machine but
    // to a different, worse-performing mode (1280x1024) on another — so "auto-max" itself was never
    // a guaranteed-safe default, just an automatic one. 640x480 is a genuine per-camera preference
    // from real testing, not a universal law; still fully overridable — set both to
    // kAutoMaxResolution to opt back into auto-detecting the camera's highest mode, both to 0 to
    // leave the camera alone entirely (its own default, without any request), or any other positive
    // WxH for a specific mode. The actual negotiated size a camera settles on (which can still
    // differ from any of these) is always logged, never just assumed.
    static constexpr int kAutoMaxResolution = -1;
    int camera_width = 640;
    int camera_height = 480;
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
    // Requests higher camera contrast and zero saturation right after opening, on the same
    // "ask for an oversized/extreme value and let the driver clamp it" technique already used for
    // camera_width/camera_height above. Aimed squarely at what actually trips up a QR decoder in
    // practice: a washed-out, low-contrast capture (dim monitor backlight, glare, ambient light)
    // blurring the boundary between a dark module and its surrounding quiet zone. Saturation to 0
    // is safe unconditionally — a QR code is pure black/white, so color information is never useful
    // input to the detector, only a possible source of chroma noise in the grayscale conversion
    // CaptureThread::run() does right after capture. Default on per explicit user request
    // (2026-07-06); still a runtime toggle since some webcams' default contrast is already
    // well-tuned and pushing it further could clip highlights/shadows on a specific setup.
    bool enhance_camera_contrast = true;
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
    // Was opt-in/default false (2026-07-05, still untested on real camera hardware at the time),
    // then promoted to the library/CLI default (still 2026-07-05) at the user's request, alongside
    // the 640x480 default above. When true, decodes using cv::wechat_qrcode::WeChatQRCode
    // (opencv_contrib: a small SSD-style detector CNN plus a super-resolution enhancement step for
    // small/blurry crops) instead of the classic cv::QRCodeDetector. Chosen over an alternative
    // under consideration (zxing-cpp, a separate third-party library) specifically because it adds
    // no new dependency or build pipeline: it's a module of the same OpenCV this project already
    // links against (the Windows cross-compile's custom-built OpenCV was extended with
    // opencv_contrib's wechat_qrcode + dnn specifically for this — see not_working.md). The classic
    // path (this flag false) remains fully available and completely untouched — see
    // wechat_qrcode_model_dir below for the 4 model files this needs that OpenCV itself doesn't
    // bundle (shipped in dist-windows.zip under models/wechat_qrcode/). Motivation: this project's
    // whole session of real-hardware logs pointed at decode *reliability* (frames that never
    // resolve, driving retries/downshifts), not raw per-attempt decode cost, as the dominant
    // remaining bottleneck — a stronger detector attacks that directly, even though each attempt
    // likely costs somewhat more CPU than the classical approach. Still not verified end-to-end on
    // real camera hardware as of this default flip.
    //
    // Default depends on whether this build's OpenCV actually has the module at all: confirmed
    // present in Fedora's stock opencv-devel package, but genuinely *absent* from Debian 12's and
    // Ubuntu 22.04/24.04's stock libopencv-dev (opencv_contrib's wechat_qrcode isn't universally
    // packaged) — see CMakeLists.txt/Makefile's QRTP_HAVE_WECHAT_QRCODE detection and
    // optical_link.cpp. Setting this true on a build without it throws at OpticalLink construction
    // rather than silently falling back, so a mismatched config is never a silent surprise.
#ifdef QRTP_HAVE_WECHAT_QRCODE
    bool use_wechat_qrcode = true;
#else
    bool use_wechat_qrcode = false;
#endif
    // Directory containing detect.prototxt, detect.caffemodel, sr.prototxt, sr.caffemodel (from
    // github.com/WeChatCV/opencv_3rdparty, branch wechat_qrcode — the canonical source
    // opencv_contrib's own wechat_qrcode module points at). Empty (default) resolves to a
    // "models/wechat_qrcode" folder next to the running executable, matching how libqrencode.dll
    // is expected next to qrtp.exe (see qr_encoder.cpp) — set explicitly to point elsewhere.
    std::string wechat_qrcode_model_dir;
    // Optional: the same cooperative-cancellation flag already passed to ProtocolConfig::cancel/
    // BroadcastConfig::cancel (checked at Sender/Receiver/BroadcastSender/BroadcastReceiver's own
    // checkpoints) — set here too so OpticalLink can draw a clickable on-screen "STOP" button on
    // the outgoing QR window (see send()/flush_send_batch()) and set this flag directly when
    // clicked, without needing any new signaling path. Left nullptr (default), no button is drawn
    // or wired at all — e.g. plain CLI usage with no GUI/cancel flag to control.
    std::atomic<bool>* cancel = nullptr;
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
    // cv::setMouseCallback needs a plain C-style function pointer (optical_link.cpp's
    // on_out_window_mouse), which forwards straight into handle_stop_button_click() below.
    friend void on_out_window_mouse(int, int, int, int, void*);

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
    void* out_canvas_ = nullptr; // cv::Mat*, persistent full-screen canvas reused by send()/
                                 // flush_send_batch() (see show_padded in optical_link.cpp) instead
                                 // of allocating and white-filling a new one every displayed frame
    int last_content_w_ = 0;     // previous frame's centered content size — show_padded only
    int last_content_h_ = 0;     // re-fills white when this actually changes
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
    // Lets try_receive_multi() tell "still actively filling up" from "genuinely stalled" before
    // forcing a partial batch onto the screen alone — see try_receive_multi() in optical_link.cpp.
    int pending_size_at_last_poll_ = 0;
    int pending_stall_polls_ = 0;

    // On-screen "STOP" button overlaid on out_window_name_ (top-right corner) — only drawn/wired
    // when cfg_.cancel is set (see OpticalLinkConfig::cancel). Rect computed once in the ctor from
    // screen_w_, since the window itself never moves/resizes for the whole session.
    int stop_btn_x0_ = 0, stop_btn_y0_ = 0, stop_btn_x1_ = 0, stop_btn_y1_ = 0;
    void draw_stop_button(void* canvas); // cv::Mat*, kept opaque so this header stays OpenCV-free
    void handle_stop_button_click(int x, int y);
};

} // namespace qrtp

#endif // QRTP_WITH_OPENCV
