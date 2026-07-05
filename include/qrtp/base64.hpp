#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace qrtp {

// Used at the OpticalLink boundary specifically: cv::QRCodeDetector::detectAndDecode returns a
// std::string, and several QR/barcode decoder implementations (OpenCV's included) build that
// string in ways that aren't guaranteed safe for arbitrary binary content — in particular, our
// protocol frames are full of embedded zero bytes (e.g. a SYN frame's seq field is always 0x00
// 0x00 0x00 0x00), which is exactly the kind of content a C-string-style decode path can silently
// truncate at. Base64 avoids the whole question: the QR only ever carries printable ASCII with no
// embedded NUL, so it doesn't matter whether the decoder's string handling is fully binary-safe.
std::string base64_encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64_decode(const std::string& text);

} // namespace qrtp
