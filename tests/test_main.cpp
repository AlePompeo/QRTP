#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "qrtp/adaptive.hpp"
#include "qrtp/base64.hpp"
#include "qrtp/broadcast.hpp"
#include "qrtp/crc32.hpp"
#include "qrtp/crypto.hpp"
#include "qrtp/fountain.hpp"
#include "qrtp/framing.hpp"
#include "qrtp/link.hpp"
#include "qrtp/protocol.hpp"
#include "qrtp/qr_encoder.hpp"
#include "qrtp/sha256.hpp"

namespace {
int g_failures = 0;
}

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                             \
        } else {                                                                      \
            std::printf("ok   %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                             \
    } while (0)

void test_crc32() {
    const char* check = "123456789";
    const uint32_t c = qrtp::crc32(reinterpret_cast<const uint8_t*>(check), 9);
    CHECK(c == 0xCBF43926u); // standard CRC-32/ISO-HDLC check value
}

void test_base64() {
    // RFC 4648 standard test vectors.
    auto to_bytes = [](const char* s) { return std::vector<uint8_t>(s, s + strlen(s)); };
    CHECK(qrtp::base64_encode({}) == "");
    CHECK(qrtp::base64_encode(to_bytes("f")) == "Zg==");
    CHECK(qrtp::base64_encode(to_bytes("fo")) == "Zm8=");
    CHECK(qrtp::base64_encode(to_bytes("foo")) == "Zm9v");
    CHECK(qrtp::base64_encode(to_bytes("foob")) == "Zm9vYg==");
    CHECK(qrtp::base64_encode(to_bytes("fooba")) == "Zm9vYmE=");
    CHECK(qrtp::base64_encode(to_bytes("foobar")) == "Zm9vYmFy");

    CHECK(qrtp::base64_decode("Zg==") == to_bytes("f"));
    CHECK(qrtp::base64_decode("Zm9vYmFy") == to_bytes("foobar"));

    // The actual bug this exists to fix: binary data riddled with embedded zero bytes (exactly
    // what a real QRTP frame header looks like — e.g. a SYN frame's seq field is 0x00 0x00 0x00
    // 0x00) must round-trip through encode+decode without truncating at the first zero.
    std::vector<uint8_t> binary_with_nulls = {0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xAB, 0x00, 0x00};
    auto roundtripped = qrtp::base64_decode(qrtp::base64_encode(binary_with_nulls));
    CHECK(roundtripped == binary_with_nulls);
    CHECK(roundtripped.size() == binary_with_nulls.size()); // would fail if truncated at a \0

    // Round-trip every length 0..64 of pseudo-random bytes (including zeros) to catch any
    // off-by-one in the padding logic. Covers the full realistic QRTP frame size range: a DATA
    // frame at the default chunk_size_max (1500) plus header/CRC overhead is ~1523 raw bytes.
    std::mt19937 rng(123);
    bool all_ok = true;
    size_t first_bad_len = 0;
    for (size_t len = 0; len <= 1600; ++len) {
        std::vector<uint8_t> data(len);
        for (auto& b : data) b = static_cast<uint8_t>(rng() & 0xFF);
        if (qrtp::base64_decode(qrtp::base64_encode(data)) != data) {
            all_ok = false;
            first_bad_len = len;
            break;
        }
    }
    if (!all_ok) std::fprintf(stderr, "  (first failing length: %zu)\n", first_bad_len);
    CHECK(all_ok);
}

void test_sha256() {
    {
        const auto d = qrtp::sha256(nullptr, 0);
        CHECK(qrtp::sha256_hex(d) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }
    {
        const char* s = "abc";
        const auto d = qrtp::sha256(reinterpret_cast<const uint8_t*>(s), 3);
        CHECK(qrtp::sha256_hex(d) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
}

void test_crypto() {
    // PBKDF2-HMAC-SHA256 known-answer test (password="password", salt="salt", c=1, dkLen=32) --
    // independently computed via Python's hashlib.pbkdf2_hmac, not derived from this code.
    {
        const std::vector<uint8_t> salt = {'s', 'a', 'l', 't'};
        const auto dk = qrtp::pbkdf2_hmac_sha256("password", salt, 1, 32);
        char hex[65] = {0};
        for (size_t i = 0; i < dk.size(); ++i) std::snprintf(hex + i * 2, 3, "%02x", dk[i]);
        CHECK(std::string(hex) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    }

    // AES-256-GCM known-answer test -- independently computed via Python's cryptography
    // library (AESGCM), not derived from this code.
    {
        const std::vector<uint8_t> key = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
        const std::vector<uint8_t> iv = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};
        const std::string pt = "hello qrtp messenger";
        const std::vector<uint8_t> ptv(pt.begin(), pt.end());
        const auto ct = qrtp::aes256gcm_encrypt(key, iv, ptv);
        char hex[2 * (20 + 16) + 1] = {0};
        for (size_t i = 0; i < ct.size(); ++i) std::snprintf(hex + i * 2, 3, "%02x", ct[i]);
        CHECK(std::string(hex) == "2f67ba77aac5b369f931b7e6d49a0b08edb1e24611b58f7fb8d1ab00fd9799fe61defe86");

        const auto decrypted = qrtp::aes256gcm_decrypt(key, iv, ct);
        CHECK(std::string(decrypted.begin(), decrypted.end()) == pt);

        // A flipped ciphertext byte must fail authentication, never silently decrypt to
        // garbage -- this is the actual security property GCM exists to provide.
        auto tampered = ct;
        tampered[0] ^= 0xFF;
        bool threw = false;
        try { qrtp::aes256gcm_decrypt(key, iv, tampered); }
        catch (const std::exception&) { threw = true; }
        CHECK(threw);
    }

    // High-level encrypt_message/decrypt_message round-trip (random salt/iv each call).
    {
        const std::string passphrase = "test passphrase";
        const std::string message = "hello native crypto";
        const std::string encoded = qrtp::encrypt_message(passphrase, message);
        CHECK(qrtp::decrypt_message(passphrase, encoded) == message);

        bool threw = false;
        try { qrtp::decrypt_message("wrong passphrase", encoded); }
        catch (const std::exception&) { threw = true; }
        CHECK(threw);
    }

    // Cross-platform wire-format vector: a full QR payload independently generated via
    // Python (PBKDF2 100k iterations + AES-256-GCM + the exact salt||iv||ciphertext base64
    // layout), matching web/messenger.js byte-for-byte -- proves this native implementation
    // can decrypt a message a browser encrypted, and (by symmetry of the same primitives)
    // vice versa.
    {
        const std::string passphrase = "qrtp-cross-platform-test";
        const std::string b64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGzQd6oAhYyKlxfClryoxgs3DGnxSL0kTfVMZqht+wIfui7E7VdQ5tXpQzygIWeTt/NR/iWhqI1cgziLLgfaG2+Ob5EKEsDroTFpe/w==";
        const std::string expected = "Interoperability test between web and native QRTP messenger";
        CHECK(qrtp::decrypt_message(passphrase, b64) == expected);
    }
}

void test_framing_roundtrip() {
    qrtp::Frame f;
    f.type = qrtp::FrameType::DATA;
    f.session_id = 0xBEEF;
    f.seq = 424242;
    f.ack_window = 7;
    f.flags_byte = qrtp::flags::LAST_FRAME;
    f.payload = {1, 2, 3, 4, 5, 250, 251, 252};

    const auto bytes = qrtp::encode_frame(f);
    const auto decoded = qrtp::decode_frame(bytes);
    CHECK(decoded.has_value());
    if (decoded) {
        CHECK(decoded->session_id == 0xBEEF);
        CHECK(decoded->seq == 424242u);
        CHECK(decoded->payload == f.payload);
    }

    auto corrupted = bytes;
    corrupted[20] ^= 0xFF;
    CHECK(!qrtp::decode_frame(corrupted).has_value()); // CRC must catch a flipped bit

    auto truncated = bytes;
    truncated.resize(5);
    CHECK(!qrtp::decode_frame(truncated).has_value()); // must reject, not crash
}

void test_adaptive_aimd() {
    qrtp::AdaptiveConfig cfg;
    qrtp::AdaptiveController ctrl(cfg);
    const size_t w0 = ctrl.window_size();

    ctrl.record_loss_event();
    CHECK(ctrl.window_size() <= w0); // multiplicative decrease never grows the window
    const size_t w1 = ctrl.window_size();

    for (int i = 0; i < 20; ++i) ctrl.record_clean_round();
    CHECK(ctrl.window_size() >= w1); // sustained clean rounds recover/grow the window
    CHECK(ctrl.rolling_fer() == 0.0);
}

void test_qr_encoder() {
    const std::vector<uint8_t> payload = {'Q', 'R', 'T', 'P', ' ', 't', 'e', 's', 't'};
    const auto qr = qrtp::qr_encode_bytes(payload, qrtp::QrEcLevel::M);
    CHECK(qr.width == 4 * qr.version + 17); // ISO/IEC 18004 version<->size formula
    CHECK(!qr.modules.empty());

    qrtp::write_pbm(qr, "/tmp/qrtp_selftest.pbm");
    std::ifstream check("/tmp/qrtp_selftest.pbm");
    CHECK(check.good());
}

void test_end_to_end_transfer() {
    std::vector<uint8_t> content;
    content.reserve(20000);
    std::mt19937 rng(42);
    for (int i = 0; i < 20000; ++i) content.push_back(static_cast<uint8_t>(rng() & 0xFF));

    const std::string path = "/tmp/qrtp_selftest_input.bin";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    }

    qrtp::LoopbackConfig lcfg;
    lcfg.loss_probability = 0.05;
    lcfg.corruption_probability = 0.03;
    lcfg.latency = std::chrono::milliseconds(2);
    lcfg.seed = 7;
    auto link_pair = qrtp::LoopbackLink::make_pair(lcfg);

    qrtp::ProtocolConfig pcfg;
    pcfg.data_poll_timeout = std::chrono::milliseconds(50);
    pcfg.handshake_timeout = std::chrono::milliseconds(300);

    qrtp::TransferResult result;
    qrtp::TransferMetadata meta;
    std::optional<std::vector<uint8_t>> received;

    std::thread sender_thread([&] {
        qrtp::Sender sender(*link_pair.first, pcfg);
        result = sender.send_file(path);
    });
    std::thread receiver_thread([&] {
        qrtp::Receiver receiver(*link_pair.second, pcfg);
        received = receiver.receive_file(meta, std::chrono::milliseconds(15000));
    });
    sender_thread.join();
    receiver_thread.join();

    CHECK(result.success);
    CHECK(received.has_value());
    if (received) CHECK(*received == content);
}

void test_cancellation() {
    // A big-ish file over a clean channel so the transfer would normally take a while — cancelling
    // shortly after start must make both sides return promptly instead of running to completion.
    std::vector<uint8_t> content(200000, 0xAB);
    const std::string path = "/tmp/qrtp_selftest_cancel_input.bin";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    }

    qrtp::LoopbackConfig lcfg;
    lcfg.latency = std::chrono::milliseconds(2);
    auto link_pair = qrtp::LoopbackLink::make_pair(lcfg);

    std::atomic<bool> cancel{false};
    qrtp::ProtocolConfig pcfg;
    pcfg.data_poll_timeout = std::chrono::milliseconds(50);
    pcfg.cancel = &cancel;

    qrtp::TransferResult result;
    qrtp::TransferMetadata meta;
    std::optional<std::vector<uint8_t>> received;

    std::thread sender_thread([&] {
        qrtp::Sender sender(*link_pair.first, pcfg);
        result = sender.send_file(path);
    });
    std::thread receiver_thread([&] {
        qrtp::Receiver receiver(*link_pair.second, pcfg);
        received = receiver.receive_file(meta, std::chrono::milliseconds(15000));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cancel.store(true);

    // Both threads must stop within a bounded time — this is the actual behavior a GUI "Stop"
    // button depends on, not just that the cancel flag compiles. std::thread has no timed join, so
    // poll completion via a flag set by a small waiter thread on each side instead.
    std::atomic<bool> sender_done{false}, receiver_done{false};
    std::thread waiter_a([&] { sender_thread.join(); sender_done = true; });
    std::thread waiter_b([&] { receiver_thread.join(); receiver_done = true; });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !(sender_done && receiver_done)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(sender_done.load());
    CHECK(receiver_done.load());
    CHECK(!result.success);
    CHECK(!received.has_value());

    waiter_a.join();
    waiter_b.join();
}

void test_broadcast_transfer() {
    // No ARQ/retransmission in this mode — reliability comes entirely from BroadcastSender looping
    // the whole file a bounded number of times, so a chunk lost/corrupted in one loop still has
    // other chances to get through before the sender gives up.
    std::vector<uint8_t> content;
    content.reserve(5000);
    std::mt19937 rng(99);
    for (int i = 0; i < 5000; ++i) content.push_back(static_cast<uint8_t>(rng() & 0xFF));

    const std::string path = "/tmp/qrtp_selftest_broadcast_input.bin";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    }

    qrtp::LoopbackConfig lcfg;
    lcfg.loss_probability = 0.05;
    lcfg.corruption_probability = 0.02;
    lcfg.latency = std::chrono::milliseconds(1);
    lcfg.seed = 11;
    auto link_pair = qrtp::LoopbackLink::make_pair(lcfg);

    qrtp::BroadcastConfig bcfg;
    bcfg.loops = 5; // bounded — enough margin over a ~7% per-loop per-chunk failure rate that no
                     // chunk should plausibly miss all 5 tries, without looping forever if it does
    qrtp::BroadcastResult result;
    qrtp::TransferMetadata meta;
    std::optional<std::vector<uint8_t>> received;

    std::thread sender_thread([&] {
        qrtp::BroadcastSender sender(*link_pair.first, bcfg);
        result = sender.send_file(path);
    });
    std::thread receiver_thread([&] {
        qrtp::BroadcastReceiver receiver(*link_pair.second, bcfg);
        received = receiver.receive_file(meta, std::chrono::milliseconds(15000));
    });
    sender_thread.join();
    receiver_thread.join();

    CHECK(result.success);
    CHECK(received.has_value());
    if (received) CHECK(*received == content);
    CHECK(meta.filesize == content.size());
}

void test_fountain_code() {
    // Pure algorithmic test of the LT code itself (fountain.hpp/fountain.cpp) — no LoopbackLink or
    // threads needed: encode K blocks, simulate symbol loss directly, decode, verify an exact byte
    // match. Fully deterministic (fixed seeds throughout), so a failure here always reproduces.
    constexpr size_t kNumBlocks = 50;
    constexpr size_t kBlockSize = 32;
    std::mt19937 rng(7);
    std::vector<std::vector<uint8_t>> blocks(kNumBlocks);
    for (auto& b : blocks) {
        b.resize(kBlockSize);
        for (auto& byte : b) byte = static_cast<uint8_t>(rng() & 0xFF);
    }

    qrtp::LtDecoder decoder(kNumBlocks, kBlockSize);
    std::mt19937 loss_rng(42);
    std::uniform_real_distribution<double> unif(0.0, 1.0);
    constexpr double kLossProbability = 0.2;
    // Generous cap: well beyond what Robust Soliton should ever need at 20% loss for K=50.
    constexpr uint32_t kMaxSymbols = kNumBlocks * 10;

    uint32_t symbol_index = 0;
    while (!decoder.is_complete() && symbol_index < kMaxSymbols) {
        if (unif(loss_rng) >= kLossProbability) { // symbol "arrives"
            decoder.add_symbol(symbol_index, qrtp::lt_encode_symbol(symbol_index, blocks));
        }
        ++symbol_index;
    }

    CHECK(decoder.is_complete());
    if (decoder.is_complete()) {
        std::vector<uint8_t> expected;
        expected.reserve(kNumBlocks * kBlockSize);
        for (const auto& b : blocks) expected.insert(expected.end(), b.begin(), b.end());
        CHECK(decoder.assemble(expected.size()) == expected);
    }
}

void test_broadcast_high_loss_reliability() {
    // Demonstrates the fountain code's reliability advantage concretely, not just in theory: at
    // this loss rate and loops budget, the *old* per-offset-repeat scheme this replaced would very
    // likely have failed -- for K=100 blocks at p=0.30 loss and loops=3, the expected number of
    // specific chunks that miss *every single one* of the 3 passes is K*p^3 ~= 100*0.027 ~= 2.7, a
    // near-certain failure for that scheme. The fountain code only needs *some* sufficient number
    // of symbols, never those specific ones, so it tolerates this rate comfortably in the same
    // symbol budget (loops*K = 300 symbols available, vastly more than the ~110-140 actually
    // needed to decode 100 blocks with typical Robust Soliton overhead).
    std::vector<uint8_t> content;
    content.reserve(5000);
    std::mt19937 rng(123);
    for (int i = 0; i < 5000; ++i) content.push_back(static_cast<uint8_t>(rng() & 0xFF));

    const std::string path = "/tmp/qrtp_selftest_broadcast_highloss_input.bin";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    }

    qrtp::LoopbackConfig lcfg;
    lcfg.loss_probability = 0.30;
    lcfg.corruption_probability = 0.0; // isolate the loss-tolerance property specifically
    lcfg.latency = std::chrono::milliseconds(1);
    lcfg.seed = 55;
    auto link_pair = qrtp::LoopbackLink::make_pair(lcfg);

    qrtp::BroadcastConfig bcfg;
    bcfg.chunk_size = 50; // -> 100 blocks for a 5000-byte file
    bcfg.loops = 3;
    qrtp::BroadcastResult result;
    qrtp::TransferMetadata meta;
    std::optional<std::vector<uint8_t>> received;

    std::thread sender_thread([&] {
        qrtp::BroadcastSender sender(*link_pair.first, bcfg);
        result = sender.send_file(path);
    });
    std::thread receiver_thread([&] {
        qrtp::BroadcastReceiver receiver(*link_pair.second, bcfg);
        received = receiver.receive_file(meta, std::chrono::milliseconds(15000));
    });
    sender_thread.join();
    receiver_thread.join();

    CHECK(received.has_value());
    if (received) CHECK(*received == content);
}

void test_broadcast_late_join() {
    // Directly engineers the "receiver sees DATA symbols before ever seeing a header" scenario
    // (hand-crafted frames, not a real BroadcastSender, so session id/timing/ordering are fully
    // controlled) to verify BroadcastReceiver::receive_file()'s pre-header buffering actually
    // contributes those symbols to the decode instead of discarding them (the old offset-repeat
    // scheme this replaced discarded any DATA frame seen before the header).
    //
    // The first half of the file's blocks are sent (systematically) *before* any header at all;
    // the header follows; then the second half is sent (also systematically). Decoding needs
    // *both* halves — the second half alone (everything sent after the header) can never cover
    // the first half's block indices, since those were only ever sent pre-header. If the pre-
    // header buffering didn't work, this decode could not complete.
    std::vector<uint8_t> content;
    content.reserve(2000);
    std::mt19937 rng(321);
    for (int i = 0; i < 2000; ++i) content.push_back(static_cast<uint8_t>(rng() & 0xFF));

    constexpr size_t kChunkSize = 40;
    const size_t num_blocks = (content.size() + kChunkSize - 1) / kChunkSize;
    std::vector<std::vector<uint8_t>> blocks(num_blocks);
    for (size_t i = 0; i < num_blocks; ++i) {
        const size_t start = i * kChunkSize;
        const size_t len = std::min(kChunkSize, content.size() - start);
        blocks[i].assign(kChunkSize, 0);
        std::copy(content.begin() + static_cast<long>(start), content.begin() + static_cast<long>(start + len),
                   blocks[i].begin());
    }
    const auto digest = qrtp::sha256(content.data(), content.size());
    constexpr uint16_t kSession = 0xABCD;

    qrtp::LoopbackConfig lcfg;
    lcfg.latency = std::chrono::milliseconds(1);
    lcfg.seed = 77;
    auto link_pair = qrtp::LoopbackLink::make_pair(lcfg);

    qrtp::BroadcastConfig bcfg;
    bcfg.chunk_size = kChunkSize;

    qrtp::TransferMetadata meta;
    std::optional<std::vector<uint8_t>> received;
    std::thread receiver_thread([&] {
        qrtp::BroadcastReceiver receiver(*link_pair.second, bcfg);
        received = receiver.receive_file(meta, std::chrono::milliseconds(5000));
    });

    const size_t half = num_blocks / 2;
    for (uint32_t s = 0; s < half; ++s) { // pre-header: first half, systematic (s < num_blocks)
        qrtp::Frame f;
        f.type = qrtp::FrameType::DATA;
        f.session_id = kSession;
        f.seq = s;
        f.payload = qrtp::lt_encode_symbol(s, blocks);
        link_pair.first->send(qrtp::encode_frame(f));
    }

    qrtp::Frame header;
    header.type = qrtp::FrameType::SYN;
    header.session_id = kSession;
    header.payload = qrtp::pack_file_metadata("test.bin", content.size(), digest);
    link_pair.first->send(qrtp::encode_frame(header));

    for (uint32_t s = static_cast<uint32_t>(half); s < num_blocks; ++s) { // post-header: second half
        qrtp::Frame f;
        f.type = qrtp::FrameType::DATA;
        f.session_id = kSession;
        f.seq = s;
        f.payload = qrtp::lt_encode_symbol(s, blocks);
        link_pair.first->send(qrtp::encode_frame(f));
    }

    receiver_thread.join();

    CHECK(received.has_value());
    if (received) CHECK(*received == content);
}

int main() {
    test_crc32();
    test_base64();
    test_sha256();
    test_crypto();
    test_framing_roundtrip();
    test_adaptive_aimd();
    test_qr_encoder();
    test_end_to_end_transfer();
    test_cancellation();
    test_broadcast_transfer();
    test_fountain_code();
    test_broadcast_high_loss_reliability();
    test_broadcast_late_join();

    std::printf("\n%s (%d failing check%s)\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
