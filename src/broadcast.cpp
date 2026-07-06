#include "qrtp/broadcast.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <vector>

#include "qrtp/fountain.hpp"
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

    // K source blocks (an LT fountain code, see fountain.hpp), the last zero-padded to
    // chunk_size — the receiver knows filesize too, so it pads/truncates identically (see
    // LtDecoder::assemble()). Reliability now comes from generating however many encoded symbols
    // it takes, not from repeating the exact same offsets in the same order every pass — a
    // receiver only ever needs *some* sufficient number of them, never specific ones.
    const size_t chunk_size = std::max<size_t>(1, cfg_.chunk_size);
    const size_t num_blocks =
        filesize == 0 ? 0 : static_cast<size_t>((filesize + chunk_size - 1) / chunk_size);
    std::vector<std::vector<uint8_t>> blocks(num_blocks);
    for (size_t i = 0; i < num_blocks; ++i) {
        const size_t start = i * chunk_size;
        const size_t len = std::min<size_t>(chunk_size, static_cast<size_t>(filesize - start));
        blocks[i].assign(chunk_size, 0);
        std::copy(content.begin() + static_cast<long>(start), content.begin() + static_cast<long>(start + len),
                   blocks[i].begin());
    }

    log("read " + std::to_string(filesize) + " bytes (" + std::to_string(num_blocks) +
        " block(s)), SHA-256 " + sha256_hex(digest).substr(0, 12) + "... — fountain-coding" +
        (cfg_.loops > 0 ? (" up to " + std::to_string(static_cast<uint64_t>(cfg_.loops) * num_blocks) + " symbol(s)")
                        : " indefinitely") +
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

    if (num_blocks == 0) {
        // Empty file: nothing to fountain-code — the header (filesize=0) is the whole transfer;
        // BroadcastReceiver completes immediately on seeing it, no DATA frame ever needed.
        if (cancelled()) return {false, "cancelled"};
        link_.send(encode_frame(header));
        return {true, "empty file, header sent"};
    }

    const uint64_t max_symbols = cfg_.loops > 0 ? static_cast<uint64_t>(cfg_.loops) * num_blocks : 0;
    uint32_t symbol_index = 0;
    while (max_symbols == 0 || symbol_index < max_symbols) {
        if (cancelled()) return {false, "cancelled"};
        // Re-sent every num_blocks symbols (about once per "old-style loop" of redundancy) so a
        // receiver that starts watching mid-broadcast still learns the filename/size/hash
        // reasonably promptly, not only if it happened to be watching from the very start.
        if (symbol_index % static_cast<uint32_t>(num_blocks) == 0) link_.send(encode_frame(header));

        Frame f;
        f.type = FrameType::DATA;
        f.session_id = session_id_;
        f.seq = symbol_index;
        f.payload = lt_encode_symbol(symbol_index, blocks);
        link_.send(encode_frame(f));

        ++symbol_index;
        if (symbol_index % static_cast<uint32_t>(num_blocks) == 0) {
            log(std::to_string(symbol_index) + " symbol(s) sent (" +
                std::to_string(symbol_index / num_blocks) + "x redundancy so far)");
        }
    }

    return {true, "broadcast finished after " + std::to_string(symbol_index) + " symbol(s)"};
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
    size_t num_blocks = 0;
    std::optional<LtDecoder> decoder;

    // DATA symbols seen before the header arrives — buffered instead of discarded (the old
    // offset-based scheme threw these away), so a receiver that starts watching mid-broadcast
    // doesn't waste everything it saw before the header happened to cycle back around: once
    // num_blocks is known, these are exactly as useful to the fountain decoder as anything else.
    std::vector<std::tuple<uint16_t, uint32_t, std::vector<uint8_t>>> pending_pre_header; // (session_id, seq, payload)

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
                num_blocks = cfg_.chunk_size > 0
                                 ? static_cast<size_t>((meta_out.filesize + cfg_.chunk_size - 1) / cfg_.chunk_size)
                                 : 0;
                log("header received: " + meta_out.filename + " (" + std::to_string(meta_out.filesize) +
                    " bytes, " + std::to_string(num_blocks) + " block(s))");

                if (meta_out.filesize == 0) return std::vector<uint8_t>{}; // empty file, nothing more needed

                decoder.emplace(num_blocks, cfg_.chunk_size);
                for (auto& [sid, seq, payload] : pending_pre_header) {
                    if (sid == session_id) decoder->add_symbol(seq, std::move(payload));
                }
                pending_pre_header.clear();
                pending_pre_header.shrink_to_fit();
            }
            // A later header (same or even a different session) is never acted on again —
            // metadata for the session already being assembled can't legitimately change mid-flight.
            continue;
        }

        if (f->type != FrameType::DATA) continue;

        if (!got_header) {
            pending_pre_header.emplace_back(f->session_id, f->seq, std::move(f->payload));
            continue;
        }
        if (f->session_id != session_id) continue;

        if (decoder->add_symbol(f->seq, std::move(f->payload))) {
            log("symbol " + std::to_string(f->seq) + " added (" + std::to_string(decoder->resolved_count()) +
                "/" + std::to_string(num_blocks) + " blocks resolved)");
        }

        if (!decoder->is_complete()) continue;

        auto full = decoder->assemble(meta_out.filesize);
        const auto digest = sha256(full.data(), full.size());
        if (digest == meta_out.sha256) {
            log("hash verified -- assembled from " + std::to_string(num_blocks) + " block(s)");
            return full;
        }
        // Unlike the old offset-based scheme (where a mismatch here was treated as "keep
        // listening, a later loop might fix it"), the decoder's state is monotonic and fully
        // determined by internally-consistent XOR algebra: every frame's CRC32 already guarantees
        // byte-perfect content in transit, so a mismatch here means the sender's own file read or
        // hash differed somehow, not something another symbol could ever resolve.
        log("hash mismatch on an apparently-complete file -- failing (this indicates a sender-side "
            "problem, not a recoverable transmission error)");
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace qrtp
