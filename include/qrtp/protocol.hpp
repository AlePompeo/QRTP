#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "qrtp/adaptive.hpp"
#include "qrtp/framing.hpp"
#include "qrtp/link.hpp"

namespace qrtp {

struct TransferMetadata {
    std::string filename;
    uint64_t filesize = 0;
    std::array<uint8_t, 32> sha256{};
};

struct ProtocolConfig {
    AdaptiveConfig adaptive;
    std::chrono::milliseconds handshake_timeout{2000};
    std::chrono::milliseconds data_poll_timeout{200};
    int max_handshake_retries = 10;
    int max_data_retries = 10; // per-offset retransmit attempts before a hard failure
    // How many DATA frames the receiver batches before sending a SACK (lower = more responsive,
    // higher = less reverse-channel traffic — real savings only on the actual optical link, since
    // every SACK there costs a full displayed QR frame). Must not exceed the sender's window size:
    // with window W, at most W frames are ever in flight, so batching above W means the receiver
    // can never collect enough frames to trigger a SACK on its own, and every frame ends up
    // paying a full wasted RTO before the immediate-SACK-on-retransmit fallback rescues it —
    // observed as a ~20x throughput collapse once AIMD shrinks the window down to 1. Default of 1
    // (ack every frame) is the only value that's safe regardless of window size.
    uint32_t sack_every_n_frames = 1;
    std::function<void(const std::string&)> log; // optional progress callback
    // Optional cooperative cancellation: if set and becomes true, Sender/Receiver stop at the next
    // safe checkpoint and return a "cancelled" failure/nullopt instead of running to completion or
    // hard timeout. Needed by anything driving a transfer from a UI thread with a "Stop" button,
    // since the alternative (killing the background thread outright) can't safely release the
    // camera/window resources an OpticalLink holds.
    std::atomic<bool>* cancel = nullptr;
};

struct TransferResult {
    bool success = false;
    std::string message;
};

// Drives one file transfer as the initiating side: SYN/SYN_ACK/ACK handshake, then a
// selective-repeat sliding window over the file's bytes, then FIN/FIN_ACK teardown.
class Sender {
public:
    Sender(ILink& link, ProtocolConfig cfg);
    TransferResult send_file(const std::filesystem::path& path);

private:
    ILink& link_;
    ProtocolConfig cfg_;
    AdaptiveController adaptive_;
    uint16_t session_id_;

    void log(const std::string& msg) const;
    bool cancelled() const;
    bool wait_for(FrameType type, uint16_t session_id, Frame& out, std::chrono::milliseconds timeout);
};

// Drives one file transfer as the responding side: waits for SYN, replies SYN_ACK, receives DATA
// frames into a byte-offset-keyed buffer, periodically SACKs, and on FIN verifies the whole file's
// SHA-256 before replying FIN_ACK.
class Receiver {
public:
    Receiver(ILink& link, ProtocolConfig cfg);
    std::optional<std::vector<uint8_t>> receive_file(TransferMetadata& meta_out,
                                                       std::chrono::milliseconds overall_timeout);

private:
    ILink& link_;
    ProtocolConfig cfg_;

    void log(const std::string& msg) const;
    bool cancelled() const;
};

} // namespace qrtp
