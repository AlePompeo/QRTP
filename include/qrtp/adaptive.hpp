#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace qrtp {

enum class ECLevel : uint8_t { L = 0, M = 1, Q = 2, H = 3 };
const char* ec_level_name(ECLevel level);

struct AdaptiveConfig {
    size_t window_initial = 4;
    size_t window_min = 1;
    size_t window_max = 64;

    size_t chunk_size_initial = 256; // bytes of file payload per DATA frame (QR-density proxy)
    size_t chunk_size_min = 32;
    size_t chunk_size_max = 1500;

    ECLevel ec_level_initial = ECLevel::M;

    double fps_initial = 10.0;
    double fps_min = 2.0;
    double fps_max = 30.0;

    double rto_initial_ms = 500.0;
    double rto_min_ms = 150.0;

    // Rolling frame-error-rate at/below which we upshift. Must stay realistic relative to
    // fer_window: with a window of 8, a threshold like 0.01 effectively demands a *perfect*
    // 8-in-a-row clean streak (since even a single failure gives 1/8 = 0.125 > 0.01), which on
    // any channel with double-digit-percent noise has such a low probability of ever occurring
    // that the controller pins itself at the floor and never recovers — observed directly with
    // 5% loss + 3% corruption (~15% effective per-round failure once you count both the DATA
    // frame and its ACK needing to survive), which stayed stuck for 90+ seconds transferring 20KB
    // at fer_low=0.01. 0.15 means "at most ~1 bad round out of 8" is still good enough to upshift.
    double fer_low = 0.15;
    size_t fer_window = 8; // how many recent rounds the rolling FER is computed over
};

// AIMD controller: reuses TCP congestion-control intuition, re-targeted at an optical channel.
// "Congestion" here means the camera/lighting/motion setup failing to resolve QR frames reliably,
// so the knobs tuned are window size, QR payload density (chunk size), QR error-correction level,
// and display frame rate — instead of a byte-oriented congestion window.
class AdaptiveController {
public:
    explicit AdaptiveController(AdaptiveConfig cfg);

    void record_frame_sent();
    void record_clean_round();  // a window's worth of frames acked with no retransmit needed
    void record_loss_event();   // a retransmission timeout fired
    void on_rtt_sample(std::chrono::milliseconds sample); // Jacobson/Karels SRTT/RTTVAR update

    size_t window_size() const { return window_size_; }
    size_t chunk_size() const { return chunk_size_; }
    ECLevel ec_level() const { return ec_level_; }
    double fps() const { return fps_; }
    std::chrono::milliseconds rto() const;
    double rolling_fer() const;

private:
    AdaptiveConfig cfg_;
    size_t window_size_;
    size_t chunk_size_;
    ECLevel ec_level_;
    double fps_;

    double srtt_ms_ = -1.0;
    double rttvar_ms_ = 0.0;
    double rto_ms_;

    std::deque<bool> recent_clean_;

    void push_sample(bool clean);
    void upshift();
    void downshift();
};

} // namespace qrtp
