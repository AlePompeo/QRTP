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
    // (adaptive.hpp) to adapt on, unlike the bidirectional Sender/Receiver. Also the source-block
    // size for the LT fountain code (see fountain.hpp) BroadcastSender/BroadcastReceiver use.
    size_t chunk_size = 180; // bytes of file payload per source block / DATA frame
    // Higher than the bidirectional default (ECLevel::M): there is no retransmission safety net
    // here, so a QR that fails to decode isn't retried until later symbols happen to resolve it —
    // more per-QR redundancy directly trades for a better chance each individual read succeeds.
    ECLevel ec_level = ECLevel::Q;
    double fps = 10.0;
    // Caps how many encoded symbols BroadcastSender::send_file() generates before returning on its
    // own, as a multiple of the file's block count K (i.e. loops=5 stops after ~5*K symbols — the
    // same order-of-magnitude redundancy the old "N full passes over the file" scheme meant, just
    // expressed in the fountain code's own unit); 0 means generate indefinitely until cancelled —
    // the natural default for a continuous broadcast/kiosk-style transmission with no return
    // channel to ever signal "done, stop".
    int loops = 0;
    std::function<void(const std::string&)> log;
    std::atomic<bool>* cancel = nullptr;
};

struct BroadcastResult {
    bool success = false;
    std::string message;
};

// One-way (simplex) transfer, coexisting with the bidirectional Sender/Receiver (protocol.hpp):
// the sender repeatedly displays the file as a sequence of QR codes with no expectation of, or
// ability to receive, any reply. Reliability comes from an LT fountain code (see fountain.hpp),
// not ARQ: send_file() emits a header frame (filename/size/hash) followed by a potentially endless
// stream of encoded symbols (XOR combinations of the file's source blocks); BroadcastReceiver
// finishes the moment it has received *any sufficient number* of symbols to reconstruct every
// block — never needing any specific symbol, or the sender's current pass to finish first, unlike
// the plain repeat-the-whole-file-in-order scheme this replaced.
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
