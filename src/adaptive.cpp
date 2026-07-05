#include "qrtp/adaptive.hpp"

#include <algorithm>
#include <cmath>

#include "qrtp/framing.hpp"

namespace qrtp {
namespace {

// Version-40 (the largest QR symbol) byte-mode capacity per EC level, ISO/IEC 18004 — the hard
// ceiling a chunk size must respect once the wire-format expansion (COBS, see cobs.hpp — was
// Base64 until this was switched over, see not_working.md 2026-07-05) and the fixed frame
// header/CRC overhead are accounted for. chunk_size_max alone doesn't guarantee this: upshift()
// grows chunk size first and only improves EC level once chunk is already at *its* ceiling, but EC
// level can still be sitting at a worse setting left over from an earlier downshift while chunk
// size climbs back up — nothing previously stopped that combination from asking qr_encode_bytes()
// to encode a payload that doesn't fit any version at the current EC level, which throws (see
// qr_encoder.cpp) and would hard-fail the transfer with an unhandled exception, regardless of what
// chunk_size_max was configured to.
constexpr size_t kVersion40Capacity[4] = {2953, 2331, 1663, 1273}; // indexed by ECLevel: L, M, Q, H

size_t max_chunk_for_ec_level(ECLevel level) {
    const size_t capacity = kVersion40Capacity[static_cast<uint8_t>(level)];
    if (capacity < 2) return 0;
    // COBS's worst case is 1 overhead byte per <=254 input bytes plus 1 initial code byte, i.e.
    // cobs_len(n) <= n + n/254 + 1 — so the largest encodable n satisfies
    // n*(255/254) <= capacity - 1, n <= (capacity-1)*254/255. A conservative (rounds down) bound,
    // not an exact inverse, which is fine for a safety ceiling: it only needs to never
    // under-estimate the true cost, not hit it exactly.
    const size_t max_frame_bytes = (capacity - 1) * 254 / 255;
    return max_frame_bytes > kFixedOverhead ? max_frame_bytes - kFixedOverhead : 0;
}

} // namespace

const char* ec_level_name(ECLevel level) {
    switch (level) {
        case ECLevel::L: return "L";
        case ECLevel::M: return "M";
        case ECLevel::Q: return "Q";
        case ECLevel::H: return "H";
    }
    return "?";
}

AdaptiveController::AdaptiveController(AdaptiveConfig cfg)
    : cfg_(cfg),
      window_size_(cfg.window_initial),
      chunk_size_(cfg.chunk_size_initial),
      ec_level_(cfg.ec_level_initial),
      fps_(cfg.fps_initial),
      rto_ms_(cfg.rto_initial_ms) {}

void AdaptiveController::record_frame_sent() {
    // Reserved for future throughput accounting; no state change needed for AIMD today.
}

void AdaptiveController::push_sample(bool clean) {
    recent_clean_.push_back(clean);
    if (recent_clean_.size() > cfg_.fer_window) recent_clean_.pop_front();
}

double AdaptiveController::rolling_fer() const {
    if (recent_clean_.empty()) return 0.0;
    const auto clean_count = std::count(recent_clean_.begin(), recent_clean_.end(), true);
    return 1.0 - (static_cast<double>(clean_count) / static_cast<double>(recent_clean_.size()));
}

void AdaptiveController::record_clean_round() {
    push_sample(true);
    if (recent_clean_.size() >= cfg_.fer_window && rolling_fer() <= cfg_.fer_low) {
        upshift();
    }
}

void AdaptiveController::record_loss_event(double severity) {
    severity = std::clamp(severity, 0.0, 1.0);
    push_sample(false);
    // Full multiplicative decrease (halve) at severity=1.0 (the whole window timed out), tapering
    // to a light trim for an isolated straggler — one late frame out of a window of several is a
    // weak signal and shouldn't crash the window the same as a total wipeout.
    const double shrink_factor = 1.0 - 0.5 * severity;
    window_size_ = std::max(cfg_.window_min,
                             static_cast<size_t>(std::llround(window_size_ * shrink_factor)));
    downshift(severity);
}

void AdaptiveController::upshift() {
    // Proportional (~25% + 1), matching chunk_size's/fps's own growth shape below instead of a
    // flat +1 — window_max=64 from window_initial=4 took up to 60 individual upshift events to
    // reach at a flat rate, meaningfully lagging chunk/fps's already-multiplicative climb for no
    // real reason.
    if (window_size_ < cfg_.window_max) {
        window_size_ = std::min(cfg_.window_max, window_size_ + window_size_ / 4 + 1);
    }

    // Never grow past what the *current* EC level can actually encode, even if chunk_size_max
    // allows more — see max_chunk_for_ec_level's comment for why this matters. This is the
    // effective ceiling chunk size grows toward; only once chunk size has reached it (whichever of
    // chunk_size_max or the EC-level limit is smaller) does EC level get to improve, unlocking a
    // higher ceiling for the next upshift.
    const size_t chunk_ceiling = std::min(cfg_.chunk_size_max, max_chunk_for_ec_level(ec_level_));
    if (chunk_size_ < chunk_ceiling) {
        // ~20% + 1 per event, up from ~10% + 1 — faster climb once a clean streak re-qualifies.
        chunk_size_ = std::min(chunk_ceiling, chunk_size_ + chunk_size_ / 5 + 1);
    } else if (ec_level_ != ECLevel::L) {
        ec_level_ = static_cast<ECLevel>(static_cast<uint8_t>(ec_level_) - 1);
    }

    // Multiplicative, matching chunk_size's growth and symmetric with downshift()'s fps_*0.8 —
    // was a flat +1.0, which took ~57 individual upshift events to climb from a fps_initial of 3
    // to the old fps_max of 15 (now 60), each gated behind re-qualifying with a clean streak. At
    // a fixed +1 that's an eternity for a channel that's actually reliable; growing by a fraction
    // of the current value ramps up fast early (when it matters most) while still tapering off
    // smoothly near the ceiling. *1.4 (was *1.25): faster climb per event, same shape.
    if (fps_ < cfg_.fps_max) fps_ = std::min(cfg_.fps_max, fps_ * 1.4 + 0.5);
}

void AdaptiveController::downshift(double severity) {
    severity = std::clamp(severity, 0.0, 1.0);

    if (chunk_size_ > cfg_.chunk_size_min) {
        // Full severity shrinks by ~1/3 (the old flat behavior); a lone straggler barely nudges
        // it instead of demanding a full renegotiation of chunk size.
        const auto shrink = static_cast<size_t>((chunk_size_ / 3 + 1) * severity);
        chunk_size_ = std::max(cfg_.chunk_size_min, chunk_size_ - shrink);
    } else if (severity >= 0.5 && ec_level_ != ECLevel::H) {
        // Raising EC level for a fixed chunk size shrinks the QR's module pixels (v9 finding), so
        // only reach for it when the loss looks systemic, not from one isolated timeout.
        ec_level_ = static_cast<ECLevel>(static_cast<uint8_t>(ec_level_) + 1);
    }

    if (fps_ > cfg_.fps_min) {
        // Full severity gives the old fps_*0.8; scale the pullback down for partial severity.
        fps_ = std::max(cfg_.fps_min, fps_ * (1.0 - 0.2 * severity));
    }
}

void AdaptiveController::on_rtt_sample(std::chrono::milliseconds sample) {
    const double s = static_cast<double>(sample.count());
    if (srtt_ms_ < 0) {
        srtt_ms_ = s;
        rttvar_ms_ = s / 2.0;
    } else {
        rttvar_ms_ = 0.75 * rttvar_ms_ + 0.25 * std::fabs(srtt_ms_ - s);
        srtt_ms_ = 0.875 * srtt_ms_ + 0.125 * s;
    }
    rto_ms_ = std::max(cfg_.rto_min_ms, srtt_ms_ + 4.0 * rttvar_ms_);
}

std::chrono::milliseconds AdaptiveController::rto() const {
    return std::chrono::milliseconds(static_cast<int64_t>(rto_ms_));
}

} // namespace qrtp
