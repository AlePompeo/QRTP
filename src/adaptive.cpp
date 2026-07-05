#include "qrtp/adaptive.hpp"

#include <algorithm>
#include <cmath>

namespace qrtp {

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

void AdaptiveController::record_loss_event() {
    push_sample(false);
    window_size_ = std::max(cfg_.window_min, window_size_ / 2);
    downshift();
}

void AdaptiveController::upshift() {
    if (window_size_ < cfg_.window_max) ++window_size_;

    if (chunk_size_ < cfg_.chunk_size_max) {
        chunk_size_ = std::min(cfg_.chunk_size_max, chunk_size_ + chunk_size_ / 10 + 1);
    } else if (ec_level_ != ECLevel::L) {
        ec_level_ = static_cast<ECLevel>(static_cast<uint8_t>(ec_level_) - 1);
    }

    // Multiplicative, matching chunk_size's growth and symmetric with downshift()'s fps_*0.8 —
    // was a flat +1.0, which took ~57 individual upshift events to climb from a fps_initial of 3
    // to the old fps_max of 15 (now 60), each gated behind re-qualifying with a clean streak. At
    // a fixed +1 that's an eternity for a channel that's actually reliable; growing by a fraction
    // of the current value ramps up fast early (when it matters most) while still tapering off
    // smoothly near the ceiling.
    if (fps_ < cfg_.fps_max) fps_ = std::min(cfg_.fps_max, fps_ * 1.25 + 0.5);
}

void AdaptiveController::downshift() {
    if (chunk_size_ > cfg_.chunk_size_min) {
        chunk_size_ = std::max(cfg_.chunk_size_min, chunk_size_ - chunk_size_ / 3 - 1);
    } else if (ec_level_ != ECLevel::H) {
        ec_level_ = static_cast<ECLevel>(static_cast<uint8_t>(ec_level_) + 1);
    }

    if (fps_ > cfg_.fps_min) fps_ = std::max(cfg_.fps_min, fps_ * 0.8);
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
