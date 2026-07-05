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
    // Was 1500, an arbitrary round number well below what a version-40 QR can actually carry.
    // Upshift only ever reaches this ceiling at EC=L (see AdaptiveController::upshift — chunk size
    // only grows while there's still headroom before EC level has to step down), so the real limit
    // is: base64_len(chunk + kFixedOverhead=23) <= 2953 (ISO/IEC 18004 version-40/L byte capacity)
    // => chunk <= ~2191 bytes. 2048 leaves ~5% margin below that edge. Only matters once the
    // channel is already clean enough for AIMD to have climbed this high; the existing
    // severity-scaled downshift/offset-splitting logic is what protects against this being too
    // large for a given camera/lighting setup, exactly as it already does for fps_max. Raises the
    // ceiling on an already self-correcting mechanism rather than asserting a new fixed rate, but
    // — like every constant in this file — still wants real-hardware confirmation that a
    // version-40-sized frame is actually decodable at the physical display size in practice.
    size_t chunk_size_max = 2048;

    ECLevel ec_level_initial = ECLevel::M;

    // Start conservative (a webcam needs some dwell time on a QR to focus/expose/decode) and let
    // the AIMD earn its way up from here — but real hardware testing showed 60fps working
    // perfectly once the channel is good, far above the old fps_max=15 ceiling that was capping
    // large-file throughput for no real reason. fps_initial stays low/safe for cameras that can't
    // keep up; fps_max now reflects what's actually achievable, not a guess.
    double fps_initial = 3.0;
    double fps_min = 1.0;
    double fps_max = 60.0;

    double rto_initial_ms = 500.0;
    // Was 150ms, which artificially floored the retransmit timeout well above what a fast, low-
    // latency high-fps channel actually needs — at 60fps a full round trip can complete in well
    // under 150ms, so a lost frame sat waiting for a timeout far longer than necessary. Let the
    // Jacobson/Karels estimate (rto_ms_ = srtt + 4*rttvar) actually reflect a fast channel instead
    // of being clamped up.
    double rto_min_ms = 50.0;

    // Rolling frame-error-rate at/below which we upshift, and how many recent rounds it's computed
    // over. Was fer_window=8/fer_low=0.15 ("at most ~1 bad round out of 8") — correct in spirit,
    // but recovering from any single isolated hiccup then required re-earning a fresh 8-round clean
    // streak from scratch before the *next* upshift could fire, which on a large file with
    // occasional isolated blips (not sustained loss) spent a lot of time re-proving reliability
    // instead of transferring. fer_window=4/fer_low=0.25 keeps the same "~1 bad in N" tolerance
    // ratio but halves how long each recovery takes to re-qualify.
    double fer_low = 0.25;
    size_t fer_window = 4;
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
    // A retransmission timeout fired. `severity` (0..1) is the fraction of currently-inflight
    // frames that timed out this round — one straggler out of a window of several is a much
    // weaker signal than the whole window vanishing at once, and shouldn't be punished the same.
    // Defaults to 1.0 (full multiplicative decrease) for callers that can't easily compute it.
    void record_loss_event(double severity = 1.0);
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
    void downshift(double severity);
};

} // namespace qrtp
