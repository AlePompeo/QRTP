#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "qrtp/adaptive.hpp" // ECLevel
#include "qrtp/framing.hpp"  // Frame, TransferMetadata, pack/parse_file_metadata
#include "qrtp/link.hpp"     // ILink

namespace qrtp {

struct BroadcastConfig {
    // Fixed for the whole session — there is no feedback channel for an AIMD controller
    // (adaptive.hpp) to adapt on, unlike the bidirectional Sender/Receiver.
    size_t chunk_size = 180; // bytes of file payload per DATA frame
    // Higher than the bidirectional default (ECLevel::M): there is no retransmission safety net
    // here, so a QR that fails to decode isn't retried until the sender's next full loop — more
    // per-QR redundancy directly trades for a better chance each individual read succeeds.
    ECLevel ec_level = ECLevel::Q;
    double fps = 10.0;
    // How many full passes over the file BroadcastSender::send_file() makes before returning on
    // its own; 0 means loop indefinitely until cancelled — the natural default for a continuous
    // broadcast/kiosk-style transmission with no return channel to ever signal "done, stop".
    int loops = 0;
    std::function<void(const std::string&)> log;
    std::atomic<bool>* cancel = nullptr;
};

struct BroadcastResult {
    bool success = false;
    std::string message;
};

// One-way (simplex) transfer, coexisting with the bidirectional Sender/Receiver (protocol.hpp):
// the sender repeatedly displays the whole file as a sequence of QR codes with no expectation of,
// or ability to receive, any reply. Reliability comes entirely from repetition instead of ARQ:
// send_file() loops the whole file (a header frame with filename/size/hash, then every chunk)
// either forever or a fixed number of times; BroadcastReceiver assembles whatever it manages to
// capture across one or more of those loops, finishing the moment it has the complete file with a
// verified SHA-256 hash — it never needs the sender's current loop to finish first.
class BroadcastSender {
public:
    BroadcastSender(ILink& link, BroadcastConfig cfg);
    BroadcastResult send_file(const std::filesystem::path& path);

private:
    ILink& link_;
    BroadcastConfig cfg_;
    uint16_t session_id_;

    void log(const std::string& msg) const;
    bool cancelled() const;
};

class BroadcastReceiver {
public:
    BroadcastReceiver(ILink& link, BroadcastConfig cfg);
    std::optional<std::vector<uint8_t>> receive_file(TransferMetadata& meta_out,
                                                       std::chrono::milliseconds overall_timeout);

private:
    ILink& link_;
    BroadcastConfig cfg_;

    void log(const std::string& msg) const;
    bool cancelled() const;
};

} // namespace qrtp
