#include "qrtp/protocol.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <random>

#include "qrtp/sha256.hpp"

namespace qrtp {
namespace {

using Clock = std::chrono::steady_clock;

void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}

void put_u64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 7; i >= 0; --i) b.push_back(uint8_t(v >> (i * 8)));
}

uint32_t get_u32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

std::vector<uint8_t> pack_syn(const std::string& filename, uint64_t filesize,
                               const std::array<uint8_t, 32>& sha) {
    std::vector<uint8_t> out;
    const uint8_t name_len = static_cast<uint8_t>(std::min<size_t>(filename.size(), 255));
    out.push_back(name_len);
    out.insert(out.end(), filename.begin(), filename.begin() + name_len);
    put_u64(out, filesize);
    out.insert(out.end(), sha.begin(), sha.end());
    return out;
}

bool parse_syn(const std::vector<uint8_t>& payload, TransferMetadata& meta) {
    if (payload.empty()) return false;
    const uint8_t name_len = payload[0];
    const size_t needed = size_t(1) + name_len + 8 + 32;
    if (payload.size() < needed) return false;
    meta.filename.assign(payload.begin() + 1, payload.begin() + 1 + name_len);
    meta.filesize = get_u64(&payload[1 + name_len]);
    std::copy(payload.begin() + 1 + name_len + 8, payload.begin() + 1 + name_len + 8 + 32, meta.sha256.begin());
    return true;
}

// SACK payload: repeated (offset:u32, length:u32) entries for chunks buffered strictly ahead of
// the cumulative point (which travels separately, in the frame's ack_window field) — mirrors
// TCP's SACK option (RFC 2018): a cumulative ACK plus a list of additionally-received ranges.
std::vector<uint8_t> pack_sack_ranges(const std::vector<std::pair<uint64_t, uint32_t>>& ranges) {
    std::vector<uint8_t> out;
    constexpr size_t kMaxEntries = 32; // keeps the SACK payload well within one QR frame
    const size_t n = std::min(ranges.size(), kMaxEntries);
    for (size_t i = 0; i < n; ++i) {
        put_u32(out, static_cast<uint32_t>(ranges[i].first));
        put_u32(out, ranges[i].second);
    }
    return out;
}

std::vector<std::pair<uint32_t, uint32_t>> parse_sack_ranges(const std::vector<uint8_t>& payload) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    const size_t n = payload.size() / 8;
    for (size_t i = 0; i < n; ++i) {
        out.emplace_back(get_u32(&payload[i * 8]), get_u32(&payload[i * 8 + 4]));
    }
    return out;
}

uint16_t random_session_id() {
    static std::random_device rd;
    static std::mt19937 rng(rd());
    static std::uniform_int_distribution<int> dist(1, 0xFFFF);
    return static_cast<uint16_t>(dist(rng));
}

} // namespace

// ============================================================================================
// Sender
// ============================================================================================

Sender::Sender(ILink& link, ProtocolConfig cfg)
    : link_(link), cfg_(std::move(cfg)), adaptive_(cfg_.adaptive), session_id_(random_session_id()) {}

void Sender::log(const std::string& msg) const {
    if (cfg_.log) cfg_.log("[sender] " + msg);
}

bool Sender::cancelled() const { return cfg_.cancel && cfg_.cancel->load(); }

bool Sender::wait_for(FrameType type, uint16_t session_id, Frame& out, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    std::vector<uint8_t> raw;
    // Poll in small slices (rather than one blocking try_receive for the whole timeout) so a
    // cancellation request is noticed quickly even mid-wait, not just between attempts.
    constexpr auto kPollSlice = std::chrono::milliseconds(150);
    while (Clock::now() < deadline) {
        if (cancelled()) return false;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) break;
        if (remaining > kPollSlice) remaining = kPollSlice;
        if (link_.try_receive(raw, remaining)) {
            auto f = decode_frame(raw);
            if (f && f->session_id == session_id && f->type == type) {
                out = *f;
                return true;
            }
            // A stray/late frame from a different phase — keep waiting within the same deadline.
        }
    }
    return false;
}

TransferResult Sender::send_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {false, "cannot open input file: " + path.string()};
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const uint64_t filesize = content.size();
    const auto digest = sha256(content.data(), content.size());

    log("read " + std::to_string(filesize) + " bytes, SHA-256 " + sha256_hex(digest).substr(0, 12) + "...");

    // --- Handshake (3-way, like TCP) ---
    link_.set_fps(adaptive_.fps());
    link_.set_ec_level(adaptive_.ec_level());
    Frame syn;
    syn.type = FrameType::SYN;
    syn.session_id = session_id_;
    syn.ack_window = static_cast<uint32_t>(adaptive_.window_size());
    syn.payload = pack_syn(path.filename().string(), filesize, digest);

    Frame syn_ack;
    bool got_syn_ack = false;
    for (int attempt = 0; attempt < cfg_.max_handshake_retries && !got_syn_ack; ++attempt) {
        if (cancelled()) return {false, "cancelled"};
        link_.send(encode_frame(syn));
        log("SYN sent (attempt " + std::to_string(attempt + 1) + ")");
        got_syn_ack = wait_for(FrameType::SYN_ACK, session_id_, syn_ack, cfg_.handshake_timeout);
    }
    if (!got_syn_ack) return {false, cancelled() ? "cancelled" : "handshake failed: no SYN_ACK received"};
    log("SYN_ACK received, receiver window hint=" + std::to_string(syn_ack.ack_window));

    Frame ack;
    ack.type = FrameType::ACK;
    ack.session_id = session_id_;
    ack.ack_window = std::min<uint32_t>(syn_ack.ack_window, static_cast<uint32_t>(adaptive_.window_size()));
    link_.send(encode_frame(ack));
    log("ACK sent, connection established");

    // --- Data phase: selective-repeat sliding window, keyed by byte offset ---
    struct InFlight {
        std::vector<uint8_t> bytes;
        Clock::time_point sent_at;
        int retries = 0;
    };
    std::map<uint64_t, InFlight> inflight;
    uint64_t next_offset = 0;
    uint64_t cumulative_acked = 0;

    while (cumulative_acked < filesize) {
        if (cancelled()) return {false, "cancelled"};
        while (inflight.size() < adaptive_.window_size() && next_offset < filesize) {
            const size_t len = std::min<uint64_t>(adaptive_.chunk_size(), filesize - next_offset);
            Frame f;
            f.type = FrameType::DATA;
            f.session_id = session_id_;
            f.seq = static_cast<uint32_t>(next_offset);
            f.flags_byte = (next_offset + len >= filesize) ? flags::LAST_FRAME : uint8_t(0);
            f.payload.assign(content.begin() + static_cast<long>(next_offset),
                              content.begin() + static_cast<long>(next_offset + len));
            link_.set_fps(adaptive_.fps());
            link_.set_ec_level(adaptive_.ec_level());
            link_.send(encode_frame(f));
            adaptive_.record_frame_sent();
            inflight[next_offset] = InFlight{f.payload, Clock::now(), 0};
            next_offset += len;
        }

        std::vector<uint8_t> raw;
        if (link_.try_receive(raw, cfg_.data_poll_timeout)) {
            auto f = decode_frame(raw);
            if (!f || f->session_id != session_id_ || f->type != FrameType::SACK) continue;

            const uint64_t new_cumulative = f->ack_window;
            bool any_was_retried = false;

            for (auto it = inflight.begin(); it != inflight.end();) {
                if (it->first < new_cumulative) {
                    adaptive_.on_rtt_sample(
                        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - it->second.sent_at));
                    if (it->second.retries > 0) any_was_retried = true;
                    it = inflight.erase(it);
                } else {
                    ++it;
                }
            }

            const bool advanced = new_cumulative > cumulative_acked;
            cumulative_acked = std::max(cumulative_acked, new_cumulative);

            for (const auto& range : parse_sack_ranges(f->payload)) {
                auto it = inflight.find(range.first);
                if (it != inflight.end()) {
                    adaptive_.on_rtt_sample(
                        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - it->second.sent_at));
                    if (it->second.retries > 0) any_was_retried = true;
                    inflight.erase(it);
                }
            }

            if (advanced && !any_was_retried) adaptive_.record_clean_round();
        } else {
            std::vector<uint64_t> timed_out;
            for (auto& [off, info] : inflight) {
                if (Clock::now() - info.sent_at > adaptive_.rto()) timed_out.push_back(off);
            }

            if (!timed_out.empty()) {
                link_.set_fps(adaptive_.fps());
                link_.set_ec_level(adaptive_.ec_level());

                // Fraction of the in-flight window that actually timed out this round — captured
                // before the loop below splits/erases entries, so it reflects the real ratio.
                const double severity =
                    static_cast<double>(timed_out.size()) / static_cast<double>(inflight.size());

                for (uint64_t off : timed_out) {
                    auto it = inflight.find(off);
                    if (it == inflight.end()) continue; // already resolved by a SACK this round

                    const size_t current_chunk = adaptive_.chunk_size();
                    if (it->second.bytes.size() > current_chunk) {
                        // Resending this offset's original (larger) payload unchanged while only
                        // raising the EC level is actually counterproductive: more redundancy for
                        // the same amount of data needs a *bigger* QR, which — at a fixed on-screen
                        // size — means smaller, harder-to-resolve modules, not easier. Split it into
                        // pieces matching the current (smaller) chunk size instead, each getting its
                        // own fresh retry budget.
                        const std::vector<uint8_t> original_bytes = it->second.bytes;
                        const uint64_t original_off = off;
                        inflight.erase(it);
                        log("splitting stuck offset " + std::to_string(original_off) + " (" +
                            std::to_string(original_bytes.size()) + " bytes) into " +
                            std::to_string(current_chunk) + "-byte pieces");
                        for (size_t i = 0; i < original_bytes.size(); i += current_chunk) {
                            const size_t len = std::min(current_chunk, original_bytes.size() - i);
                            const uint64_t sub_off = original_off + i;
                            std::vector<uint8_t> sub_bytes(original_bytes.begin() + static_cast<long>(i),
                                                             original_bytes.begin() + static_cast<long>(i + len));
                            Frame rf;
                            rf.type = FrameType::DATA;
                            rf.session_id = session_id_;
                            rf.seq = static_cast<uint32_t>(sub_off);
                            rf.flags_byte = flags::RETRANSMIT;
                            if (sub_off + len >= filesize) rf.flags_byte |= flags::LAST_FRAME;
                            rf.payload = sub_bytes;
                            link_.send(encode_frame(rf));
                            inflight[sub_off] = InFlight{sub_bytes, Clock::now(), 0};
                        }
                    } else {
                        if (++it->second.retries > cfg_.max_data_retries) {
                            return {false, "max retransmit attempts exceeded at offset " + std::to_string(off)};
                        }
                        Frame rf;
                        rf.type = FrameType::DATA;
                        rf.session_id = session_id_;
                        rf.seq = static_cast<uint32_t>(off);
                        rf.flags_byte = flags::RETRANSMIT;
                        if (off + it->second.bytes.size() >= filesize) rf.flags_byte |= flags::LAST_FRAME;
                        rf.payload = it->second.bytes;
                        link_.send(encode_frame(rf));
                        it->second.sent_at = Clock::now();
                    }
                }

                adaptive_.record_loss_event(severity);
                log("retransmit (severity=" + std::to_string(severity) + ") — window=" + std::to_string(adaptive_.window_size()) +
                    " chunk=" + std::to_string(adaptive_.chunk_size()) +
                    " ec=" + ec_level_name(adaptive_.ec_level()) +
                    " fps=" + std::to_string(adaptive_.fps()));
            }
        }
    }

    log("all " + std::to_string(filesize) + " bytes acked, tearing down");

    // --- Teardown (like TCP's FIN/FIN-ACK) ---
    Frame fin;
    fin.type = FrameType::FIN;
    fin.session_id = session_id_;
    fin.seq = static_cast<uint32_t>(filesize);

    Frame fin_ack;
    bool got_fin_ack = false;
    for (int attempt = 0; attempt < cfg_.max_handshake_retries && !got_fin_ack; ++attempt) {
        if (cancelled()) break; // all bytes are already acked at this point; just stop cleanly
        link_.send(encode_frame(fin));
        got_fin_ack = wait_for(FrameType::FIN_ACK, session_id_, fin_ack, cfg_.handshake_timeout);
    }
    if (!got_fin_ack) return {false, "teardown failed: no FIN_ACK received"};

    const bool receiver_ok = (fin_ack.flags_byte & flags::LAST_FRAME) != 0;
    return {receiver_ok,
            receiver_ok ? "transfer complete, hash verified by receiver" : "receiver reported a hash mismatch"};
}

// ============================================================================================
// Receiver
// ============================================================================================

Receiver::Receiver(ILink& link, ProtocolConfig cfg)
    : link_(link), cfg_(std::move(cfg)), adaptive_(cfg_.adaptive) {}

void Receiver::log(const std::string& msg) const {
    if (cfg_.log) cfg_.log("[receiver] " + msg);
}

bool Receiver::cancelled() const { return cfg_.cancel && cfg_.cancel->load(); }

std::optional<std::vector<uint8_t>> Receiver::receive_file(TransferMetadata& meta_out,
                                                             std::chrono::milliseconds overall_timeout) {
    const auto deadline = Clock::now() + overall_timeout;
    uint16_t session_id = 0;
    bool got_syn = false;
    std::vector<uint8_t> raw;

    // --- Handshake ---
    while (Clock::now() < deadline && !got_syn) {
        if (cancelled()) return std::nullopt;
        if (link_.try_receive(raw, std::chrono::milliseconds(200))) {
            auto f = decode_frame(raw);
            if (f && f->type == FrameType::SYN && parse_syn(f->payload, meta_out)) {
                session_id = f->session_id;
                got_syn = true;
            }
        }
    }
    if (!got_syn) return std::nullopt;
    log("SYN received: " + meta_out.filename + " (" + std::to_string(meta_out.filesize) + " bytes)");

    link_.set_fps(adaptive_.fps());
    link_.set_ec_level(adaptive_.ec_level());
    Frame syn_ack;
    syn_ack.type = FrameType::SYN_ACK;
    syn_ack.session_id = session_id;
    syn_ack.ack_window = static_cast<uint32_t>(cfg_.adaptive.window_initial);
    link_.send(encode_frame(syn_ack));

    // --- Data phase (an explicit ACK or simply the first DATA frame both confirm the handshake) ---
    std::map<uint64_t, std::vector<uint8_t>> chunks;
    uint64_t cumulative = 0;
    uint32_t frames_since_sack = 0;
    bool any_retransmit_since_sack = false;
    bool fin_seen = false;

    auto advance_cumulative = [&] {
        while (true) {
            auto it = chunks.find(cumulative);
            if (it == chunks.end()) break;
            cumulative += it->second.size();
        }
    };

    auto send_sack = [&] {
        // Always push the current adaptive fps/EC level before displaying — this is the reverse
        // channel's only throttle, so every send here (including the FIN-nudge/final-ack paths
        // below, not just the main per-DATA-frame case) should reflect it.
        link_.set_fps(adaptive_.fps());
        link_.set_ec_level(adaptive_.ec_level());
        std::vector<std::pair<uint64_t, uint32_t>> ranges;
        for (const auto& [off, bytes] : chunks) {
            if (off > cumulative) ranges.emplace_back(off, static_cast<uint32_t>(bytes.size()));
        }
        Frame f;
        f.type = FrameType::SACK;
        f.session_id = session_id;
        f.ack_window = static_cast<uint32_t>(cumulative);
        f.payload = pack_sack_ranges(ranges);
        link_.send(encode_frame(f));
    };

    while (cumulative < meta_out.filesize || !fin_seen) {
        if (cancelled()) return std::nullopt;
        if (link_.try_receive(raw, cfg_.data_poll_timeout)) {
            auto f = decode_frame(raw);
            if (!f) {
                log("decode_frame rejected " + std::to_string(raw.size()) +
                    " raw bytes (size/CRC mismatch — camera read something, but it wasn't a valid frame)");
                continue;
            }
            if (f->session_id != session_id) continue;

            if (f->type == FrameType::SYN) {
                link_.send(encode_frame(syn_ack)); // sender never saw our SYN_ACK
                continue;
            }
            if (f->type == FrameType::DATA) {
                log("DATA received: offset=" + std::to_string(f->seq) + " len=" + std::to_string(f->payload.size()) +
                    " cumulative_before=" + std::to_string(cumulative));
                if (static_cast<uint64_t>(f->seq) >= cumulative) {
                    // Re-chunking (protocol.cpp: Sender splits a stuck offset into smaller pieces)
                    // only ever shrinks a chunk, never grows one back — so if we already have
                    // something stored at this exact offset and the new arrival is *larger*, it can
                    // only be a stale straggler of a since-superseded, now-obsolete larger frame
                    // (e.g. a camera frame buffered from just before the sender re-chunked this
                    // offset). Accepting it would silently corrupt an already-correct, more
                    // fine-grained reassembly with no CRC to catch it — the stale frame is itself
                    // perfectly valid, just outdated. Smaller-or-equal always wins.
                    auto existing = chunks.find(f->seq);
                    if (existing == chunks.end() || f->payload.size() <= existing->second.size()) {
                        chunks[f->seq] = f->payload;
                    } else {
                        log("ignoring stale larger frame at offset " + std::to_string(f->seq) + " (" +
                            std::to_string(f->payload.size()) + " bytes, already have " +
                            std::to_string(existing->second.size()) + ")");
                    }
                    advance_cumulative();
                }
                // else: offset already confirmed past cumulative -- a stale duplicate; ignore its
                // payload entirely (never let already-locked-in reassembly data change), but still
                // fall through below so the sender gets an ack either way.
                ++frames_since_sack;
                // A RETRANSMIT flag means the sender's window is starved for feedback (this is
                // most visible once the window has shrunk to 1: with only one frame ever in
                // flight, sack_every_n_frames' batching would otherwise never be reached by fresh
                // arrivals alone) — answer immediately instead of waiting for the batching count.
                // It's also the only loss signal this side has for its own reverse channel: a
                // retransmit means a previous SACK likely didn't get through in time.
                const bool starved_for_ack = (f->flags_byte & flags::RETRANSMIT) != 0;
                if (starved_for_ack) any_retransmit_since_sack = true;
                if (starved_for_ack || frames_since_sack >= cfg_.sack_every_n_frames ||
                    (f->flags_byte & flags::LAST_FRAME) || cumulative >= meta_out.filesize) {
                    if (any_retransmit_since_sack) adaptive_.record_loss_event();
                    else adaptive_.record_clean_round();
                    send_sack();
                    frames_since_sack = 0;
                    any_retransmit_since_sack = false;
                }
                continue;
            }
            if (f->type == FrameType::FIN) {
                if (cumulative < meta_out.filesize) {
                    send_sack(); // sender thinks it's done but we still have gaps — nudge it
                } else {
                    fin_seen = true;
                }
                continue;
            }
        } else if (cumulative >= meta_out.filesize && !fin_seen) {
            send_sack(); // make sure the sender has seen our final cumulative ACK
        }
    }

    // --- Verify + teardown ---
    std::vector<uint8_t> full;
    full.reserve(meta_out.filesize);
    for (const auto& [off, bytes] : chunks) {
        (void)off;
        full.insert(full.end(), bytes.begin(), bytes.end());
        if (full.size() >= meta_out.filesize) break;
    }
    full.resize(meta_out.filesize);

    const auto digest = sha256(full.data(), full.size());
    const bool ok = digest == meta_out.sha256;
    log(ok ? "hash verified" : "HASH MISMATCH");

    Frame fin_ack;
    fin_ack.type = FrameType::FIN_ACK;
    fin_ack.session_id = session_id;
    fin_ack.flags_byte = ok ? flags::LAST_FRAME : uint8_t(0);
    for (int i = 0; i < 3; ++i) link_.send(encode_frame(fin_ack)); // guard against a lost FIN_ACK

    if (!ok) return std::nullopt;
    return full;
}

} // namespace qrtp
