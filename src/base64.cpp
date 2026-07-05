#include "qrtp/base64.hpp"

namespace qrtp {
namespace {

constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decode_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

std::string base64_encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= data.size()) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back(kTable[n & 0x3F]);
        i += 3;
    }

    const size_t rem = data.size() - i;
    if (rem == 1) {
        const uint32_t n = uint32_t(data[i]) << 16;
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::vector<uint8_t> base64_decode(const std::string& text) {
    std::vector<uint8_t> out;
    out.reserve((text.size() / 4) * 3);

    int vals[4];
    size_t vi = 0;
    for (unsigned char c : text) {
        if (c == '=') break;
        const int v = decode_char(c);
        if (v < 0) continue; // skip anything that isn't a base64 character (whitespace, etc.)
        vals[vi++] = v;
        if (vi == 4) {
            const uint32_t n = (uint32_t(vals[0]) << 18) | (uint32_t(vals[1]) << 12) | (uint32_t(vals[2]) << 6) |
                                uint32_t(vals[3]);
            out.push_back(uint8_t(n >> 16));
            out.push_back(uint8_t(n >> 8));
            out.push_back(uint8_t(n));
            vi = 0;
        }
    }
    if (vi == 2) {
        const uint32_t n = (uint32_t(vals[0]) << 18) | (uint32_t(vals[1]) << 12);
        out.push_back(uint8_t(n >> 16));
    } else if (vi == 3) {
        const uint32_t n = (uint32_t(vals[0]) << 18) | (uint32_t(vals[1]) << 12) | (uint32_t(vals[2]) << 6);
        out.push_back(uint8_t(n >> 16));
        out.push_back(uint8_t(n >> 8));
    }
    return out;
}

} // namespace qrtp
