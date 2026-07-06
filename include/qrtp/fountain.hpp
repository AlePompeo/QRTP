#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace qrtp {

// LT (Luby Transform) fountain code — M. Luby, "LT Codes", FOCS 2002. Used by BroadcastSender/
// BroadcastReceiver (broadcast.cpp) instead of plain repeat-the-whole-file looping: the sender
// emits a potentially endless stream of encoded symbols (XOR combinations of the source blocks),
// and the receiver only needs *some sufficient number* of them, not any specific ones — the
// standard scheme for broadcast/multicast delivery (DVB-H, 3GPP MBMS use Raptor codes, a
// systematic extension of this same idea).
//
// Symbol indices 0..num_blocks-1 are systematic (degree 1, that exact source block, unmodified) —
// a clean channel behaves identically to the old plain-looping scheme with zero overhead; the
// fountain code only kicks in for symbol_index >= num_blocks. Both sides derive the same degree
// and source-block indices for a given symbol from symbol_index alone (a seeded PRNG) — nothing
// else needs to cross the wire, so a bare (seq, payload) DATA frame is all a symbol ever needs.

// Degree for a given symbol under the Robust Soliton distribution (small num_blocks <= 2 always
// returns 1: fountain coding has no benefit at that scale, plain repetition is just as good and
// simpler). Deterministic — must be called identically on both sender and receiver.
size_t lt_degree_for_symbol(uint32_t symbol_index, size_t num_blocks);

// The distinct source-block indices (0..num_blocks-1) that make up a given symbol. For
// symbol_index < num_blocks this is just {symbol_index} (the systematic phase, see above).
std::vector<size_t> lt_indices_for_symbol(uint32_t symbol_index, size_t num_blocks);

// XORs together the source blocks selected by lt_indices_for_symbol(symbol_index, blocks.size()).
// Every block must be exactly the same size (callers zero-pad the file's last, short block to
// chunk_size — same padding convention on both sides, truncated back off at final assembly).
std::vector<uint8_t> lt_encode_symbol(uint32_t symbol_index, const std::vector<std::vector<uint8_t>>& blocks);

// Peeling (belief-propagation) decoder: accumulates encoded symbols and resolves source blocks the
// moment enough independent equations have arrived, using the exact same degree/index rule the
// encoder used — never needs to know which specific symbols the sender sent, or in what order.
class LtDecoder {
public:
    LtDecoder(size_t num_blocks, size_t block_size);

    // Returns false only if this symbol carried no new information at all (every source block it
    // touches was already resolved) — informational for logging, not required for correctness.
    bool add_symbol(uint32_t symbol_index, std::vector<uint8_t> payload);

    bool is_complete() const { return resolved_count_ == num_blocks_; }
    size_t resolved_count() const { return resolved_count_; }
    size_t num_blocks() const { return num_blocks_; }

    // Concatenates all resolved blocks and truncates to `filesize` (undoing the sender's zero-pad
    // on the last block) — only meaningful once is_complete().
    std::vector<uint8_t> assemble(uint64_t filesize) const;

private:
    struct Equation {
        std::vector<size_t> indices; // remaining unresolved source-block indices, distinct
        std::vector<uint8_t> value;  // running XOR, already had every resolved index cancelled out
    };

    size_t num_blocks_;
    size_t block_size_;
    std::vector<std::optional<std::vector<uint8_t>>> resolved_; // size num_blocks_
    size_t resolved_count_ = 0;

    std::map<size_t, Equation> waiting_;             // degree >= 2 equations, keyed by a stable id
    std::vector<std::vector<size_t>> referencing_;   // per block index, which waiting_ ids still need it
    size_t next_eq_id_ = 0;

    // Marks `index` resolved to `value`, then iteratively cascades: subtracts it out of every
    // waiting equation that referenced it, resolving any that drop to degree 1 in turn (a
    // worklist, not recursion — a single symbol can trigger a long cascade for a large file).
    void resolve_block(size_t index, std::vector<uint8_t> value);
};

} // namespace qrtp
