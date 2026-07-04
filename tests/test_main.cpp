#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "qrtp/adaptive.hpp"
#include "qrtp/crc32.hpp"
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

int main() {
    test_crc32();
    test_sha256();
    test_framing_roundtrip();
    test_adaptive_aimd();
    test_qr_encoder();
    test_end_to_end_transfer();
    test_cancellation();

    std::printf("\n%s (%d failing check%s)\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
