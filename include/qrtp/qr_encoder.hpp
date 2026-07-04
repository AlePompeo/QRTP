#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace qrtp {

enum class QrEcLevel : int { L = 0, M = 1, Q = 2, H = 3 };

struct QrMatrix {
    int version = 0;
    int width = 0;                // modules per side
    std::vector<uint8_t> modules; // width*width, 1 = dark module, 0 = light
};

// Renders `data` as a real QR code via the system's libqrencode (loaded at runtime with dlopen —
// see qr_encoder.cpp for why). Throws std::runtime_error if the library can't be loaded/linked, or
// if the payload doesn't fit in a version-40 symbol at the requested EC level.
QrMatrix qr_encode_bytes(const std::vector<uint8_t>& data, QrEcLevel level = QrEcLevel::M);

// Writes a QrMatrix as a binary-ASCII PBM image (portable, no image library needed to view/verify
// it — e.g. `convert out.pbm out.png` or most image viewers open PBM directly).
void write_pbm(const QrMatrix& qr, const std::string& path, int scale = 8, int quiet_zone = 4);

} // namespace qrtp
