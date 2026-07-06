#include "qrtp/fountain.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <stdexcept>

namespace qrtp {
namespace {

// Robust Soliton distribution parameters (Luby, "LT Codes", FOCS 2002) — c and delta are the
// standard tunable knobs; these specific values are the commonly-cited defaults in the practical
// fountain-coding literature (not derived for this project specifically, just conventional
// starting points that work well across a wide range of block counts).
constexpr double kC = 0.1;
constexpr double kDelta = 0.05;

// Builds the (normalized) Robust Soliton CDF over degrees 1..K. Cached per distinct K: a single
// broadcast transfer calls this for every symbol, so building it once per K (not once per symbol)
// matters more than the cache being reused across separate transfers.
const std::vector<double>& robust_soliton_cdf(size_t k) {
    static std::map<size_t, std::vector<double>> cache;
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;

    std::vector<double> mu(k + 1, 0.0); // 1-indexed; mu[0] unused
    mu[1] = 1.0 / static_cast<double>(k);
    for (size_t i = 2; i <= k; ++i) mu[i] = 1.0 / (static_cast<double>(i) * static_cast<double>(i - 1));

    const double kd = static_cast<double>(k);
    const double r = kC * std::log(kd / kDelta) * std::sqrt(kd);
    const size_t r_floor = std::max<size_t>(1, static_cast<size_t>(r));
    for (size_t i = 1; i < r_floor && i <= k; ++i) mu[i] += r / (static_cast<double>(i) * kd);
    if (r_floor <= k) mu[r_floor] += r * std::log(r / kDelta) / kd;

    double sum = 0.0;
    for (size_t i = 1; i <= k; ++i) sum += mu[i];

    std::vector<double> cdf(k + 1, 0.0);
    double acc = 0.0;
    for (size_t i = 1; i <= k; ++i) {
        acc += mu[i] / sum;
        cdf[i] = acc;
    }
    cdf[k] = 1.0; // guards against floating-point drift leaving the top just under 1.0

    return cache.emplace(k, std::move(cdf)).first->second;
}

} // namespace

size_t lt_degree_for_symbol(uint32_t symbol_index, size_t num_blocks) {
    if (num_blocks <= 2) return 1; // no benefit at this scale — see fountain.hpp
    // Seeded by symbol_index alone: sender and receiver, given the same symbol_index, must derive
    // the exact same degree without exchanging anything else.
    std::mt19937 rng(symbol_index);
    std::uniform_real_distribution<double> unif(0.0, 1.0);
    const double u = unif(rng);
    const auto& cdf = robust_soliton_cdf(num_blocks);

    size_t lo = 1, hi = num_blocks;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cdf[mid] >= u) hi = mid; else lo = mid + 1;
    }
    return lo;
}

std::vector<size_t> lt_indices_for_symbol(uint32_t symbol_index, size_t num_blocks) {
    if (symbol_index < num_blocks) return {static_cast<size_t>(symbol_index)}; // systematic phase
    const size_t degree = std::min(lt_degree_for_symbol(symbol_index, num_blocks), num_blocks);

    // A distinct seed from the degree draw above (XORed with a fixed odd constant, the classic
    // "decorrelate a derived seed" trick) so this draw doesn't depend on lt_degree_for_symbol's
    // own RNG having already been advanced — each function reseeds fresh from symbol_index, so
    // either can be called independently and both still agree with each other.
    std::mt19937 rng(symbol_index ^ 0x9E3779B9u);
    std::vector<size_t> pool(num_blocks);
    std::iota(pool.begin(), pool.end(), size_t{0});
    // Partial Fisher-Yates: the first `degree` entries after this loop are `degree` distinct
    // indices drawn uniformly at random from [0, num_blocks).
    for (size_t i = 0; i < degree; ++i) {
        std::uniform_int_distribution<size_t> pick(i, num_blocks - 1);
        std::swap(pool[i], pool[pick(rng)]);
    }
    pool.resize(degree);
    return pool;
}

std::vector<uint8_t> lt_encode_symbol(uint32_t symbol_index, const std::vector<std::vector<uint8_t>>& blocks) {
    const auto indices = lt_indices_for_symbol(symbol_index, blocks.size());
    std::vector<uint8_t> out(blocks[indices[0]].size(), 0);
    for (size_t idx : indices) {
        const auto& b = blocks[idx];
        for (size_t i = 0; i < out.size(); ++i) out[i] ^= b[i];
    }
    return out;
}

LtDecoder::LtDecoder(size_t num_blocks, size_t block_size)
    : num_blocks_(num_blocks), block_size_(block_size), resolved_(num_blocks), referencing_(num_blocks) {}

bool LtDecoder::add_symbol(uint32_t symbol_index, std::vector<uint8_t> payload) {
    if (is_complete()) return false;

    const auto indices = lt_indices_for_symbol(symbol_index, num_blocks_);
    std::vector<size_t> remaining;
    remaining.reserve(indices.size());
    for (size_t idx : indices) {
        if (resolved_[idx]) {
            const auto& rv = *resolved_[idx];
            for (size_t b = 0; b < payload.size(); ++b) payload[b] ^= rv[b];
        } else {
            remaining.push_back(idx);
        }
    }

    if (remaining.empty()) return false; // every block this symbol touches was already known
    if (remaining.size() == 1) {
        resolve_block(remaining[0], std::move(payload));
        return true;
    }

    const size_t eq_id = next_eq_id_++;
    for (size_t idx : remaining) referencing_[idx].push_back(eq_id);
    waiting_.emplace(eq_id, Equation{std::move(remaining), std::move(payload)});
    return true;
}

void LtDecoder::resolve_block(size_t index, std::vector<uint8_t> value) {
    // Iterative worklist, not recursion: a single symbol can cascade-resolve a long chain of
    // blocks for a large file, and this must not risk a stack overflow doing it.
    std::vector<std::pair<size_t, std::vector<uint8_t>>> queue;
    queue.emplace_back(index, std::move(value));

    while (!queue.empty()) {
        auto [idx, val] = std::move(queue.back());
        queue.pop_back();
        if (resolved_[idx]) continue; // already resolved via another branch of this same cascade

        resolved_[idx] = val;
        ++resolved_count_;

        auto refs = std::move(referencing_[idx]);
        referencing_[idx].clear();
        for (size_t eq_id : refs) {
            auto it = waiting_.find(eq_id);
            if (it == waiting_.end()) continue; // already consumed via another index in this cascade
            Equation& eq = it->second;

            const auto pos = std::find(eq.indices.begin(), eq.indices.end(), idx);
            if (pos == eq.indices.end()) continue;
            eq.indices.erase(pos);
            for (size_t b = 0; b < eq.value.size(); ++b) eq.value[b] ^= val[b];

            if (eq.indices.empty()) {
                waiting_.erase(it); // fully consumed, redundant (value should be all-zero)
            } else if (eq.indices.size() == 1) {
                const size_t only = eq.indices[0];
                auto v = std::move(eq.value);
                waiting_.erase(it);
                queue.emplace_back(only, std::move(v));
            }
        }
    }
}

std::vector<uint8_t> LtDecoder::assemble(uint64_t filesize) const {
    std::vector<uint8_t> full;
    full.reserve(num_blocks_ * block_size_);
    for (size_t i = 0; i < num_blocks_; ++i) {
        if (!resolved_[i]) throw std::logic_error("LtDecoder::assemble() called before is_complete()");
        full.insert(full.end(), resolved_[i]->begin(), resolved_[i]->end());
    }
    full.resize(filesize);
    return full;
}

} // namespace qrtp
