// Emscripten/WASM implementation of qrtp/qr_encoder.hpp's qr_encode_bytes(). Alternate to
// qr_encoder.cpp (which dlopen's the system libqrencode) — WASM has no dlopen, so this links
// libqrencode statically instead (see third_party/libqrencode, built alongside this target; see
// CMakeLists.txt's WITH_WASM block). Never compiled together with qr_encoder.cpp: the CMake
// target picks exactly one of the two, same pattern as gui_win32.cpp vs gui_gtk.cpp picking one
// GUI backend per platform.
//
// write_pbm() is intentionally not defined here: it's only called by the CLI's `render-qr`
// command (main.cpp), which isn't part of the qrtp_web target, so the linker never needs it.
#include "qrtp/qr_encoder.hpp"

#include <qrencode.h>

#include <stdexcept>

namespace qrtp {

QrMatrix qr_encode_bytes(const std::vector<uint8_t>& data, QrEcLevel level) {
    // version=0 tells libqrencode to pick the smallest version that fits the payload — same call
    // shape as qr_encoder.cpp's dlopen'd version, just linked directly instead of through a
    // function pointer.
    QRcode* qr = QRcode_encodeData(static_cast<int>(data.size()), data.data(), 0,
                                    static_cast<QRecLevel>(level));
    if (!qr) throw std::runtime_error("qr_encoder_wasm: encoding failed (payload likely too large for a QR code)");

    QrMatrix out;
    out.version = qr->version;
    out.width = qr->width;
    out.modules.resize(static_cast<size_t>(qr->width) * static_cast<size_t>(qr->width));
    for (size_t i = 0; i < out.modules.size(); ++i) {
        out.modules[i] = qr->data[i] & 0x01u; // bit 0 = dark module; other bits are library-internal
    }
    QRcode_free(qr);

    if (out.width != 4 * out.version + 17) {
        throw std::runtime_error("qr_encoder_wasm: unexpected width/version relationship");
    }
    return out;
}

} // namespace qrtp
