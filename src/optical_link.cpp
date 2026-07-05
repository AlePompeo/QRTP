#ifdef QRTP_WITH_OPENCV
#include "qrtp/optical_link.hpp"

#include <opencv2/objdetect.hpp>
#include <opencv2/opencv.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "qrtp/base64.hpp"
#include "qrtp/crc32.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace qrtp {
namespace {
cv::VideoCapture* cap(void* p) { return reinterpret_cast<cv::VideoCapture*>(p); }
cv::QRCodeDetector* det(void* p) { return reinterpret_cast<cv::QRCodeDetector*>(p); }

// Captures frames continuously in the background so try_receive() never has to block on
// cap->read() itself — it just picks up whatever the latest ready frame is. Only ever touches the
// camera (cap->read) and does a color-space conversion (cv::cvtColor); no HighGUI call
// (imshow/waitKey/window) ever happens here, since those need to stay on the thread that created
// the windows (see optical_link.hpp's class comment on why this matters on Windows).
struct CaptureThread {
    cv::VideoCapture* capture;
    std::mutex mtx;
    std::condition_variable cv_ready;
    cv::Mat latest_gray;
    uint64_t generation = 0;
    double last_capture_ms = 0.0;
    std::atomic<bool> stop{false};
    std::thread worker;

    explicit CaptureThread(cv::VideoCapture* c) : capture(c) {
        worker = std::thread([this] { run(); });
    }

    ~CaptureThread() {
        stop.store(true);
        cv_ready.notify_all();
        if (worker.joinable()) worker.join();
    }

    void run() {
        using Clock = std::chrono::steady_clock;
        while (!stop.load()) {
            const auto t0 = Clock::now();
            cv::Mat frame;
            const bool ok = capture->read(frame) && !frame.empty();
            const double capture_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (!ok) continue; // camera hiccup — cap->read() already paces this, just retry

            cv::Mat gray;
            if (frame.channels() > 1) {
                cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
            } else {
                gray = frame;
            }
            {
                std::lock_guard<std::mutex> lock(mtx);
                latest_gray = std::move(gray);
                last_capture_ms = capture_ms;
                ++generation;
            }
            cv_ready.notify_all();
        }
    }
};

CaptureThread* capture_thread(void* p) { return reinterpret_cast<CaptureThread*>(p); }
} // namespace

OpticalLink::OpticalLink(OpticalLinkConfig cfg)
    : cfg_(cfg), out_window_name_("QRTP - invio (mostra questo alla fotocamera dell'altro PC)"),
      camera_window_name_("QRTP - anteprima fotocamera locale") {
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

    // Two separate windows — see optical_link.hpp for why sharing one would break the protocol.
    cv::namedWindow(out_window_name_, cv::WINDOW_NORMAL);
    cv::namedWindow(camera_window_name_, cv::WINDOW_NORMAL);
    cv::resizeWindow(camera_window_name_, 480, 360);
    cv::moveWindow(camera_window_name_, 20, 20);

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

    // Requests a resolution whenever both dimensions are positive — true by default (1280x720,
    // see OpticalLinkConfig), since OpenCV's DirectShow backend otherwise tends to settle for a
    // conservative default (often 640x480) well below what the camera actually supports. Real
    // two-machine hardware data showed the exact same request can be honored cleanly by one
    // camera and remapped to a different, worse-performing mode by another, so CLI --width/--height
    // (or the GUI's field) let this be overridden per machine — including to 0x0, which skips the
    // request entirely and leaves the camera fully alone, for hardware where forcing anything
    // backfires.
    if (cfg_.camera_width > 0 && cfg_.camera_height > 0) {
        cap(capture_)->set(cv::CAP_PROP_FRAME_WIDTH, cfg_.camera_width);
        cap(capture_)->set(cv::CAP_PROP_FRAME_HEIGHT, cfg_.camera_height);
    }
    if (cfg_.log) {
        // Always reports what the camera actually negotiated, not just what (if anything) was
        // requested — a set() request can be silently ignored or remapped by the driver, so this
        // is the only way to know what's really in effect.
        cfg_.log("[optical] camera opened: resolution " +
                  std::to_string(static_cast<int>(cap(capture_)->get(cv::CAP_PROP_FRAME_WIDTH))) + "x" +
                  std::to_string(static_cast<int>(cap(capture_)->get(cv::CAP_PROP_FRAME_HEIGHT))) +
                  (cfg_.camera_width > 0 && cfg_.camera_height > 0
                       ? " (requested " + std::to_string(cfg_.camera_width) + "x" +
                             std::to_string(cfg_.camera_height) + ")"
                       : " (no resolution requested)"));
    }

    capture_thread_ = new CaptureThread(cap(capture_));
}

OpticalLink::~OpticalLink() {
    delete capture_thread(capture_thread_); // stops + joins the background thread first
    delete cap(capture_);
    delete det(detector_);
    cv::destroyWindow(out_window_name_);
    cv::destroyWindow(camera_window_name_);
}

void OpticalLink::set_fps(double fps) { cfg_.fps = fps; }

void OpticalLink::set_ec_level(ECLevel level) { cfg_.ec_level = static_cast<QrEcLevel>(static_cast<uint8_t>(level)); }

void OpticalLink::send(const std::vector<uint8_t>& frame_bytes) {
    using Clock = std::chrono::steady_clock;
    auto ms_since = [](Clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    };

    // Base64 first: our protocol frames are dense binary (a SYN's seq field alone is four 0x00
    // bytes), and cv::QRCodeDetector's decode path isn't trustworthy with embedded NUL bytes —
    // see base64.hpp. Base64 text is pure printable ASCII, sidestepping the question entirely.
    const std::string encoded_text = base64_encode(frame_bytes);
    const std::vector<uint8_t> encoded_bytes(encoded_text.begin(), encoded_text.end());
    const auto t_encode_start = Clock::now();
    const QrMatrix qr = qr_encode_bytes(encoded_bytes, cfg_.ec_level);
    const double encode_ms = ms_since(t_encode_start);
    const int quiet = 4;
    const int modules_per_side = qr.width + 2 * quiet;

    // Render at an integer pixels-per-module scale chosen so the image fills most of the screen —
    // sharp and pixel-exact (no blur from letting the window stretch a small bitmap). Large and
    // crisp matters here: the whole point is that another machine's camera has to resolve every
    // module edge cleanly from a normal camera-to-screen distance.
    const int target = static_cast<int>(0.9 * std::min(screen_w_, screen_h_));
    const int scale = std::max(4, target / modules_per_side);
    const int side = modules_per_side * scale;

    // Build at 1 px/module first (a plain pointer-write loop, no OpenCV call per module) and let a
    // single cv::resize scale it up to the on-screen size — was one cv::Rect + Mat::setTo call per
    // dark module (up to ~31k of them for a version-40 QR), which is real, avoidable per-frame CPU
    // cost that grows with the square of the QR version — exactly the kind of thing that would show
    // up as "bigger QR codes make everything laggier" independent of anything about the adaptive
    // controller's tuning. INTER_NEAREST at an exact integer scale factor maps every destination
    // pixel back to one source module pixel, so this produces byte-identical output to the old
    // per-module fill — same sharp, blocky module edges, just built much cheaper.
    const auto t_render_start = Clock::now();
    cv::Mat small(modules_per_side, modules_per_side, CV_8UC1, cv::Scalar(255));
    for (int y = 0; y < qr.width; ++y) {
        uint8_t* row = small.ptr<uint8_t>(y + quiet) + quiet;
        for (int x = 0; x < qr.width; ++x) {
            row[x] = qr.modules[static_cast<size_t>(y) * qr.width + x] ? 0 : 255;
        }
    }
    cv::Mat img;
    cv::resize(small, img, cv::Size(side, side), 0, 0, cv::INTER_NEAREST);
    const double render_ms = ms_since(t_render_start);

    if (side != last_window_side_) {
        // Large and centered, but still a normal decorated window (WINDOW_NORMAL, not
        // WND_PROP_FULLSCREEN) — "fills the screen" without going full OS-level fullscreen.
        cv::resizeWindow(out_window_name_, side, side);
        cv::moveWindow(out_window_name_, (screen_w_ - side) / 2, (screen_h_ - side) / 2);
        last_window_side_ = side;
    }

    // Shown in its own window (out_window_name_) that nothing else writes to, so it stays on
    // screen exactly as-is — for as long as it takes the other side to read it, not just for this
    // call's brief waitKey — until the next real send() replaces it. try_receive()'s camera-preview
    // updates happen in a completely separate window, so they can never race with this.
    cv::imshow(out_window_name_, img);
    if (cfg_.log) {
        cfg_.log("[optical] showing QR: version=" + std::to_string(qr.version) + " modules=" +
                  std::to_string(qr.width) + "x" + std::to_string(qr.width) + " module_px=" +
                  std::to_string(scale) + " base64_len=" + std::to_string(encoded_text.size()) +
                  " encode_ms=" + std::to_string(encode_ms) + " render_ms=" + std::to_string(render_ms));
    }
    cv::waitKey(static_cast<int>(1000.0 / std::max(1.0, cfg_.fps)));
}

bool OpticalLink::try_receive(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) {
    using Clock = std::chrono::steady_clock;
    auto* ct = capture_thread(capture_thread_);

    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        cv::Mat gray;
        double capture_ms = 0.0;
        bool got_new = false;
        {
            // Wait for the background CaptureThread to publish a frame we haven't seen yet, in
            // short slices so the outer loop still gets to call cv::waitKey(1) regularly even
            // during a dry spell (that call is what keeps this window's message queue serviced —
            // see the comment below). cv::Mat's assignment here is a cheap, reference-counted
            // shallow copy — safe to take under the lock and use after releasing it, since the
            // capture thread replacing its own latest_gray afterwards doesn't touch the buffer our
            // copy is still holding a reference to.
            std::unique_lock<std::mutex> lock(ct->mtx);
            const auto wait_until = std::min(deadline, Clock::now() + std::chrono::milliseconds(20));
            ct->cv_ready.wait_until(lock, wait_until,
                                     [&] { return ct->generation != last_seen_generation_ || ct->stop.load(); });
            if (ct->generation != last_seen_generation_) {
                gray = ct->latest_gray;
                capture_ms = ct->last_capture_ms;
                last_seen_generation_ = ct->generation;
                got_new = true;
            }
        }

        if (got_new && !gray.empty()) {
            capture_ms_accum_ += capture_ms;
            // Live camera preview in its own window — helps the user aim the camera, and never
            // touches out_window_name_, so it can't stomp on whatever we're currently displaying
            // to the other side. Throttled to every 3rd captured frame: it's a usability aid (aim
            // the camera), not something detection depends on, and imshow-ing every single frame
            // from a fast camera is real, avoidable render cost in this hot loop. detectAndDecode
            // and the waitKey below still run on every frame — cadence of those is what actually
            // matters for decode latency and for keeping the window responsive.
            if (++preview_frame_counter_ % 3 == 0) cv::imshow(camera_window_name_, gray);
            const auto t_decode_start = Clock::now();
            const std::string decoded_text = det(detector_)->detectAndDecode(gray);
            decode_ms_accum_ += std::chrono::duration<double, std::milli>(Clock::now() - t_decode_start).count();
            ++poll_count_since_last_decode_;
            if (!decoded_text.empty()) {
                const uint32_t h = crc32(reinterpret_cast<const uint8_t*>(decoded_text.data()), decoded_text.size());
                const std::vector<uint8_t> hash_bytes = {uint8_t(h), uint8_t(h >> 8), uint8_t(h >> 16),
                                                           uint8_t(h >> 24)};
                if (hash_bytes != last_decoded_hash_) {
                    last_decoded_hash_ = hash_bytes;
                    out = base64_decode(decoded_text);
                    if (cfg_.log) {
                        const int n = std::max(1, poll_count_since_last_decode_);
                        cfg_.log("[optical] camera read a NEW QR: base64_len=" + std::to_string(decoded_text.size()) +
                                  " decoded_bytes=" + std::to_string(out.size()) + " polls_since_last=" +
                                  std::to_string(poll_count_since_last_decode_) + " avg_capture_ms=" +
                                  std::to_string(capture_ms_accum_ / n) + " avg_decode_ms=" +
                                  std::to_string(decode_ms_accum_ / n));
                    }
                    capture_ms_accum_ = 0.0;
                    decode_ms_accum_ = 0.0;
                    poll_count_since_last_decode_ = 0;
                    cv::waitKey(1);
                    return true;
                }
                // Same content as the last successful read — the display probably hasn't
                // advanced to the next frame yet; keep polling.
            }
        }
        // cv::waitKey pumps HighGUI's message loop for *all* of this process's windows (paints,
        // lets them be moved/closed) — send() gets this for free via its own per-frame waitKey
        // call; try_receive() previously only slept, so neither window ever processed a message
        // while only receiving was happening, which Windows eventually reports as "Not
        // Responding" and can crash — this call is what was missing, not just cosmetic.
        cv::waitKey(1);
    }
    return false;
}

} // namespace qrtp
#endif // QRTP_WITH_OPENCV
