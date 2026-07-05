#pragma once
#ifdef QRTP_WITH_OPENCV

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "qrtp/link.hpp"
#include "qrtp/qr_encoder.hpp"

namespace qrtp {

struct OpticalLinkConfig {
    int camera_index = 0;
    // Was 3.0 (conservative starting point, letting the AIMD ramp it up); raised to 24 by request
    // based on real hardware testing showing that range works well from the start. Matches
    // AdaptiveConfig::fps_initial — this is only the value the link is constructed with before any
    // Sender/Receiver ever calls set_fps(); the AIMD takes over from there and still protects
    // against this being too fast for a given camera/lighting setup via its existing downshift.
    double fps = 24.0;
    QrEcLevel ec_level = QrEcLevel::M;
    // Defaults to kAutoMaxResolution (both), which requests the camera's own highest available
    // mode automatically (see OpticalLink's constructor for how — OpenCV has no portable "list
    // supported resolutions" API, so this asks for an intentionally oversized resolution and reads
    // back whatever the driver actually clamps to). Set both to 0 to leave the camera alone
    // entirely (its own default, without any request — see not_working.md, 2026-07-05, for why
    // that's sometimes a conservative mode like 640x480), or to any other positive WxH to request a
    // specific resolution instead. A real per-machine escape hatch either way: testing showed the
    // exact same fixed-resolution request can be honored cleanly by one camera and remapped to a
    // different, worse-performing mode by another — the actual negotiated size (which a camera can
    // still override regardless of what's requested) is always logged, never just assumed.
    static constexpr int kAutoMaxResolution = -1;
    int camera_width = kAutoMaxResolution;
    int camera_height = kAutoMaxResolution;
    // Optional diagnostics from the optical layer itself (distinct from ProtocolConfig::log):
    // whether the camera is detecting any QR content at all, and whether decoded text survives
    // frame validation. Wire it to the same sink as ProtocolConfig::log to see both in one place —
    // essential for telling "camera never sees a QR" apart from "sees one, gets rejected."
    std::function<void(const std::string&)> log;
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
    OpticalLinkConfig cfg_;
    std::string out_window_name_;    // shows the QR we're sending — what the other side's camera reads
    std::string camera_window_name_; // our own camera preview — purely local, for aiming/feedback
    void* capture_ = nullptr;  // cv::VideoCapture* (kept opaque so this header stays OpenCV-free)
    void* detector_ = nullptr; // cv::QRCodeDetector*
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
};

} // namespace qrtp

#endif // QRTP_WITH_OPENCV
