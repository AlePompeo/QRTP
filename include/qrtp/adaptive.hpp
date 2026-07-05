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
    // History: 1500 (v1-18) -> 2048, real regression, reverted -> 1024 -> 1500 (see adaptive.cpp's
    // git history / not_working.md for the 2048 postmortem — turned out to be a render-loop bug,
    // fixed separately, not chunk size itself) -> back down to 1024 by request. Independently of
    // this constant's own value, the frame payload is now COBS-encoded rather than Base64 (see
    // cobs.hpp, not_working.md 2026-07-05) — COBS's ~0.4% overhead vs. Base64's ~33% means the same
    // chunk size now needs meaningfully fewer QR-capacity bytes than it used to, so 1024 today
    // produces a noticeably smaller/less dense QR than 1024 did under the old Base64 encoding.
    // Still no real evidence covers QR versions much above ~15-20, where physical module pixels
    // shrink enough at a fixed on-screen size to plausibly hurt decode *reliability* even if raw
    // decode time doesn't scale much (the v9 finding, same mechanism as raising EC level) —
    // untested territory, not disproven safe either way.
    // Safe regardless of this value: upshift() caps chunk growth by what the *current* EC level can
    // actually encode (see max_chunk_for_ec_level in adaptive.cpp, also updated for COBS's real
    // overhead) — previously nothing stopped chunk size from growing back up while EC level was
    // still elevated from an earlier downshift, which could ask qr_encode_bytes() for a payload too
    // large for that EC level and throw, hard-failing the transfer regardless of this constant.
    size_t chunk_size_max = 1024;

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
    // over. History: fer_window=8/fer_low=0.15 -> 4/0.25 (each halving keeps the same "~1 bad in N"
    // tolerance ratio while halving how long a recovery takes to re-qualify for the next upshift).
    // 4/0.25 -> 3/0.34 continues that same pattern once more, on top of upshift() itself now also
    // growing window/chunk/fps faster per event (see adaptive.cpp) — both dials (how often it's
    // allowed to upshift, and how much each one grows) move together rather than only one of them.
    double fer_low = 0.34;
    size_t fer_window = 3;
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
