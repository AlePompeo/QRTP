#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qrtp {

// End-to-end file integrity check (independent of the per-frame CRC32).
std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len);
std::string sha256_hex(const std::array<uint8_t, 32>& digest);

} // namespace qrtp
