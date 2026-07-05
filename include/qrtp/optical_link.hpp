#pragma once
#ifdef QRTP_WITH_OPENCV

#include <functional>
#include <string>
#include <vector>

#include "qrtp/link.hpp"
#include "qrtp/qr_encoder.hpp"

namespace qrtp {

struct OpticalLinkConfig {
    int camera_index = 0;
    // A webcam needs meaningful dwell time on a static QR to focus, expose, and let the decode
    // algorithm lock on; 10fps (100ms/frame) proved too fast for real hardware in testing.
    double fps = 3.0;
    QrEcLevel ec_level = QrEcLevel::M;
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
    std::vector<uint8_t> last_decoded_hash_;
    int screen_w_ = 1000;
    int screen_h_ = 1000;
    int last_window_side_ = 0; // avoids re-resizing the window on every single frame
};

} // namespace qrtp

#endif // QRTP_WITH_OPENCV
