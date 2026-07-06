#include "qrtp/framing.hpp"

#include <algorithm>

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

void put_u64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 7; i >= 0; --i) b.push_back(uint8_t(v >> (i * 8)));
}

uint16_t get_u16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }

uint32_t get_u32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
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

std::vector<uint8_t> pack_file_metadata(const std::string& filename, uint64_t filesize,
                                          const std::array<uint8_t, 32>& sha) {
    std::vector<uint8_t> out;
    const uint8_t name_len = static_cast<uint8_t>(std::min<size_t>(filename.size(), 255));
    out.push_back(name_len);
    out.insert(out.end(), filename.begin(), filename.begin() + name_len);
    put_u64(out, filesize);
    out.insert(out.end(), sha.begin(), sha.end());
    return out;
}

bool parse_file_metadata(const std::vector<uint8_t>& payload, TransferMetadata& meta) {
    if (payload.empty()) return false;
    const uint8_t name_len = payload[0];
    const size_t needed = size_t(1) + name_len + 8 + 32;
    if (payload.size() < needed) return false;
    meta.filename.assign(payload.begin() + 1, payload.begin() + 1 + name_len);
    meta.filesize = get_u64(&payload[1 + name_len]);
    std::copy(payload.begin() + 1 + name_len + 8, payload.begin() + 1 + name_len + 8 + 32, meta.sha256.begin());
    return true;
}

} // namespace qrtp
