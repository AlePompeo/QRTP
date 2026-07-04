#include "qrtp/link.hpp"

#include <algorithm>

namespace qrtp {

std::pair<std::shared_ptr<LoopbackLink>, std::shared_ptr<LoopbackLink>> LoopbackLink::make_pair(
    const LoopbackConfig& cfg) {
    auto shared = std::make_shared<Shared>();
    shared->cfg = cfg;
    shared->rng.seed(cfg.seed);
    auto a = std::shared_ptr<LoopbackLink>(new LoopbackLink(shared, true));
    auto b = std::shared_ptr<LoopbackLink>(new LoopbackLink(shared, false));
    return {a, b};
}

LoopbackLink::LoopbackLink(std::shared_ptr<Shared> shared, bool is_a)
    : shared_(std::move(shared)), is_a_(is_a) {}

void LoopbackLink::send(const std::vector<uint8_t>& frame_bytes) {
    std::lock_guard<std::mutex> lock(shared_->mtx);

    std::uniform_real_distribution<double> uni(0.0, 1.0);
    if (uni(shared_->rng) < shared_->cfg.loss_probability) {
        return; // simulated optical drop — the frame simply never arrives
    }

    std::vector<uint8_t> bytes = frame_bytes;
    if (!bytes.empty() && uni(shared_->rng) < shared_->cfg.corruption_probability) {
        std::uniform_int_distribution<size_t> pick_byte(0, bytes.size() - 1);
        std::uniform_int_distribution<int> pick_bit(0, 7);
        bytes[pick_byte(shared_->rng)] ^= uint8_t(1u << pick_bit(shared_->rng));
    }

    const auto arrival = std::chrono::steady_clock::now() + shared_->cfg.latency;
    auto& queue = is_a_ ? shared_->a_to_b : shared_->b_to_a;
    queue.emplace_back(arrival, std::move(bytes));
    shared_->cv.notify_all();
}

bool LoopbackLink::try_receive(std::vector<uint8_t>& out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(shared_->mtx);
    auto& queue = is_a_ ? shared_->b_to_a : shared_->a_to_b;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (!queue.empty() && queue.front().first <= now) {
            out = std::move(queue.front().second);
            queue.pop_front();
            return true;
        }
        if (now >= deadline) return false;

        auto wake_at = deadline;
        if (!queue.empty()) wake_at = std::min(wake_at, queue.front().first);
        shared_->cv.wait_until(lock, wake_at);
    }
}

} // namespace qrtp
