#pragma once
#include <cstddef>
#include <cstdint>

namespace qrtp {

// CRC-32/ISO-HDLC (the common "zlib" CRC-32): poly 0xEDB88320, init/final XOR 0xFFFFFFFF.
uint32_t crc32(const uint8_t* data, size_t len);

} // namespace qrtp
