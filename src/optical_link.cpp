#ifdef QRTP_WITH_OPENCV
#include "qrtp/optical_link.hpp"

#include <opencv2/objdetect.hpp>
#include <opencv2/opencv.hpp>
#include <opencv2/wechat_qrcode.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
cv::wechat_qrcode::WeChatQRCode* wechat_det(void* p) {
    return reinterpret_cast<cv::wechat_qrcode::WeChatQRCode*>(p);
}
cv::Mat* screen_canvas(void* p) { return reinterpret_cast<cv::Mat*>(p); }

// Resolves OpticalLinkConfig::wechat_qrcode_model_dir when left empty: a "models/wechat_qrcode"
// folder next to the running executable, the same "next to the .exe" convention already used for
// libqrencode.dll (see qr_encoder.cpp's LoadLibrary comment) — keeps the distributed dist-windows.zip
// self-contained without needing an install step or environment variable. Falls back to a plain
// relative path on non-Windows, since this project's OpenCV/GUI path is Windows-primary (CAP_DSHOW,
// the Win32 GUI) and this fallback is never exercised by the automated tests either way.
std::string default_wechat_model_dir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring wpath(path, (len > 0 && len < MAX_PATH) ? len : 0);
    const auto pos = wpath.find_last_of(L'\\');
    const std::wstring dir = (pos != std::wstring::npos) ? wpath.substr(0, pos) : L".";
    const int size = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<size_t>(std::max(0, size - 1)), '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, narrow.data(), size, nullptr, nullptr);
    return narrow + "\\models\\wechat_qrcode";
#else
    return "models/wechat_qrcode";
#endif
}

// Bounds decode cost independent of whatever resolution the camera negotiated: cv::QRCodeDetector's
// per-attempt cost tracks the *input* image's pixel count, not just the QR's own module count (real
// timing data from this project's own logs confirmed that — see not_working.md), so decoding a full
// 1280x720-or-larger frame is real, avoidable per-attempt cost once the image is far bigger than what
// resolving a QR's modules actually needs. 800px on the longest side matches the value a comparable
// real-world project (jtalbott22/optical-data-bridge) uses for the same tradeoff — decode input
// downscaled, capture/display resolution left untouched. Only ever shrinks, never upscales, so an
// already-small capture (e.g. an explicit lower --width/--height override) passes through unchanged.
// cv::INTER_AREA is the recommended OpenCV interpolation for shrinking specifically (avoids the
// aliasing/moiré that INTER_LINEAR or INTER_NEAREST can introduce on fine, high-contrast QR module
// edges) — this only feeds the decoder; the unscaled `gray` is still what the live preview shows.
cv::Mat downscale_for_decode(const cv::Mat& gray) {
    constexpr int kMaxDecodeDim = 800;
    const int longest = std::max(gray.cols, gray.rows);
    if (longest <= kMaxDecodeDim) return gray;
    const double scale = static_cast<double>(kMaxDecodeDim) / longest;
    cv::Mat small;
    cv::resize(gray, small, cv::Size(), scale, scale, cv::INTER_AREA);
    return small;
}

// Centers `img` on `persistent_canvas`, a canvas exactly the display's screen resolution (or
// larger, only in the rare case img itself is already bigger — never crops), reused across calls
// instead of being reallocated every frame. This is what lets out_window_name_ stay fixed at the
// full screen size for the entire session (see the ctor) instead of being resized/moved to match
// whatever the current QR happens to render at: every frame becomes a single 1:1 pixel copy into a
// same-sized canvas, so the window never has to stretch the image itself (stretching a non-matching
// aspect ratio would distort the QR's square modules, exactly what a decoder can't tolerate) and
// never has to move/resize on screen, which would otherwise happen every time the AIMD controller
// changes QR version mid-transfer.
//
// `last_w`/`last_h` (the caller's own previous-content-size memory, not read from the canvas
// itself) let this skip the white-fill entirely on the common case of consecutive frames at the
// same rendered size — only reallocating/re-filling when the canvas itself needs to grow, or when
// `img`'s size actually changed since the last call (a smaller new image would otherwise leave the
// previous, larger one's pixels lingering outside the new centered rect).
void show_padded(cv::Mat& persistent_canvas, int& last_w, int& last_h, int screen_w, int screen_h,
                  const cv::Mat& img) {
    const int canvas_w = std::max(screen_w, img.cols);
    const int canvas_h = std::max(screen_h, img.rows);
    if (persistent_canvas.empty() || persistent_canvas.cols != canvas_w || persistent_canvas.rows != canvas_h) {
        persistent_canvas = cv::Mat(canvas_h, canvas_w, img.type(), cv::Scalar(255));
        last_w = last_h = -1; // force the copyTo below to be the only content on a fresh canvas
    } else if (img.cols != last_w || img.rows != last_h) {
        persistent_canvas.setTo(cv::Scalar(255));
    }
    const int off_x = (canvas_w - img.cols) / 2;
    const int off_y = (canvas_h - img.rows) / 2;
    img.copyTo(persistent_canvas(cv::Rect(off_x, off_y, img.cols, img.rows)));
    last_w = img.cols;
    last_h = img.rows;
}

// Captures frames continuously in the background so try_receive() never has to block on
// cap->read() itself — it just picks up whatever the latest ready frame is. Only ever touches the
// camera (cap->read) and does a color-space conversion (cv::cvtColor); no HighGUI call
// (imshow/waitKey/window) ever happens here, since those need to stay on the thread that created
// the windows (see optical_link.hpp's class comment on why this matters on Windows).
struct CaptureThread {
    cv::VideoCapture* capture;
    std::function<void(const std::string&)> log;
    std::mutex mtx;
    std::condition_variable cv_ready;
    cv::Mat latest_gray;
    uint64_t generation = 0;
    double last_capture_ms = 0.0;
    std::atomic<bool> stop{false};
    std::thread worker;

    CaptureThread(cv::VideoCapture* c, std::function<void(const std::string&)> log_fn)
        : capture(c), log(std::move(log_fn)) {
        worker = std::thread([this] { run(); });
    }

    ~CaptureThread() {
        stop.store(true);
        cv_ready.notify_all();
        if (worker.joinable()) worker.join();
    }

    void run() {
        using Clock = std::chrono::steady_clock;
#ifdef _WIN32
        // This thread's only job is grabbing camera frames as promptly as possible; a modest
        // priority bump (not REALTIME/HIGH, which can starve the rest of the system) reduces how
        // often the OS scheduler lets something less time-sensitive delay a capture in progress.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        int consecutive_failures = 0;
        while (!stop.load()) {
            const auto t0 = Clock::now();
            cv::Mat frame;
            const bool ok = capture->read(frame) && !frame.empty();
            const double capture_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (!ok) {
                // cap->read() normally paces this loop by blocking until the next frame — but if
                // the camera disconnects or its driver wedges, some backends return false
                // immediately instead of blocking, which without a backoff here would spin this
                // thread at ~100% of a CPU core indefinitely with no signal anything is wrong. Logs
                // once per sustained failure streak (not every iteration, which would just spam).
                if (++consecutive_failures == 30 && log) {
                    log("[optical] camera has not produced a frame in a while (disconnected, or "
                        "driver/USB stalled?)");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            consecutive_failures = 0;

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

    // A modest, whole-process priority bump — this process's entire job while a transfer is
    // running is latency-sensitive (render/capture/decode round trips), so reduce how much the OS
    // scheduler lets unrelated background processes delay it. ABOVE_NORMAL, not HIGH/REALTIME,
    // which can genuinely starve the rest of the system — this is the safe, commonly-recommended
    // middle ground for latency-sensitive desktop apps (many audio/video tools use exactly this).
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#endif

    // Two separate windows — see optical_link.hpp for why sharing one would break the protocol.
    cv::namedWindow(out_window_name_, cv::WINDOW_NORMAL);
    cv::namedWindow(camera_window_name_, cv::WINDOW_NORMAL);
    cv::resizeWindow(camera_window_name_, 480, 360);
    cv::moveWindow(camera_window_name_, 20, 20);

    // Fixed at the full screen resolution, for the entire session — not OS-level exclusive
    // fullscreen (WND_PROP_FULLSCREEN), which hands display/compositor control away from the
    // window manager and can fight with alt-tab or a second monitor; this stays an ordinary,
    // decorated WINDOW_NORMAL window that simply happens to be sized to cover the whole screen.
    // send()/flush_send_batch() pad every frame onto a same-sized persistent canvas (see
    // show_padded above) instead of resizing this window to match the QR's own size, so it never
    // needs to move or resize again after this.
    cv::resizeWindow(out_window_name_, screen_w_, screen_h_);
    cv::moveWindow(out_window_name_, 0, 0);
    out_canvas_ = new cv::Mat(); // lazily sized to the screen on the first show_padded() call
    // Keeps the QR visible even if the user clicks into another window mid-transfer — the whole
    // point of this window is that a camera on the other machine has to be able to read it
    // continuously, so it must never end up hidden behind something else.
    cv::setWindowProperty(out_window_name_, cv::WND_PROP_TOPMOST, 1);

#ifdef _WIN32
    // DirectShow is the well-known reliable backend for opening a webcam by index on Windows —
    // OpenCV's default auto-selected backend (Media Foundation) is markedly slower to open and
    // occasionally fails outright on some webcam drivers.
    capture_ = new cv::VideoCapture(cfg_.camera_index, cv::CAP_DSHOW);
#else
    capture_ = new cv::VideoCapture(cfg_.camera_index);
#endif
    detector_ = new cv::QRCodeDetector();

    // Opt-in (see OpticalLinkConfig::use_wechat_qrcode) — loaded eagerly here, not lazily on first
    // decode, so a missing/misplaced model file fails loudly at startup (matching how a missing
    // camera index fails below) instead of surfacing as a confusing per-frame decode failure deep
    // into a transfer.
    if (cfg_.use_wechat_qrcode) {
        const std::string model_dir =
            cfg_.wechat_qrcode_model_dir.empty() ? default_wechat_model_dir() : cfg_.wechat_qrcode_model_dir;
#ifdef _WIN32
        const std::string sep = "\\";
#else
        const std::string sep = "/";
#endif
        try {
            wechat_detector_ = new cv::wechat_qrcode::WeChatQRCode(
                model_dir + sep + "detect.prototxt", model_dir + sep + "detect.caffemodel",
                model_dir + sep + "sr.prototxt", model_dir + sep + "sr.caffemodel");
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "OpticalLink: failed to load WeChatQRCode models from \"" + model_dir +
                "\" (expects detect.prototxt, detect.caffemodel, sr.prototxt, sr.caffemodel there): " + e.what());
        }
    }

    if (!cap(capture_)->isOpened()) {
        throw std::runtime_error("OpticalLink: cannot open camera index " + std::to_string(cfg_.camera_index));
    }

    // Asks the backend to keep at most 1 frame queued internally, so cap->read() returns the
    // freshest available frame instead of dequeuing one the driver already had buffered — a
    // narrower, more uniformly-safe request than the resolution one below: it doesn't change the
    // capture format/pixel count (where different camera hardware diverged sharply), just how many
    // already-captured frames sit in a queue before this process sees them. A `set()` a backend
    // doesn't honor is simply ignored, same as any other capability request here.
    cap(capture_)->set(cv::CAP_PROP_BUFFERSIZE, 1);

    // Requests MJPEG explicitly — real user report: the camera lags badly at higher resolutions
    // in this app specifically, but not when opened at the same resolution in Windows' own Camera
    // app. That's the textbook symptom of a USB webcam being driven in a raw/uncompressed capture
    // mode: a 1080p raw YUY2 frame is ~4MB, which at any real frame rate exceeds what USB can
    // sustain, so the camera/driver ends up throttling or stalling. Most native camera apps
    // request MJPEG (hardware-compressed by the camera itself, ~10-20x smaller per frame) for
    // exactly this reason at anything beyond low resolutions; OpenCV's DirectShow backend doesn't
    // do this automatically unless asked. Set before the resolution request below, since some
    // backends only unlock a camera's higher-resolution modes once the pixel format is already
    // negotiated.
    //
    // Made opt-in (default off) as of 2026-07-05, reverting the earlier "always force it"
    // reasoning above: a fresh real-hardware log showed capture staying fast (<1.5ms/frame)
    // throughout, while decode *reliability* was poor (many retries, cascading AIMD downshifts
    // that never stabilized) even though decode cost itself was stable — not the CPU/bandwidth
    // symptom this was originally meant to fix. JPEG's lossy compression is a well-documented
    // cause of ringing/blur on sharp high-contrast edges, exactly what a QR module boundary is, so
    // it's now a live suspect for that reliability problem instead of a pure win. Resolution
    // forcing already showed opposite effects on different camera hardware; this is the same risk
    // shape, so it gets the same treatment — a per-machine opt-in, not a forced default — see
    // request_mjpeg in optical_link.hpp.
    if (cfg_.request_mjpeg) {
        cap(capture_)->set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    }

    // Opt-in, default off — see OpticalLinkConfig::disable_autofocus for the reasoning (a real log
    // showed capture *and* decode latency both drop sharply and together right at a fps transition,
    // which doesn't fit a pure camera-hardware-bound explanation once same-machine CPU contention
    // is ruled out; continuous autofocus hunting under fast-changing on-screen content is the next
    // suspect). Requested after BUFFERSIZE/FOURCC, before resolution, for the same reason those are
    // ordered this way: some backends renegotiate more freely before a resolution request locks
    // things in.
    if (cfg_.disable_autofocus) {
        cap(capture_)->set(cv::CAP_PROP_AUTOFOCUS, 0);
    }

    // Default on (see OpticalLinkConfig::enhance_camera_contrast) — same "ask for an oversized/
    // extreme value and let the driver clamp it" technique as the resolution request below, applied
    // here to contrast instead of pixel dimensions. Ordered with BUFFERSIZE/FOURCC/AUTOFOCUS above,
    // before the resolution request, for the same reason those are: some backends renegotiate more
    // freely before a resolution request locks things in.
    if (cfg_.enhance_camera_contrast) {
        constexpr double kOversizedProp = 1000.0; // comfortably beyond any driver's real max/min
        cap(capture_)->set(cv::CAP_PROP_CONTRAST, kOversizedProp);
        cap(capture_)->set(cv::CAP_PROP_SATURATION, 0.0);
    }

    // Three modes, by default requesting the camera's highest available mode automatically
    // (kAutoMaxResolution): OpenCV's base VideoCapture interface has no portable "list supported
    // resolutions" call (that's backend-specific, e.g. DirectShow's IAMStreamConfig, real COM
    // interop this codebase doesn't otherwise need), so this asks for an intentionally oversized
    // resolution and lets the driver clamp it to its true native maximum — a common, well-
    // documented technique for exactly this. NOTE this is a real, deliberate trade: a higher-
    // resolution frame is *more* expensive for cv::QRCodeDetector to scan per attempt (real timing
    // data suggests its cost tracks the captured frame's size, not just the QR's own module
    // count), so this can make each individual decode attempt slower even as it may (unverified)
    // reduce failed/retried attempts from a sharper image. CLI --width/--height (or the GUI field)
    // override this per machine: a specific positive WxH requests exactly that, and 0x0 skips any
    // request and leaves the camera fully alone.
    std::string requested_desc;
    if (cfg_.camera_width == OpticalLinkConfig::kAutoMaxResolution &&
        cfg_.camera_height == OpticalLinkConfig::kAutoMaxResolution) {
        constexpr int kOversized = 10000; // comfortably beyond any consumer webcam's native mode
        cap(capture_)->set(cv::CAP_PROP_FRAME_WIDTH, kOversized);
        cap(capture_)->set(cv::CAP_PROP_FRAME_HEIGHT, kOversized);
        requested_desc = " (requested auto-max)";
    } else if (cfg_.camera_width > 0 && cfg_.camera_height > 0) {
        cap(capture_)->set(cv::CAP_PROP_FRAME_WIDTH, cfg_.camera_width);
        cap(capture_)->set(cv::CAP_PROP_FRAME_HEIGHT, cfg_.camera_height);
        requested_desc =
            " (requested " + std::to_string(cfg_.camera_width) + "x" + std::to_string(cfg_.camera_height) + ")";
    } else {
        requested_desc = " (no resolution requested)";
    }
    if (cfg_.log) {
        // Always reports what the camera actually negotiated, not just what (if anything) was
        // requested — a set() request can be silently ignored or remapped by the driver, so this
        // is the only way to know what's really in effect.
        const int fourcc = static_cast<int>(cap(capture_)->get(cv::CAP_PROP_FOURCC));
        const char fourcc_chars[5] = {static_cast<char>(fourcc & 0xFF), static_cast<char>((fourcc >> 8) & 0xFF),
                                       static_cast<char>((fourcc >> 16) & 0xFF), static_cast<char>((fourcc >> 24) & 0xFF),
                                       '\0'};
        cfg_.log("[optical] camera opened: resolution " +
                  std::to_string(static_cast<int>(cap(capture_)->get(cv::CAP_PROP_FRAME_WIDTH))) + "x" +
                  std::to_string(static_cast<int>(cap(capture_)->get(cv::CAP_PROP_FRAME_HEIGHT))) + requested_desc +
                  " fourcc=" + std::string(fourcc_chars) +
                  (cfg_.request_mjpeg ? " (mjpeg requested)" : " (mjpeg not requested)") +
                  (cfg_.disable_autofocus ? " (autofocus disabled)" : " (autofocus left on)") +
                  (cfg_.use_wechat_qrcode ? " (decoder=WeChatQRCode)" : " (decoder=classic)") +
                  (cfg_.enhance_camera_contrast
                       ? " (contrast boosted: contrast=" + std::to_string(cap(capture_)->get(cv::CAP_PROP_CONTRAST)) +
                             " saturation=" + std::to_string(cap(capture_)->get(cv::CAP_PROP_SATURATION)) + ")"
                       : " (contrast left at default)"));
    }

    capture_thread_ = new CaptureThread(cap(capture_), cfg_.log);
}

OpticalLink::~OpticalLink() {
    delete capture_thread(capture_thread_); // stops + joins the background thread first
    delete cap(capture_);
    delete det(detector_);
    delete wechat_det(wechat_detector_);
    delete screen_canvas(out_canvas_);
    cv::destroyWindow(out_window_name_);
    cv::destroyWindow(camera_window_name_);
}

void OpticalLink::set_fps(double fps) { cfg_.fps = fps; }

void OpticalLink::set_ec_level(ECLevel level) { cfg_.ec_level = static_cast<QrEcLevel>(static_cast<uint8_t>(level)); }

void OpticalLink::send(const std::vector<uint8_t>& frame_bytes) {
    if (cfg_.qr_codes_per_frame > 1) {
        send_multi(frame_bytes);
        return;
    }

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

    // Shown in its own window (out_window_name_) that nothing else writes to, so it stays on
    // screen exactly as-is — for as long as it takes the other side to read it, not just for this
    // call's brief waitKey — until the next real send() replaces it. try_receive()'s camera-preview
    // updates happen in a completely separate window, so they can never race with this. Padded onto
    // the persistent full-screen canvas (see show_padded) since the window itself is fixed at
    // screen_w_ x screen_h_ for the whole session — this frame's QR can be a different size than
    // the last (AIMD-driven) without ever needing to move or resize the window, or re-filling the
    // whole canvas white unless that size actually changed.
    show_padded(*screen_canvas(out_canvas_), last_content_w_, last_content_h_, screen_w_, screen_h_, img);
    cv::imshow(out_window_name_, *screen_canvas(out_canvas_));
    if (cfg_.log) {
        cfg_.log("[optical] showing QR: version=" + std::to_string(qr.version) + " modules=" +
                  std::to_string(qr.width) + "x" + std::to_string(qr.width) + " module_px=" +
                  std::to_string(scale) + " base64_len=" + std::to_string(encoded_text.size()) +
                  " encode_ms=" + std::to_string(encode_ms) + " render_ms=" + std::to_string(render_ms));
    }
    cv::waitKey(static_cast<int>(1000.0 / std::max(1.0, cfg_.fps)));
}

bool OpticalLink::try_receive(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) {
    if (cfg_.qr_codes_per_frame > 1) {
        return try_receive_multi(out, timeout);
    }

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
            const cv::Mat decode_input = downscale_for_decode(gray);
            std::string decoded_text;
            if (cfg_.use_wechat_qrcode) {
                // Can return more than one decoded string even here (its detector doesn't assume a
                // single QR) — the classic path's own invariant is "one QR on screen at a time," so
                // just take the first result, same as detectAndDecode()'s single-string contract.
                const std::vector<std::string> results = wechat_det(wechat_detector_)->detectAndDecode(decode_input);
                if (!results.empty()) decoded_text = results.front();
            } else {
                decoded_text = det(detector_)->detectAndDecode(decode_input);
            }
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

// ============================================================================================
// Experimental multi-QR-per-frame path (OpticalLinkConfig::qr_codes_per_frame > 1). Nothing here
// runs, and nothing above this point behaves any differently, unless a caller explicitly opts in —
// see the config field's own comment for why this is a runtime toggle rather than a hard swap.
// ============================================================================================

void OpticalLink::send_multi(const std::vector<uint8_t>& frame_bytes) {
    // Same Base64 step as the classic path (see send()'s own comment for why) — just queued
    // instead of immediately rendered, so several can be laid out in one grid together.
    const std::string encoded_text = base64_encode(frame_bytes);
    const std::vector<uint8_t> encoded_bytes(encoded_text.begin(), encoded_text.end());
    pending_send_batch_.push_back(qr_encode_bytes(encoded_bytes, cfg_.ec_level));
    if (static_cast<int>(pending_send_batch_.size()) >= cfg_.qr_codes_per_frame) {
        flush_send_batch();
    }
}

void OpticalLink::flush_send_batch() {
    if (pending_send_batch_.empty()) return;

    const int n = static_cast<int>(pending_send_batch_.size());
    // Simple square-ish grid — fine for the small N (2-4) this is meant for; not worth a smarter
    // bin-packer for a handful of cells.
    const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    const int rows = static_cast<int>(std::ceil(static_cast<double>(n) / cols));

    const int quiet = 4;
    const int cell_target = static_cast<int>(0.85 * std::min(screen_w_ / cols, screen_h_ / rows));

    // Each QR gets its own scale/side computed independently (versions can differ — e.g. a
    // retransmit queued alongside fresh sends carries whatever payload size it was originally
    // split to, not necessarily today's chunk size), same per-module-pixel logic as the classic
    // path. All cells share the *largest* side so the grid stays uniform; smaller QRs are centered
    // within their cell rather than stretched.
    std::vector<int> sides(pending_send_batch_.size());
    int max_side = 0, max_scale = 0;
    for (size_t i = 0; i < pending_send_batch_.size(); ++i) {
        const int modules_per_side = pending_send_batch_[i].width + 2 * quiet;
        const int scale = std::max(4, cell_target / modules_per_side);
        sides[i] = modules_per_side * scale;
        max_side = std::max(max_side, sides[i]);
        max_scale = std::max(max_scale, scale);
    }

    cv::Mat canvas(max_side * rows, max_side * cols, CV_8UC1, cv::Scalar(255));
    for (size_t i = 0; i < pending_send_batch_.size(); ++i) {
        const auto& qr = pending_send_batch_[i];
        const int modules_per_side = qr.width + 2 * quiet;
        cv::Mat small(modules_per_side, modules_per_side, CV_8UC1, cv::Scalar(255));
        for (int y = 0; y < qr.width; ++y) {
            uint8_t* row = small.ptr<uint8_t>(y + quiet) + quiet;
            for (int x = 0; x < qr.width; ++x) {
                row[x] = qr.modules[static_cast<size_t>(y) * qr.width + x] ? 0 : 255;
            }
        }
        cv::Mat one;
        cv::resize(small, one, cv::Size(sides[i], sides[i]), 0, 0, cv::INTER_NEAREST);

        const int cell_row = static_cast<int>(i) / cols;
        const int cell_col = static_cast<int>(i) % cols;
        const int off_x = cell_col * max_side + (max_side - sides[i]) / 2;
        const int off_y = cell_row * max_side + (max_side - sides[i]) / 2;
        one.copyTo(canvas(cv::Rect(off_x, off_y, sides[i], sides[i])));
    }

    // Same fixed full-screen window and persistent canvas as the classic path (see send()) — pad
    // this grid onto it instead of resizing/moving the window, or reallocating/re-filling a new
    // canvas, to match it.
    show_padded(*screen_canvas(out_canvas_), last_content_w_, last_content_h_, screen_w_, screen_h_, canvas);
    cv::imshow(out_window_name_, *screen_canvas(out_canvas_));
    if (cfg_.log) {
        cfg_.log("[optical] showing " + std::to_string(n) + " QR(s) in a " + std::to_string(cols) + "x" +
                  std::to_string(rows) + " grid, max_module_px=" + std::to_string(max_scale));
    }
    cv::waitKey(static_cast<int>(1000.0 / std::max(1.0, cfg_.fps)));
    pending_send_batch_.clear();
}

bool OpticalLink::try_receive_multi(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) {
    using Clock = std::chrono::steady_clock;
    // A batch send() queued but didn't fill (e.g. the window-fill loop ran out of new offsets
    // before reaching qr_codes_per_frame) must actually reach the screen before we start waiting
    // for a reply to it — same invariant the classic path gets for free by rendering immediately.
    flush_send_batch();

    if (!pending_decoded_.empty()) {
        out = std::move(pending_decoded_.front());
        pending_decoded_.pop_front();
        return true;
    }

    auto* ct = capture_thread(capture_thread_);
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        cv::Mat gray;
        double capture_ms = 0.0;
        bool got_new = false;
        {
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
            if (++preview_frame_counter_ % 3 == 0) cv::imshow(camera_window_name_, gray);

            const auto t_decode_start = Clock::now();
            const cv::Mat decode_input = downscale_for_decode(gray);
            std::vector<std::string> decoded_texts;
            if (cfg_.use_wechat_qrcode) {
                // Its detectAndDecode() already returns every code found in one call — no separate
                // "multi" entry point needed, unlike the classic detector.
                decoded_texts = wechat_det(wechat_detector_)->detectAndDecode(decode_input);
            } else {
                det(detector_)->detectAndDecodeMulti(decode_input, decoded_texts);
            }
            decode_ms_accum_ += std::chrono::duration<double, std::milli>(Clock::now() - t_decode_start).count();
            ++poll_count_since_last_decode_;

            // Dedup against "what's currently on screen" the same way the classic path does with
            // a single hash, just generalized to a set: a capture can legitimately return anywhere
            // from 0 to N decoded strings (motion blur or a bad angle can cost just one QR in the
            // grid, not necessarily all of them), so each decoded string is judged independently
            // rather than requiring the whole batch to match before accepting any of it.
            std::vector<uint32_t> current_hashes;
            std::vector<std::vector<uint8_t>> new_payloads;
            for (const auto& text : decoded_texts) {
                if (text.empty()) continue;
                const uint32_t h = crc32(reinterpret_cast<const uint8_t*>(text.data()), text.size());
                current_hashes.push_back(h);
                if (std::find(last_decoded_hashes_.begin(), last_decoded_hashes_.end(), h) ==
                    last_decoded_hashes_.end()) {
                    new_payloads.push_back(base64_decode(text));
                }
            }
            if (!current_hashes.empty()) last_decoded_hashes_ = std::move(current_hashes);

            if (!new_payloads.empty()) {
                if (cfg_.log) {
                    const int n = std::max(1, poll_count_since_last_decode_);
                    cfg_.log("[optical] camera read " + std::to_string(new_payloads.size()) + " NEW QR(s) (of " +
                              std::to_string(decoded_texts.size()) + " decoded this capture) avg_capture_ms=" +
                              std::to_string(capture_ms_accum_ / n) + " avg_decode_ms=" +
                              std::to_string(decode_ms_accum_ / n));
                }
                capture_ms_accum_ = 0.0;
                decode_ms_accum_ = 0.0;
                poll_count_since_last_decode_ = 0;

                for (auto& payload : new_payloads) pending_decoded_.push_back(std::move(payload));
                out = std::move(pending_decoded_.front());
                pending_decoded_.pop_front();
                cv::waitKey(1);
                return true;
            }
        }
        cv::waitKey(1);
    }
    return false;
}

} // namespace qrtp
#endif // QRTP_WITH_OPENCV
