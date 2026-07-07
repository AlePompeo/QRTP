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
extern "C" {
#include <jabcode.h> // pure C11 header, no __cplusplus guard of its own -- wrap it here so the
                      // symbols compiled into libjabcode.a (plain C, unmangled) actually match.
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>
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

// EXPERIMENTAL, opt-in (color_multiplex=true): instead of one QR per displayed frame, encodes up
// to 3 independent QR codes into the R, G, B channels of a single color image — the exact same
// role OpticalLink::send_multi()/try_receive_multi() play for the native spatial-grid multi-QR
// mode (qr_codes_per_frame), just batching by color channel instead of screen position. Web-only:
// this project already found (real hardware) that zeroing camera saturation improves classic QR
// reliability ("color is noise") — this feature makes the opposite bet, so it stays opt-in/
// unverified-on-real-hardware rather than a default, and native GUIs aren't touched at all.
class JsLink : public ILink {
public:
    explicit JsLink(bool color_multiplex = false, bool jabcode = false)
        : color_multiplex_(color_multiplex), jabcode_(jabcode) {}

    void send(const std::vector<uint8_t>& frame_bytes) override {
        if (jabcode_) {
            send_jabcode(frame_bytes);
            return;
        }
        if (!color_multiplex_) {
            send_one(frame_bytes);
            return;
        }
        const std::string encoded_text = base64_encode(frame_bytes);
        const std::vector<uint8_t> encoded_bytes(encoded_text.begin(), encoded_text.end());
        pending_send_batch_.push_back(
            qr_encode_bytes(encoded_bytes, static_cast<QrEcLevel>(static_cast<uint8_t>(ec_level_))));
        if (pending_send_batch_.size() >= 3) flush_send_batch();
        // Otherwise: wait for more before showing anything — see try_receive()'s flush-on-stall
        // note below for why an incomplete batch never gets stuck unshown in bidirectional mode.
    }

    bool try_receive(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) override {
        if (jabcode_) return try_receive_jabcode(out, timeout);
        if (!color_multiplex_) return try_receive_one(out, timeout);

        // The caller is about to block waiting for a reply to whatever it just sent — anything
        // still sitting in the batch (1 or 2 QRs, never reached 3) must go out now, or the other
        // side can never see it at all. Correct for both roles: Sender bursts several send()s
        // then waits for a SACK; Receiver's own ACK/SACK sends interleave with waiting for DATA.
        if (!pending_send_batch_.empty()) flush_send_batch();

        if (!pending_decoded_.empty()) {
            out = base64_decode(pending_decoded_.front());
            pending_decoded_.pop_front();
            return true;
        }

        val result = bridge().call<val>("waitForFrameColor", static_cast<int>(timeout.count())).await();
        const unsigned len = result["length"].as<unsigned>();
        if (len == 0) return false;
        for (unsigned i = 0; i < len; ++i) pending_decoded_.push_back(result[i].as<std::string>());

        out = base64_decode(pending_decoded_.front());
        pending_decoded_.pop_front();
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
    void send_one(const std::vector<uint8_t>& frame_bytes) {
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

    bool try_receive_one(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) {
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

    void flush_send_batch() {
        if (pending_send_batch_.empty()) return;
        val channels = val::array();
        for (size_t c = 0; c < pending_send_batch_.size(); ++c) {
            const auto& qr = pending_send_batch_[c];
            val modules = val::array();
            for (size_t i = 0; i < qr.modules.size(); ++i) modules.set(i, qr.modules[i]);
            val entry = val::object();
            entry.set("width", qr.width);
            entry.set("modules", modules);
            channels.set(c, entry);
        }
        bridge().call<void>("renderQrColor", channels);
        // One dwell for the whole composite frame (not once per channel) — this is still a single
        // displayed frame, same pacing contract as send_one() above.
        const int dwell_ms = static_cast<int>(1000.0 / std::max(1.0, fps_));
        bridge().call<val>("sleep", dwell_ms).await();
        pending_send_batch_.clear();
    }

    // EXPERIMENTAL: JAB Code (github.com/jabcode/jabcode, MIT) — a genuinely different barcode
    // symbology from QR, not a variant of it: each module holds one of up to 256 colors directly
    // (log2(N) bits/module, its own LDPC error correction) instead of a black/white bit, roughly
    // tripling per-frame capacity at the default 8-color palette used here. Unlike the QR-based
    // paths above, jab_data is raw-bytes-plus-length with no text/NUL assumption anywhere in the
    // library, so frame bytes go in directly with no base64 detour — also saves the ~33% base64
    // overhead the QR paths pay. Mutually exclusive with color_multiplex_ by construction (only
    // one of the two experimental opt-ins is ever active at once, enforced in web/main.js).
    void send_jabcode(const std::vector<uint8_t>& frame_bytes) {
        jab_data* data = static_cast<jab_data*>(std::malloc(sizeof(jab_data) + frame_bytes.size()));
        if (!data) throw std::runtime_error("jabcode: out of memory building jab_data");
        data->length = static_cast<jab_int32>(frame_bytes.size());
        std::memcpy(data->data, frame_bytes.data(), frame_bytes.size());

        jab_encode* enc = createEncode(8, 1); // 8-color palette, single (non-cascaded) symbol
        if (!enc) {
            std::free(data);
            throw std::runtime_error("jabcode: createEncode failed");
        }
        // generateJABCode does NOT follow this library's usual JAB_SUCCESS(1)/JAB_FAILURE(0)
        // convention (verified by reading encoder.c, not assumed from the header comment alone):
        // it returns 0 on success and a specific non-zero error code per failure path.
        const jab_int32 status = generateJABCode(enc, data);
        std::free(data);
        if (status != 0) {
            destroyEncode(enc);
            throw std::runtime_error("jabcode: generateJABCode failed with code " + std::to_string(status));
        }

        // jab_bitmap's layout (32bpp, RGBA, 4 channels — see jabcode.h) is bit-for-bit identical
        // to the canvas ImageData format already used everywhere else in this file, so the pixel
        // buffer goes straight to JS with no conversion, just a safe out-of-heap copy (same
        // typed_memory_view+slice pattern already used by run_receive()/run_broadcast_receive()
        // to return the assembled file, for the same reason: never hand JS a raw view into wasm
        // memory that could be invalidated, and never round-trip binary data through a JS string).
        const jab_bitmap* bmp = enc->bitmap;
        const size_t pixel_bytes = static_cast<size_t>(bmp->width) * static_cast<size_t>(bmp->height) * 4;
        val pixels = val(emscripten::typed_memory_view(pixel_bytes, bmp->pixel)).call<val>("slice");
        bridge().call<void>("renderJabCode", bmp->width, bmp->height, pixels);
        destroyEncode(enc);

        // Same dwell pacing contract as send_one()/flush_send_batch() above.
        const int dwell_ms = static_cast<int>(1000.0 / std::max(1.0, fps_));
        bridge().call<val>("sleep", dwell_ms).await();
    }

    bool try_receive_jabcode(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) {
        val result = bridge().call<val>("waitForFrameJabCode", static_cast<int>(timeout.count())).await();
        const unsigned len = result["length"].as<unsigned>();
        if (len == 0) return false;
        // Payloads here are protocol frames (hundreds of bytes), not multi-megapixel camera
        // frames — no need for decode_qr_rgba's raw-pointer/no-copy treatment at this scale.
        out.resize(len);
        for (unsigned i = 0; i < len; ++i) out[i] = result[i].as<uint8_t>();
        return true;
    }

    ECLevel ec_level_ = ECLevel::M;
    double fps_ = 10.0;
    bool color_multiplex_ = false;
    bool jabcode_ = false;
    std::vector<QrMatrix> pending_send_batch_;  // up to 3, R/G/B — color_multiplex_ only
    std::deque<std::string> pending_decoded_;   // decoded base64 texts not yet returned — color_multiplex_ only
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

// EXPERIMENTAL color-multiplex counterpart to decode_qr_rgba(): decodes each of the R/G/B channels
// of the same captured RGBA frame as an *independent* grayscale QR image, returning a JS array of
// exactly 3 strings ("" for a channel with no valid QR this frame). No pixel copy needed: each
// channel is viewed in place via ImageView's own pixStride/rowStride override — starting at
// data+channel and stepping 4 bytes per pixel (skipping the other 3 channels) is indistinguishable
// to ZXing from a genuinely separate single-channel (Lum) image occupying that same memory.
val decode_qr_channels_rgba(uintptr_t ptr, int width, int height) {
    const auto* data = reinterpret_cast<const uint8_t*>(ptr);
    val out = val::array();
    for (int channel = 0; channel < 3; ++channel) {
        const ZXing::ImageView view(data + channel, width, height, ZXing::ImageFormat::Lum,
                                     /*rowStride=*/width * 4, /*pixStride=*/4);
        ZXing::ReaderOptions opts;
        opts.setFormats(ZXing::BarcodeFormat::QRCode);
        opts.setTryHarder(false);
        const auto barcode = ZXing::ReadBarcode(view, opts);
        out.set(channel, barcode.isValid() ? barcode.text() : std::string());
    }
    return out;
}

// EXPERIMENTAL counterpart to decode_qr_rgba()/decode_qr_channels_rgba() for the JAB Code path
// (see JsLink::send_jabcode()'s comment). jab_bitmap's layout (32bpp, RGBA, 4 channels) is
// bit-for-bit identical to the RGBA buffer `ptr` already points at, so building one is a single
// memcpy into a jab_bitmap-shaped allocation (its flexible array member rules out a plain struct
// copy). Returns the decoded bytes as a JS Uint8Array, never a std::string — jab_data is raw bytes
// with no text assumption, and a std::string would hit the same TextDecoder/NUL-byte hazard noted
// throughout this file for the QR paths (see decode_qr_rgba's comment).
val decode_jabcode_rgba(uintptr_t ptr, int width, int height) {
    const size_t pixel_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    jab_bitmap* bitmap = static_cast<jab_bitmap*>(std::malloc(sizeof(jab_bitmap) + pixel_bytes));
    if (!bitmap) return val::array();
    bitmap->width = width;
    bitmap->height = height;
    bitmap->bits_per_pixel = 32;
    bitmap->bits_per_channel = 8;
    bitmap->channel_count = 4;
    std::memcpy(bitmap->pixel, reinterpret_cast<const void*>(ptr), pixel_bytes);

    jab_int32 status = 0;
    jab_data* decoded = decodeJABCode(bitmap, NORMAL_DECODE, &status);
    std::free(bitmap);
    if (!decoded) return val::array();

    val out = val(emscripten::typed_memory_view(static_cast<size_t>(decoded->length),
                                                 reinterpret_cast<uint8_t*>(decoded->data)))
                  .call<val>("slice");
    std::free(decoded);
    return out;
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
    TransferResult run_send(const std::string& path, double fps, int chunk_max, int ec_level,
                             bool color_multiplex, bool jabcode) {
        try {
            cancel_flag_.store(false);
            JsLink link(color_multiplex, jabcode);
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

    val run_receive(double fps, int timeout_minutes, bool color_multiplex, bool jabcode) {
        try {
            cancel_flag_.store(false);
            JsLink link(color_multiplex, jabcode);
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

    BroadcastResult run_broadcast_send(const std::string& path, double fps, int chunk_size, int ec_level, int loops,
                                        bool color_multiplex, bool jabcode) {
        try {
            cancel_flag_.store(false);
            JsLink link(color_multiplex, jabcode);
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

    val run_broadcast_receive(double fps, int timeout_minutes, bool color_multiplex, bool jabcode) {
        try {
            cancel_flag_.store(false);
            JsLink link(color_multiplex, jabcode);
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
    emscripten::function("decodeQrChannelsRgba", &qrtp::web::decode_qr_channels_rgba);
    emscripten::function("decodeJabcodeRgba", &qrtp::web::decode_jabcode_rgba);

    emscripten::class_<qrtp::web::WebSession>("WebSession")
        .constructor<>()
        .function("runSend", &qrtp::web::WebSession::run_send)
        .function("runReceive", &qrtp::web::WebSession::run_receive)
        .function("runBroadcastSend", &qrtp::web::WebSession::run_broadcast_send)
        .function("runBroadcastReceive", &qrtp::web::WebSession::run_broadcast_receive)
        .function("requestCancel", &qrtp::web::WebSession::request_cancel);
}

#endif // __EMSCRIPTEN__
