#pragma once
#include <cstdint>
#include <vector>

namespace qrtp {

// Consistent Overhead Byte Stuffing (Cheshire & Baker, 1999): eliminates every 0x00 byte from
// arbitrary binary data at a bounded, low cost (at most 1 extra byte per 254 input bytes), in
// exchange for a guarantee the output never contains a zero byte. Used at the OpticalLink
// boundary, replacing an earlier Base64 scheme (see not_working.md, 2026-07-05): our protocol
// frames are full of embedded zero bytes (e.g. a SYN frame's seq field is always 0x00 0x00 0x00
// 0x00), and cv::QRCodeDetector::detectAndDecode returns a std::string built in a way that isn't
// guaranteed safe for those — but that failure mode is specifically about zero bytes, not binary
// content in general, so removing just the zero bytes is enough to avoid it, at a small fraction
// of Base64's ~33% expansion (Base64 additionally restricts the output to printable text, which
// this doesn't need to and doesn't — more real file bytes fit in the same QR capacity).
std::vector<uint8_t> cobs_encode(const std::vector<uint8_t>& data);

// Returns an empty vector if `data` is malformed (truncated mid-block) — never trust decoded QR
// content, same posture as decode_frame() in framing.cpp.
std::vector<uint8_t> cobs_decode(const std::vector<uint8_t>& data);

} // namespace qrtp
