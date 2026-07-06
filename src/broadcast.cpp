#include "qrtp/broadcast.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>

#include "qrtp/sha256.hpp"

namespace qrtp {
namespace {

using Clock = std::chrono::steady_clock;

uint16_t random_session_id() {
    static std::random_device rd;
    static std::mt19937 rng(rd());
    static std::uniform_int_distribution<int> dist(1, 0xFFFF);
    return static_cast<uint16_t>(dist(rng));
}

} // namespace

// ============================================================================================
// BroadcastSender
// ============================================================================================

BroadcastSender::BroadcastSender(ILink& link, BroadcastConfig cfg)
    : link_(link), cfg_(std::move(cfg)), session_id_(random_session_id()) {}

void BroadcastSender::log(const std::string& msg) const {
    if (cfg_.log) cfg_.log("[broadcast-sender] " + msg);
}

bool BroadcastSender::cancelled() const { return cfg_.cancel && cfg_.cancel->load(); }

BroadcastResult BroadcastSender::send_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {false, "cannot open input file: " + path.string()};
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const uint64_t filesize = content.size();
    const auto digest = sha256(content.data(), content.size());

    log("read " + std::to_string(filesize) + " bytes, SHA-256 " + sha256_hex(digest).substr(0, 12) +
        "... — broadcasting" + (cfg_.loops > 0 ? (" " + std::to_string(cfg_.loops) + " loop(s)") : " indefinitely") +
        " at fixed fps=" + std::to_string(cfg_.fps) + " chunk=" + std::to_string(cfg_.chunk_size) +
        " ec=" + ec_level_name(cfg_.ec_level));

    // Fixed for the whole broadcast — there is no feedback channel to adapt these from, unlike the
    // bidirectional Sender's AdaptiveController.
    link_.set_fps(cfg_.fps);
    link_.set_ec_level(cfg_.ec_level);

    Frame header;
    header.type = FrameType::SYN;
    header.session_id = session_id_;
    header.payload = pack_file_metadata(path.filename().string(), filesize, digest);

    int loop = 0;
    while (cfg_.loops <= 0 || loop < cfg_.loops) {
        if (cancelled()) return {false, "cancelled"};
        // Sent once per loop (not just once at the start) so a receiver that starts watching
        // mid-broadcast can still learn the filename/size/hash on the next pass, instead of only
        // ever being able to join a session that happened to start before it did.
        link_.send(encode_frame(header));

        for (uint64_t offset = 0; offset < filesize; offset += cfg_.chunk_size) {
            if (cancelled()) return {false, "cancelled"};
            const size_t len = std::min<uint64_t>(cfg_.chunk_size, filesize - offset);
            Frame f;
            f.type = FrameType::DATA;
            f.session_id = session_id_;
            f.seq = static_cast<uint32_t>(offset);
            f.flags_byte = (offset + len >= filesize) ? flags::LAST_FRAME : uint8_t(0);
            f.payload.assign(content.begin() + static_cast<long>(offset),
                              content.begin() + static_cast<long>(offset + len));
            link_.send(encode_frame(f));
        }

        ++loop;
        log("loop " + std::to_string(loop) + (cfg_.loops > 0 ? "/" + std::to_string(cfg_.loops) : "") +
            " complete");
    }

    return {true, "broadcast finished after " + std::to_string(loop) + " loop(s)"};
}

// ============================================================================================
// BroadcastReceiver
// ============================================================================================

BroadcastReceiver::BroadcastReceiver(ILink& link, BroadcastConfig cfg) : link_(link), cfg_(std::move(cfg)) {}

void BroadcastReceiver::log(const std::string& msg) const {
    if (cfg_.log) cfg_.log("[broadcast-receiver] " + msg);
}

bool BroadcastReceiver::cancelled() const { return cfg_.cancel && cfg_.cancel->load(); }

std::optional<std::vector<uint8_t>> BroadcastReceiver::receive_file(TransferMetadata& meta_out,
                                                                      std::chrono::milliseconds overall_timeout) {
    using Clock2 = std::chrono::steady_clock;
    const auto deadline = Clock2::now() + overall_timeout;
    bool got_header = false;
    uint16_t session_id = 0;
    size_t expected_chunk_count = 0; // computed once the header arrives (ceil(filesize/chunk_size))

    // No re-chunking/splitting concept exists in this mode (no ARQ ever runs), so every sighting
    // of a given offset across however many loops the sender makes is always byte-identical — a
    // plain overwrite is safe, no need for the bidirectional Receiver's "smaller-or-equal-size
    // wins" staleness handling.
    std::map<uint64_t, std::vector<uint8_t>> chunks;
    std::set<uint64_t> received_offsets;
    uint64_t received_bytes = 0;

    std::vector<uint8_t> raw;
    while (Clock2::now() < deadline) {
        if (cancelled()) return std::nullopt;
        if (!link_.try_receive(raw, std::chrono::milliseconds(200))) continue;
        auto f = decode_frame(raw);
        if (!f) continue;

        if (f->type == FrameType::SYN) {
            TransferMetadata candidate;
            if (!parse_file_metadata(f->payload, candidate)) continue;
            if (!got_header) {
                meta_out = candidate;
                session_id = f->session_id;
                got_header = true;
                expected_chunk_count = cfg_.chunk_size > 0
                                           ? static_cast<size_t>((meta_out.filesize + cfg_.chunk_size - 1) /
                                                                  cfg_.chunk_size)
                                           : 0;
                log("header received: " + meta_out.filename + " (" + std::to_string(meta_out.filesize) +
                    " bytes, " + std::to_string(expected_chunk_count) + " expected chunk(s))");
            }
            // A later loop's header (same or even a different session) is never acted on again —
            // metadata for the session already being assembled can't legitimately change mid-flight.
            continue;
        }

        if (!got_header || f->session_id != session_id || f->type != FrameType::DATA) continue;

        if (received_offsets.insert(f->seq).second) {
            // First sighting of this exact offset — only the first counts toward received_bytes;
            // later loops just repeat the same bytes at the same key.
            chunks[f->seq] = f->payload;
            received_bytes += f->payload.size();
            log("DATA received: offset=" + std::to_string(f->seq) + " len=" + std::to_string(f->payload.size()) +
                " (" + std::to_string(received_offsets.size()) + "/" + std::to_string(expected_chunk_count) +
                " chunks)");
        }

        const bool complete = meta_out.filesize == 0 ||
                               (received_offsets.size() >= expected_chunk_count && received_bytes >= meta_out.filesize);
        if (!complete) continue;

        std::vector<uint8_t> full;
        full.reserve(meta_out.filesize);
        for (const auto& [off, bytes] : chunks) full.insert(full.end(), bytes.begin(), bytes.end());
        full.resize(meta_out.filesize);

        const auto digest = sha256(full.data(), full.size());
        if (digest == meta_out.sha256) {
            log("hash verified -- assembled from " + std::to_string(received_offsets.size()) + " chunk(s)");
            return full;
        }
        // Every frame's CRC32 already guarantees byte-perfect content, and fixed, non-overlapping
        // offsets can't collide — a mismatch here shouldn't happen, but keep listening rather than
        // fail outright if it somehow does: a later loop might still resolve whatever's wrong.
        log("unexpected hash mismatch with an apparently-complete file -- continuing to listen");
    }
    return std::nullopt;
}

} // namespace qrtp
