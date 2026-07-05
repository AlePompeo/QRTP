#ifdef QRTP_WITH_OPENCV
#include "qrtp/optical_link.hpp"

#include <opencv2/objdetect.hpp>
#include <opencv2/opencv.hpp>

#include <algorithm>
#include <stdexcept>

#include "qrtp/crc32.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace qrtp {
namespace {
cv::VideoCapture* cap(void* p) { return reinterpret_cast<cv::VideoCapture*>(p); }
cv::QRCodeDetector* det(void* p) { return reinterpret_cast<cv::QRCodeDetector*>(p); }
} // namespace

OpticalLink::OpticalLink(OpticalLinkConfig cfg) : cfg_(cfg), window_name_("qrtp-display") {
#ifdef _WIN32
    // Without this, Windows silently bitmap-stretches (and blurs) everything this process draws
    // on a display with scaling enabled (125%/150%, the default on most modern laptops) — which
    // is fatal for a QR code specifically, since blurred module edges are exactly what a QR
    // decoder can't cope with. Must be called before any window is created.
    SetProcessDPIAware();
    screen_w_ = GetSystemMetrics(SM_CXSCREEN);
    screen_h_ = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w_ <= 0 || screen_h_ <= 0) { screen_w_ = 1000; screen_h_ = 1000; }
#endif

    cv::namedWindow(window_name_, cv::WINDOW_NORMAL);
#ifdef _WIN32
    // DirectShow is the well-known reliable backend for opening a webcam by index on Windows —
    // OpenCV's default auto-selected backend (Media Foundation) is markedly slower to open and
    // occasionally fails outright on some webcam drivers.
    capture_ = new cv::VideoCapture(cfg_.camera_index, cv::CAP_DSHOW);
#else
    capture_ = new cv::VideoCapture(cfg_.camera_index);
#endif
    detector_ = new cv::QRCodeDetector();
    if (!cap(capture_)->isOpened()) {
        throw std::runtime_error("OpticalLink: cannot open camera index " + std::to_string(cfg_.camera_index));
    }
}

OpticalLink::~OpticalLink() {
    delete cap(capture_);
    delete det(detector_);
    cv::destroyWindow(window_name_);
}

void OpticalLink::send(const std::vector<uint8_t>& frame_bytes) {
    const QrMatrix qr = qr_encode_bytes(frame_bytes, cfg_.ec_level);
    const int quiet = 4;
    const int modules_per_side = qr.width + 2 * quiet;

    // Render at an integer pixels-per-module scale chosen so the image fills most of the screen —
    // sharp and pixel-exact (no blur from letting the window stretch a small bitmap). Large and
    // crisp matters here: the whole point is that another machine's camera has to resolve every
    // module edge cleanly from a normal camera-to-screen distance.
    const int target = static_cast<int>(0.9 * std::min(screen_w_, screen_h_));
    const int scale = std::max(4, target / modules_per_side);
    const int side = modules_per_side * scale;

    cv::Mat img(side, side, CV_8UC1, cv::Scalar(255));
    for (int y = 0; y < qr.width; ++y) {
        for (int x = 0; x < qr.width; ++x) {
            if (qr.modules[static_cast<size_t>(y) * qr.width + x]) {
                cv::Rect block((x + quiet) * scale, (y + quiet) * scale, scale, scale);
                img(block).setTo(cv::Scalar(0));
            }
        }
    }

    if (side != last_window_side_) {
        // Large and centered, but still a normal decorated window (WINDOW_NORMAL, not
        // WND_PROP_FULLSCREEN) — "fills the screen" without going full OS-level fullscreen.
        cv::resizeWindow(window_name_, side, side);
        cv::moveWindow(window_name_, (screen_w_ - side) / 2, (screen_h_ - side) / 2);
        last_window_side_ = side;
    }

    cv::imshow(window_name_, img);
    cv::waitKey(static_cast<int>(1000.0 / std::max(1.0, cfg_.fps)));
}

bool OpticalLink::try_receive(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        cv::Mat frame;
        if (cap(capture_)->read(frame) && !frame.empty()) {
            // Live camera preview — also helps the user physically aim the camera at the sender's
            // screen, not just cosmetic.
            cv::imshow(window_name_, frame);
            const std::string decoded = det(detector_)->detectAndDecode(frame);
            if (!decoded.empty()) {
                std::vector<uint8_t> bytes(decoded.begin(), decoded.end());
                const uint32_t h = crc32(bytes.data(), bytes.size());
                const std::vector<uint8_t> hash_bytes = {uint8_t(h), uint8_t(h >> 8), uint8_t(h >> 16),
                                                           uint8_t(h >> 24)};
                if (hash_bytes != last_decoded_hash_) {
                    last_decoded_hash_ = hash_bytes;
                    out = std::move(bytes);
                    cv::waitKey(1);
                    return true;
                }
                // Same content as the last successful read — the display probably hasn't
                // advanced to the next frame yet; keep polling.
            }
        }
        // cv::waitKey pumps the HighGUI window's message loop (paints, lets it be moved/closed).
        // send() gets this for free via its own per-frame waitKey call; try_receive() previously
        // only slept, so its window never processed any messages, which Windows eventually reports
        // as "Not Responding" and can crash — this call is what was missing, not just cosmetic.
        cv::waitKey(1);
    }
    return false;
}

} // namespace qrtp
#endif // QRTP_WITH_OPENCV
