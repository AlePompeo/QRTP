// Browser/WASM entry point for QRTP — new, additive file, no existing source touched.
//
// The whole point of this file is how little of it there is: `Sender`/`Receiver`
// (protocol.cpp) and `BroadcastSender`/`BroadcastReceiver` (broadcast.cpp) only ever depend on
// `ILink&` (link.hpp), never on `OpticalLink` directly — the exact same boundary that already let
// `LoopbackLink` validate the whole protocol (handshake, sliding window, SACK, AIMD) with no
// hardware. `JsLink` below is a second `ILink` implementation, this time backed by a browser tab
// instead of a camera+monitor, and everything above that boundary (protocol/adaptive/framing/
// broadcast/crc32/sha256/base64) is reused completely unmodified.
//
// send() renders a QR to a <canvas> via a JS callback; try_receive() is the one genuinely new
// problem this target has that the native build doesn't: it must block (with a timeout) waiting
// for the next decoded camera frame, but a browser's main thread cannot block synchronously
// without freezing the UI (and the camera capture loop that would deliver that very frame).
// Emscripten's Asyncify (-sASYNCIFY, see CMakeLists.txt's WITH_WASM block) lets `emscripten::val`
// "await" a JS Promise from what still reads as ordinary, synchronous-looking C++ — so
// Sender::send_file/Receiver::receive_file's existing blocking loops work completely unchanged,
// suspended and resumed by the browser's own event loop instead of a real OS thread wait.
//
// The QR payload itself is base64 text, exactly like optical_link.cpp's send()/try_receive() (see
// their comments): our frames are dense binary with embedded NUL bytes, and neither a JS string
// nor zxing-cpp's text-mode decode is guaranteed safe with those — base64 sidesteps the question
// on both platforms identically, so the wire format is unchanged crossing from native to web.
#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <ReadBarcode.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "qrtp/adaptive.hpp"
#include "qrtp/base64.hpp"
#include "qrtp/broadcast.hpp"
#include "qrtp/framing.hpp"
#include "qrtp/link.hpp"
#include "qrtp/protocol.hpp"
#include "qrtp/qr_encoder.hpp"

using emscripten::val;

namespace qrtp {
namespace web {

namespace {

// web/main.js sets Module.qrtpBridge (see there) to the JS-side half of the boundary: renderQr/
// waitForFrame/sleep/setFps/setEcLevel/onLog. module_property() (rather than a page-global like
// window.qrtpBridge) reads it off *this* WASM instance's own Module object, so two independently-
// instantiated modules in the same JS realm never see each other's bridge — irrelevant in the
// browser (one instance per tab) but what makes the two-instance round-trip sanity check possible
// (see PROGRESS.md), same in-process role LoopbackLink plays for the native protocol tests.
val bridge() { return val::module_property("qrtpBridge"); }

} // namespace

class JsLink : public ILink {
public:
    void send(const std::vector<uint8_t>& frame_bytes) override {
        const std::string encoded_text = base64_encode(frame_bytes);
        const std::vector<uint8_t> encoded_bytes(encoded_text.begin(), encoded_text.end());
        const QrMatrix qr = qr_encode_bytes(encoded_bytes, static_cast<QrEcLevel>(static_cast<uint8_t>(ec_level_)));
        val modules = val::array();
        for (size_t i = 0; i < qr.modules.size(); ++i) modules.set(i, qr.modules[i]);
        bridge().call<void>("renderQr", qr.width, modules);
        // Dwell ~1000/fps ms after painting, mirroring OpticalLink::send()'s own
        // cv::waitKey(1000.0/fps) pacing: Sender/Receiver's ARQ timing (see protocol.cpp's
        // Receiver comment on SACK cadence) assumes send() itself takes about this long to
        // return, not something layered on separately by the caller.
        const int dwell_ms = static_cast<int>(1000.0 / std::max(1.0, fps_));
        bridge().call<val>("sleep", dwell_ms).await();
    }

    bool try_receive(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) override {
        // Asyncify suspends the whole call stack up to whatever exported WebSession method
        // started this (run_send/run_receive/...) here, and resumes it once the JS Promise
        // settles — see the file header comment. waitForFrame() itself resolves with "" if
        // `timeout` elapses with no new decode, mirroring try_receive()'s own timeout contract.
        val result = bridge().call<val>("waitForFrame", static_cast<int>(timeout.count())).await();
        const std::string text = result.as<std::string>();
        if (text.empty()) return false;
        out = base64_decode(text);
        return true;
    }

    void set_fps(double fps) override {
        fps_ = fps;
        bridge().call<void>("setFps", fps);
    }
    void set_ec_level(ECLevel level) override {
        ec_level_ = level;
        bridge().call<void>("setEcLevel", static_cast<int>(level));
    }

private:
    ECLevel ec_level_ = ECLevel::M;
    double fps_ = 10.0;
};


// Decodes one captured camera frame. `ptr` points at `width*height*4` bytes of RGBA pixel data
// that JS has already written into the WASM heap (via Module._malloc + HEAPU8.set) — passed as a
// raw pointer rather than through embind's usual vector marshaling so a multi-megapixel frame
// isn't copied element-by-element on every poll of what's effectively a tight camera-rate loop.
// Returns the decoded base64 text, or "" if no QR was found this frame. Purely synchronous: no
// Asyncify needed here, only try_receive()'s *wait* for the next one is actually asynchronous.
std::string decode_qr_rgba(uintptr_t ptr, int width, int height) {
    const auto* data = reinterpret_cast<const uint8_t*>(ptr);
    const ZXing::ImageView view(data, width, height, ZXing::ImageFormat::RGBA);
    ZXing::ReaderOptions opts;
    opts.setFormats(ZXing::BarcodeFormat::QRCode);
    // Every captured frame gets its own decode attempt at typical camera fps (see AdaptiveConfig::
    // fps_initial) — favor per-attempt speed over exhaustively trying every rotation/inversion,
    // same trade the native path makes by polling continuously rather than trying harder once.
    opts.setTryHarder(false);
    const auto barcode = ZXing::ReadBarcode(view, opts);
    if (!barcode.isValid()) return std::string();
    return barcode.text();
}

// One transfer session's worth of state: just the cooperative-cancellation flag every
// Sender/Receiver/BroadcastSender/BroadcastReceiver already accepts (ProtocolConfig::cancel /
// BroadcastConfig::cancel) — same role as the "Stop" button in gui_win32.cpp/gui_gtk.cpp.
class WebSession {
public:
    // Every method below wraps its whole body in try/catch: -fwasm-exceptions (needed so a throw
    // doesn't permanently abort the whole module -- see PROGRESS.md) reliably keeps the instance
    // alive across a throw, but a C++ exception object crossing the embind boundary into JS
    // arrives as an opaque WebAssembly.Exception with no usable .message (confirmed empirically,
    // not everywhere embind's usual exception-to-JS-Error translation applies under this EH mode)
    // -- so exceptions are caught and converted to a normal failure result *before* they'd ever
    // cross that boundary, exactly like Sender::send_file already does for a plain "file not
    // found" (both are just different flavors of "this attempt didn't work," reported the same
    // way to the caller).
    TransferResult run_send(const std::string& path, double fps, int chunk_max, int ec_level) {
        try {
            cancel_flag_.store(false);
            JsLink link;
            ProtocolConfig cfg;
            cfg.adaptive.fps_initial = fps;
            cfg.adaptive.chunk_size_initial = static_cast<size_t>(chunk_max);
            cfg.adaptive.chunk_size_max = static_cast<size_t>(chunk_max);
            cfg.adaptive.ec_level_initial = static_cast<ECLevel>(ec_level);
            cfg.cancel = &cancel_flag_;
            cfg.log = [](const std::string& msg) { bridge().call<void>("onLog", msg); };
            Sender sender(link, cfg);
            return sender.send_file(path);
        } catch (const std::exception& e) {
            return {false, std::string("exception: ") + e.what()};
        }
    }

    val run_receive(double fps, int timeout_minutes) {
        try {
            cancel_flag_.store(false);
            JsLink link;
            ProtocolConfig cfg;
            cfg.adaptive.fps_initial = fps;
            cfg.cancel = &cancel_flag_;
            cfg.log = [](const std::string& msg) { bridge().call<void>("onLog", msg); };
            Receiver receiver(link, cfg);
            TransferMetadata meta;
            auto bytes = receiver.receive_file(meta, std::chrono::minutes(timeout_minutes));

            val out = val::object();
            out.set("success", bytes.has_value());
            out.set("filename", meta.filename);
            if (bytes.has_value()) {
                out.set("data", val(emscripten::typed_memory_view(bytes->size(), bytes->data())).call<val>("slice"));
            }
            return out;
        } catch (const std::exception& e) {
            val out = val::object();
            out.set("success", false);
            out.set("filename", std::string());
            out.set("message", std::string("exception: ") + e.what());
            return out;
        }
    }

    BroadcastResult run_broadcast_send(const std::string& path, double fps, int chunk_size, int ec_level, int loops) {
        try {
            cancel_flag_.store(false);
            JsLink link;
            BroadcastConfig cfg;
            cfg.chunk_size = static_cast<size_t>(chunk_size);
            cfg.ec_level = static_cast<ECLevel>(ec_level);
            cfg.fps = fps;
            cfg.loops = loops;
            cfg.cancel = &cancel_flag_;
            cfg.log = [](const std::string& msg) { bridge().call<void>("onLog", msg); };
            BroadcastSender sender(link, cfg);
            return sender.send_file(path);
        } catch (const std::exception& e) {
            return {false, std::string("exception: ") + e.what()};
        }
    }

    val run_broadcast_receive(double fps, int timeout_minutes) {
        try {
            cancel_flag_.store(false);
            JsLink link;
            BroadcastConfig cfg;
            cfg.fps = fps;
            cfg.cancel = &cancel_flag_;
            cfg.log = [](const std::string& msg) { bridge().call<void>("onLog", msg); };
            BroadcastReceiver receiver(link, cfg);
            TransferMetadata meta;
            auto bytes = receiver.receive_file(meta, std::chrono::minutes(timeout_minutes));

            val out = val::object();
            out.set("success", bytes.has_value());
            out.set("filename", meta.filename);
            if (bytes.has_value()) {
                out.set("data", val(emscripten::typed_memory_view(bytes->size(), bytes->data())).call<val>("slice"));
            }
            return out;
        } catch (const std::exception& e) {
            val out = val::object();
            out.set("success", false);
            out.set("filename", std::string());
            out.set("message", std::string("exception: ") + e.what());
            return out;
        }
    }

    void request_cancel() { cancel_flag_.store(true); }

private:
    std::atomic<bool> cancel_flag_{false};
};

} // namespace web
} // namespace qrtp

EMSCRIPTEN_BINDINGS(qrtp_web) {
    emscripten::value_object<qrtp::TransferResult>("TransferResult")
        .field("success", &qrtp::TransferResult::success)
        .field("message", &qrtp::TransferResult::message);
    emscripten::value_object<qrtp::BroadcastResult>("BroadcastResult")
        .field("success", &qrtp::BroadcastResult::success)
        .field("message", &qrtp::BroadcastResult::message);

    emscripten::function("decodeQrRgba", &qrtp::web::decode_qr_rgba);

    emscripten::class_<qrtp::web::WebSession>("WebSession")
        .constructor<>()
        .function("runSend", &qrtp::web::WebSession::run_send)
        .function("runReceive", &qrtp::web::WebSession::run_receive)
        .function("runBroadcastSend", &qrtp::web::WebSession::run_broadcast_send)
        .function("runBroadcastReceive", &qrtp::web::WebSession::run_broadcast_receive)
        .function("requestCancel", &qrtp::web::WebSession::request_cancel);
}

#endif // __EMSCRIPTEN__
