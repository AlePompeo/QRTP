#include "qrtp/framing.hpp"

#include "qrtp/crc32.hpp"

namespace qrtp {
namespace {

void put_u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}

void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}

uint16_t get_u16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }

uint32_t get_u32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

} // namespace

std::vector<uint8_t> encode_frame(const Frame& f) {
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + f.payload.size() + kCrcSize);
    out.push_back(f.version);
    out.push_back(uint8_t(f.type));
    put_u16(out, f.session_id);
    put_u32(out, f.seq);
    put_u32(out, f.ack_window);
    out.push_back(f.flags_byte);
    put_u16(out, static_cast<uint16_t>(f.payload.size()));
    out.insert(out.end(), f.payload.begin(), f.payload.end());
    uint32_t crc = crc32(out.data(), out.size());
    put_u32(out, crc);
    return out;
}

std::optional<Frame> decode_frame(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < kFixedOverhead) return std::nullopt;

    const size_t declared_payload_len = get_u16(&bytes[13]);
    const size_t expected_total = kHeaderSize + declared_payload_len + kCrcSize;
    if (bytes.size() != expected_total) return std::nullopt;

    const size_t crc_offset = bytes.size() - kCrcSize;
    const uint32_t received_crc = get_u32(&bytes[crc_offset]);
    const uint32_t computed_crc = crc32(bytes.data(), crc_offset);
    if (received_crc != computed_crc) return std::nullopt;

    Frame f;
    f.version = bytes[0];
    f.type = static_cast<FrameType>(bytes[1]);
    f.session_id = get_u16(&bytes[2]);
    f.seq = get_u32(&bytes[4]);
    f.ack_window = get_u32(&bytes[8]);
    f.flags_byte = bytes[12];
    f.payload.assign(bytes.begin() + kHeaderSize, bytes.begin() + kHeaderSize + declared_payload_len);
    return f;
}

} // namespace qrtp
