#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

#include "qrtp/adaptive.hpp"

namespace qrtp {

// The physical/optical layer's contract: send a framed byte string outward, or try to receive one
// within a timeout. Two implementations exist: LoopbackLink (below, hardware-free) and
// OpticalLink (optical_link.hpp, real screen+camera, requires OpenCV).
class ILink {
public:
    virtual ~ILink() = default;
    virtual void send(const std::vector<uint8_t>& frame_bytes) = 0;
    virtual bool try_receive(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) = 0;

    // Lets Sender push its AdaptiveController's current display parameters down to whatever the
    // concrete link actually is. Default no-ops: LoopbackLink has no physical display, so there's
    // nothing for it to do with these — only OpticalLink overrides them. Without this, the AIMD's
    // computed ec_level()/fps() were purely cosmetic log values that never touched the actual
    // rendered QR, which is exactly why a stuck offset never got any more robust across retries no
    // matter how far the controller thought it had backed off.
    virtual void set_fps(double) {}
    virtual void set_ec_level(ECLevel) {}
};

struct LoopbackConfig {
    double loss_probability = 0.0;        // probability a sent frame never arrives
    double corruption_probability = 0.0;  // probability a sent frame arrives with a flipped bit
    std::chrono::milliseconds latency{5}; // simulated one-way transit delay
    unsigned seed = 1;
};

// An in-process, hardware-free stand-in for two optical links facing each other. Frame bytes are
// passed directly between two paired LoopbackLink instances (one per direction), with optional
// synthetic loss/bit-corruption — enough to exercise the sliding window, ARQ, and adaptive
// controller without a monitor or camera. It intentionally does NOT round-trip frames through real
// QR rendering/decoding; see qr_encoder.hpp for that (exercised separately by `qrtp render-qr`).
class LoopbackLink : public ILink {
public:
    static std::pair<std::shared_ptr<LoopbackLink>, std::shared_ptr<LoopbackLink>> make_pair(
        const LoopbackConfig& cfg);

    void send(const std::vector<uint8_t>& frame_bytes) override;
    bool try_receive(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) override;

private:
    struct Shared {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<std::pair<std::chrono::steady_clock::time_point, std::vector<uint8_t>>> a_to_b;
        std::deque<std::pair<std::chrono::steady_clock::time_point, std::vector<uint8_t>>> b_to_a;
        LoopbackConfig cfg;
        std::mt19937 rng;
    };

    LoopbackLink(std::shared_ptr<Shared> shared, bool is_a);

    std::shared_ptr<Shared> shared_;
    bool is_a_;
};

} // namespace qrtp
