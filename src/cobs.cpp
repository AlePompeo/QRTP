#include "qrtp/cobs.hpp"

namespace qrtp {

std::vector<uint8_t> cobs_encode(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    out.reserve(data.size() + data.size() / 254 + 1);
    out.push_back(0); // placeholder for the first code byte, patched in below
    size_t code_index = 0;
    uint8_t code = 1;
    for (uint8_t byte : data) {
        if (byte == 0) {
            out[code_index] = code;
            code = 1;
            code_index = out.size();
            out.push_back(0); // placeholder for the next code byte
        } else {
            out.push_back(byte);
            if (++code == 0xFF) {
                out[code_index] = code;
                code = 1;
                code_index = out.size();
                out.push_back(0);
            }
        }
    }
    out[code_index] = code;
    return out;
}

std::vector<uint8_t> cobs_decode(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    out.reserve(data.size());
    size_t i = 0;
    while (i < data.size()) {
        const uint8_t code = data[i];
        if (code == 0 || i + code - 1 > data.size()) return {}; // malformed: truncated block
        ++i;
        for (uint8_t j = 1; j < code; ++j) out.push_back(data[i++]);
        if (code != 0xFF && i < data.size()) out.push_back(0);
    }
    return out;
}

} // namespace qrtp
