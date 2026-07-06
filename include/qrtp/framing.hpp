#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qrtp {

enum class FrameType : uint8_t {
    SYN = 1,
    SYN_ACK = 2,
    ACK = 3,
    DATA = 4,
    SACK = 5,
    FIN = 6,
    FIN_ACK = 7,
    CALIB = 8,
    PAUSE = 9,
    RESUME = 10,
};

namespace flags {
constexpr uint8_t MORE_FRAGMENTS = 0x01;
constexpr uint8_t LAST_FRAME = 0x02;
constexpr uint8_t RETRANSMIT = 0x04;
constexpr uint8_t CALIBRATION_PATTERN = 0x08;
} // namespace flags

constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 15; // version(1)+type(1)+session(2)+seq(4)+ack_window(4)+flags(1)+len(2)
constexpr size_t kCrcSize = 4;
constexpr size_t kFixedOverhead = kHeaderSize + kCrcSize; // 19 bytes of header + 4 bytes CRC32

// One QR code's worth of data. `seq` is a byte offset within the file for DATA frames (TCP-style
// byte-sequence numbering, not a plain frame counter) — this lets the receiver reassemble the file
// correctly even though the adaptive controller varies chunk size mid-transfer.
struct Frame {
    uint8_t version = kProtocolVersion;
    FrameType type = FrameType::DATA;
    uint16_t session_id = 0;
    uint32_t seq = 0;
    uint32_t ack_window = 0;
    uint8_t flags_byte = 0;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> encode_frame(const Frame& f);

// Returns std::nullopt for anything structurally malformed or CRC-invalid — the caller must never
// trust a frame's fields until this has returned a value (the decoded QR payload is untrusted,
// externally-sourced input, exactly like a byte stream off a socket).
std::optional<Frame> decode_frame(const std::vector<uint8_t>& bytes);

// Describes the file a transfer carries — the parsed result of pack_file_metadata()'s payload.
struct TransferMetadata {
    std::string filename;
    uint64_t filesize = 0;
    std::array<uint8_t, 32> sha256{};
};

// Packs filename/filesize/sha256 into a payload for a SYN-type (session-opening/header) frame —
// shared by the bidirectional Sender/Receiver (protocol.cpp) and the simplex
// BroadcastSender/BroadcastReceiver (broadcast.cpp), both of which need identical file-metadata
// framing but otherwise share no protocol state machine.
std::vector<uint8_t> pack_file_metadata(const std::string& filename, uint64_t filesize,
                                          const std::array<uint8_t, 32>& sha);
bool parse_file_metadata(const std::vector<uint8_t>& payload, TransferMetadata& meta);

} // namespace qrtp
