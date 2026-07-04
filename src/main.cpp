#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "qrtp/link.hpp"
#include "qrtp/protocol.hpp"
#include "qrtp/qr_encoder.hpp"
#ifdef QRTP_WITH_OPENCV
#include "qrtp/optical_link.hpp"
#endif

namespace {

void print_usage() {
    std::printf(
        "QRTP - QR Transfer Protocol\n\n"
        "Usage:\n"
        "  qrtp render-qr <text> <output.pbm>\n"
        "      Render text as a real QR code (PBM image) — proves the QR encoder works.\n\n"
        "  qrtp demo <file> [--loss P] [--corrupt P] [--out DIR]\n"
        "      Full send+receive transfer over an in-process loopback channel, with optional\n"
        "      synthetic loss/corruption (P in [0,1]). No camera/monitor required.\n\n"
#ifdef QRTP_WITH_OPENCV
        "  qrtp send <file> [--camera N] [--fps F]\n"
        "      Send over the real optical link: renders QR frames to a window and reads\n"
        "      ACKs/SACKs back from camera index N (the peer's screen must face this camera).\n\n"
        "  qrtp receive --out <dir> [--camera N] [--fps F]\n"
        "      Receive over the real optical link.\n");
#else
        "  qrtp send / qrtp receive\n"
        "      Not available in this build — rebuild with `make WITH_OPENCV=1` after\n"
        "      installing opencv-devel to enable the real screen+camera link.\n");
#endif
}

int cmd_render_qr(int argc, char** argv) {
    if (argc < 4) {
        print_usage();
        return 1;
    }
    const std::string text = argv[2];
    const std::string out = argv[3];
    try {
        const std::vector<uint8_t> data(text.begin(), text.end());
        const auto qr = qrtp::qr_encode_bytes(data, qrtp::QrEcLevel::M);
        qrtp::write_pbm(qr, out);
        std::printf("QR version %d, %dx%d modules -> %s\n", qr.version, qr.width, qr.width, out.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "render-qr failed: %s\n", e.what());
        return 1;
    }
}

int cmd_demo(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }
    const std::string path = argv[2];
    double loss = 0.0, corrupt = 0.0;
    std::string out_dir = ".";
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--loss" && i + 1 < argc) loss = std::stod(argv[++i]);
        else if (a == "--corrupt" && i + 1 < argc) corrupt = std::stod(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
    }

    qrtp::LoopbackConfig lcfg;
    lcfg.loss_probability = loss;
    lcfg.corruption_probability = corrupt;
    lcfg.latency = std::chrono::milliseconds(5);
    auto link_pair = qrtp::LoopbackLink::make_pair(lcfg);

    qrtp::ProtocolConfig cfg;
    cfg.log = [](const std::string& m) { std::printf("[qrtp] %s\n", m.c_str()); };

    qrtp::TransferResult result;
    qrtp::TransferMetadata meta;
    std::optional<std::vector<uint8_t>> received;

    std::thread sender_thread([&] {
        qrtp::Sender sender(*link_pair.first, cfg);
        result = sender.send_file(path);
    });
    std::thread receiver_thread([&] {
        qrtp::Receiver receiver(*link_pair.second, cfg);
        received = receiver.receive_file(meta, std::chrono::seconds(60));
    });
    sender_thread.join();
    receiver_thread.join();

    std::printf("\nSender result:   %s (%s)\n", result.success ? "OK" : "FAILED", result.message.c_str());
    if (!received) {
        std::printf("Receiver result: FAILED (no data / hash mismatch)\n");
        return 1;
    }
    const std::filesystem::path out_path = std::filesystem::path(out_dir) / meta.filename;
    std::ofstream f(out_path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(received->data()), static_cast<std::streamsize>(received->size()));
    std::printf("Receiver result: OK, wrote %zu bytes to %s\n", received->size(), out_path.string().c_str());
    return result.success ? 0 : 1;
}

#ifdef QRTP_WITH_OPENCV
int cmd_send_optical(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }
    const std::string path = argv[2];
    qrtp::OpticalLinkConfig ocfg;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--camera" && i + 1 < argc) ocfg.camera_index = std::stoi(argv[++i]);
        else if (a == "--fps" && i + 1 < argc) ocfg.fps = std::stod(argv[++i]);
    }
    try {
        qrtp::OpticalLink link(ocfg);
        qrtp::ProtocolConfig cfg;
        cfg.log = [](const std::string& m) { std::printf("[qrtp] %s\n", m.c_str()); };
        qrtp::Sender sender(link, cfg);
        const auto result = sender.send_file(path);
        std::printf("%s: %s\n", result.success ? "OK" : "FAILED", result.message.c_str());
        return result.success ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "send failed: %s\n", e.what());
        return 1;
    }
}

int cmd_receive_optical(int argc, char** argv) {
    qrtp::OpticalLinkConfig ocfg;
    std::string out_dir = ".";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--camera" && i + 1 < argc) ocfg.camera_index = std::stoi(argv[++i]);
        else if (a == "--fps" && i + 1 < argc) ocfg.fps = std::stod(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
    }
    try {
        qrtp::OpticalLink link(ocfg);
        qrtp::ProtocolConfig cfg;
        cfg.log = [](const std::string& m) { std::printf("[qrtp] %s\n", m.c_str()); };
        qrtp::Receiver receiver(link, cfg);
        qrtp::TransferMetadata meta;
        auto received = receiver.receive_file(meta, std::chrono::minutes(30));
        if (!received) {
            std::fprintf(stderr, "receive failed\n");
            return 1;
        }
        const std::filesystem::path out_path = std::filesystem::path(out_dir) / meta.filename;
        std::ofstream f(out_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(received->data()), static_cast<std::streamsize>(received->size()));
        std::printf("OK, wrote %zu bytes to %s\n", received->size(), out_path.string().c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "receive failed: %s\n", e.what());
        return 1;
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string cmd = argv[1];
    if (cmd == "render-qr") return cmd_render_qr(argc, argv);
    if (cmd == "demo") return cmd_demo(argc, argv);
#ifdef QRTP_WITH_OPENCV
    if (cmd == "send") return cmd_send_optical(argc, argv);
    if (cmd == "receive") return cmd_receive_optical(argc, argv);
#else
    if (cmd == "send" || cmd == "receive") {
        std::fprintf(stderr,
                      "'%s' requires a build with real display/camera support: rebuild with "
                      "`make WITH_OPENCV=1` after installing opencv-devel.\n",
                      cmd.c_str());
        return 1;
    }
#endif
    print_usage();
    return 1;
}
