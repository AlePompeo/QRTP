#pragma once
#ifdef QRTP_WITH_OPENCV

#include <string>
#include <vector>

#include "qrtp/link.hpp"
#include "qrtp/qr_encoder.hpp"

namespace qrtp {

struct OpticalLinkConfig {
    int camera_index = 0;
    double fps = 10.0;
    QrEcLevel ec_level = QrEcLevel::M;
};

// The real physical layer: send() renders a QR frame to an on-screen window; try_receive() polls
// the camera and decodes any QR code currently visible to it, de-duplicating repeated reads of a
// frame that hasn't changed yet. Requires OpenCV — build with `make WITH_OPENCV=1` after
// installing opencv-devel. Not exercised by this project's automated tests (those run against
// LoopbackLink), since no OpenCV/camera is available in the environment this was written in.
class OpticalLink : public ILink {
public:
    explicit OpticalLink(OpticalLinkConfig cfg);
    ~OpticalLink() override;

    void send(const std::vector<uint8_t>& frame_bytes) override;
    bool try_receive(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) override;

private:
    OpticalLinkConfig cfg_;
    std::string window_name_;
    void* capture_ = nullptr;  // cv::VideoCapture* (kept opaque so this header stays OpenCV-free)
    void* detector_ = nullptr; // cv::QRCodeDetector*
    std::vector<uint8_t> last_decoded_hash_;
    int screen_w_ = 1000;
    int screen_h_ = 1000;
    int last_window_side_ = 0; // avoids re-resizing the window on every single frame
};

} // namespace qrtp

#endif // QRTP_WITH_OPENCV
