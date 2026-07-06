#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "qrtp/broadcast.hpp"
#include "qrtp/link.hpp"
#include "qrtp/protocol.hpp"
#include "qrtp/qr_encoder.hpp"
#ifdef QRTP_WITH_OPENCV
#include "qrtp/optical_link.hpp"
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
        "  qrtp send <file> [--camera N] [--fps F] [--width W --height H] [--qr-grid N] [--no-mjpeg]\n"
        "                   [--lock-focus] [--no-wechat-qrcode] [--wechat-model-dir DIR]\n"
        "                   [--no-contrast-boost] [--chunk-max N]\n"
        "      Send over the real optical link: renders QR frames to a window and reads\n"
        "      ACKs/SACKs back from camera index N (the peer's screen must face this camera).\n"
        "      --qr-grid N (default 1, classic/original behavior) batches N frames into a single\n"
        "      on-screen grid per display update instead of one at a time, decoded via OpenCV's\n"
        "      detectAndDecodeMulti(). Experimental and opt-in — a runtime toggle specifically so\n"
        "      it can be turned back off (to 1) without needing a rebuild if it doesn't hold up;\n"
        "      must be set the same on both sides of the transfer.\n"
        "      --width/--height request a capture resolution from the camera — defaults to a fixed\n"
        "      640x480 (2026-07-05; was 1280x720 per earlier real hardware testing, lowered to pair\n"
        "      with WeChatQRCode below). Pass --width -1 --height -1 to opt back into auto-max\n"
        "      (auto-detecting the camera's own highest mode — an earlier test showed this same\n"
        "      request negotiating cleanly to 1280x720 on one machine but to a worse mode on\n"
        "      another, so it's not the default), --width 0 --height 0 to skip any request and leave\n"
        "      the camera fully alone, or any other specific WxH. Check the logged \"camera opened:\n"
        "      resolution ...\" line to see what actually got negotiated either way.\n"
        "      MJPEG capture is requested by default (real hardware testing on 2026-07-05 showed\n"
        "      raw/uncompressed capture costing ~100x more per frame on at least one camera, with\n"
        "      no reliability benefit from disabling it). Pass --no-mjpeg to request raw capture\n"
        "      instead, if a specific camera does better with it — check the logged \"fourcc=\" /\n"
        "      avg_capture_ms stats to compare.\n"
        "      --lock-focus disables continuous autofocus after opening the camera (off by\n"
        "      default). Experimental: a real log showed avg_capture_ms/avg_decode_ms dropping\n"
        "      together sharply right at a fps transition on a genuine two-machine setup, which\n"
        "      doesn't fit pure camera-hardware cost — continuous autofocus hunting under fast-\n"
        "      changing on-screen content is one candidate explanation; not yet confirmed on real\n"
        "      hardware.\n"
        "      Decodes using OpenCV's WeChatQRCode by default (opencv_contrib: an SSD-style detector\n"
        "      CNN plus a super-resolution step for small/blurry crops) instead of the classic\n"
        "      cv::QRCodeDetector. Needs detect.prototxt, detect.caffemodel, sr.prototxt,\n"
        "      sr.caffemodel in a \"models/wechat_qrcode\" folder next to this executable (or wherever\n"
        "      --wechat-model-dir points) — see models/wechat_qrcode/LICENSE for their source and\n"
        "      terms. Expected to cost somewhat more CPU per attempt than the classic detector, in\n"
        "      exchange for (unverified end-to-end on real hardware) better detection of small/\n"
        "      blurry/angled codes. Pass --no-wechat-qrcode to use the classic detector instead; must\n"
        "      be set the same on both sides either way.\n"
        "      Requests higher camera contrast and zero saturation by default, aimed at washed-out/\n"
        "      low-contrast captures that trip up a QR decoder (dim monitor backlight, glare,\n"
        "      ambient light). Pass --no-contrast-boost to leave the camera's own contrast/\n"
        "      saturation settings untouched, if a specific camera is already well-tuned.\n"
        "      --chunk-max N overrides AdaptiveConfig::chunk_size_max (default 180 bytes of file\n"
        "      payload per DATA frame — the AIMD's ceiling on how dense a QR code's payload is\n"
        "      allowed to grow to). Higher values pack more bytes per QR (fewer QR codes needed for\n"
        "      the same file) at the cost of a higher QR version — larger, more finely-detailed\n"
        "      codes that are more sensitive to camera resolution/focus/distance; lower values do\n"
        "      the opposite. Already safe regardless of value: upshift() also caps growth by what\n"
        "      the current EC level can actually encode, so this can't ask for a payload too large\n"
        "      to fit any QR version. Must be set the same on both sides of the transfer.\n\n"
        "  qrtp receive --out <dir> [--camera N] [--fps F] [--width W --height H] [--qr-grid N]\n"
        "                          [--no-mjpeg] [--lock-focus] [--no-wechat-qrcode]\n"
        "                          [--wechat-model-dir DIR] [--no-contrast-boost] [--chunk-max N]\n"
        "      Receive over the real optical link. Same --width/--height, --qr-grid, --no-mjpeg,\n"
        "      --lock-focus, --no-wechat-qrcode, --wechat-model-dir, --no-contrast-boost and\n"
        "      --chunk-max notes as above.\n\n"
        "  qrtp broadcast <file> [--camera N] [--fps F] [--chunk-size N] [--ec-level L|M|Q|H]\n"
        "                        [--loops N] [--width W --height H] [--no-mjpeg] [--lock-focus]\n"
        "                        [--no-wechat-qrcode] [--no-contrast-boost] [...]\n"
        "      One-way (simplex) transfer: coexists with `send`/`receive` as a separate mode, for\n"
        "      when there is no return channel at all — the sender never expects, or is able to\n"
        "      receive, any reply. Reliability comes entirely from repetition instead of ARQ: this\n"
        "      loops the whole file (a header frame with filename/size/hash, then every fixed-size\n"
        "      chunk) either forever (--loops 0, the default — a continuous broadcast/kiosk-style\n"
        "      transmission) or a fixed number of times (--loops N). --chunk-size (default 180\n"
        "      bytes) and --ec-level (default Q — higher than send/receive's M, since a lost/\n"
        "      corrupted read here isn't retried until the next loop, so extra per-QR redundancy\n"
        "      directly improves the odds each individual read succeeds) are both fixed for the\n"
        "      whole broadcast, unlike the AIMD-adapted values `send`/`receive` use. Same\n"
        "      --camera/--width/--height/--no-mjpeg/--lock-focus/--no-wechat-qrcode/\n"
        "      --no-contrast-boost/--wechat-model-dir flags as `send`. --qr-grid is accepted but\n"
        "      always forced to 1: multi-QR-per-frame batching needs a listener to trigger each\n"
        "      flush, which this send-only mode never does.\n\n"
        "  qrtp listen --out <dir> [--camera N] [--fps F] [--chunk-size N] [--ec-level L|M|Q|H]\n"
        "                         [--timeout SECONDS] [--width W --height H] [--no-mjpeg]\n"
        "                         [--lock-focus] [--no-wechat-qrcode] [--no-contrast-boost] [...]\n"
        "      Receives a `broadcast` transmission. Only needs a camera pointed at the sender's\n"
        "      screen — never sends anything back. Assembles the file from whatever it manages to\n"
        "      capture across one or more of the sender's loops, finishing the moment it has the\n"
        "      complete file with a verified SHA-256 hash (does not need the sender's current loop\n"
        "      to finish first). --chunk-size/--ec-level must match what the sender used.\n"
        "      --timeout (default 1800s) is the overall giving-up point if the file never\n"
        "      completes. Same camera flags as `receive`.\n");
#else
        "  qrtp send / qrtp receive / qrtp broadcast / qrtp listen\n"
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
// Camera/capture flags shared by all four optical subcommands (send/receive/broadcast/listen).
// Returns true if `a` was recognized and consumed (advancing i for flags that take a value).
bool parse_camera_flag(const std::string& a, int argc, char** argv, int& i, qrtp::OpticalLinkConfig& ocfg) {
    if (a == "--camera" && i + 1 < argc) ocfg.camera_index = std::stoi(argv[++i]);
    else if (a == "--width" && i + 1 < argc) ocfg.camera_width = std::stoi(argv[++i]);
    else if (a == "--height" && i + 1 < argc) ocfg.camera_height = std::stoi(argv[++i]);
    else if (a == "--qr-grid" && i + 1 < argc) ocfg.qr_codes_per_frame = std::stoi(argv[++i]);
    else if (a == "--no-mjpeg") ocfg.request_mjpeg = false;
    else if (a == "--lock-focus") ocfg.disable_autofocus = true;
    else if (a == "--no-wechat-qrcode") ocfg.use_wechat_qrcode = false;
    else if (a == "--wechat-model-dir" && i + 1 < argc) ocfg.wechat_qrcode_model_dir = argv[++i];
    else if (a == "--no-contrast-boost") ocfg.enhance_camera_contrast = false;
    else return false;
    return true;
}

int cmd_send_optical(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }
    const std::string path = argv[2];
    qrtp::OpticalLinkConfig ocfg;
    std::optional<size_t> chunk_max_override;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fps" && i + 1 < argc) ocfg.fps = std::stod(argv[++i]);
        else if (a == "--chunk-max" && i + 1 < argc) chunk_max_override = static_cast<size_t>(std::stoul(argv[++i]));
        else parse_camera_flag(a, argc, argv, i, ocfg);
    }
    try {
        ocfg.log = [](const std::string& m) { std::printf("[qrtp] %s\n", m.c_str()); };
        qrtp::OpticalLink link(ocfg);
        qrtp::ProtocolConfig cfg;
        cfg.log = ocfg.log;
        if (chunk_max_override) cfg.adaptive.chunk_size_max = *chunk_max_override;
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
    std::optional<size_t> chunk_max_override;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fps" && i + 1 < argc) ocfg.fps = std::stod(argv[++i]);
        else if (a == "--chunk-max" && i + 1 < argc) chunk_max_override = static_cast<size_t>(std::stoul(argv[++i]));
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else parse_camera_flag(a, argc, argv, i, ocfg);
    }
    try {
        ocfg.log = [](const std::string& m) { std::printf("[qrtp] %s\n", m.c_str()); };
        qrtp::OpticalLink link(ocfg);
        qrtp::ProtocolConfig cfg;
        cfg.log = ocfg.log;
        if (chunk_max_override) cfg.adaptive.chunk_size_max = *chunk_max_override;
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

int cmd_ec_level_from_flag(const std::string& lvl, qrtp::ECLevel& out) {
    if (lvl == "L") out = qrtp::ECLevel::L;
    else if (lvl == "M") out = qrtp::ECLevel::M;
    else if (lvl == "Q") out = qrtp::ECLevel::Q;
    else if (lvl == "H") out = qrtp::ECLevel::H;
    else return 1;
    return 0;
}

int cmd_broadcast_optical(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }
    const std::string path = argv[2];
    qrtp::OpticalLinkConfig ocfg;
    qrtp::BroadcastConfig bcfg;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fps" && i + 1 < argc) bcfg.fps = std::stod(argv[++i]);
        else if (a == "--chunk-size" && i + 1 < argc) bcfg.chunk_size = static_cast<size_t>(std::stoul(argv[++i]));
        else if (a == "--ec-level" && i + 1 < argc) cmd_ec_level_from_flag(argv[++i], bcfg.ec_level);
        else if (a == "--loops" && i + 1 < argc) bcfg.loops = std::stoi(argv[++i]);
        else parse_camera_flag(a, argc, argv, i, ocfg);
    }
    try {
        ocfg.fps = bcfg.fps; // only affects OpticalLink's initial "camera opened: ..." log context —
                              // send_file() below overrides it immediately via set_fps().
        // Multi-QR-per-frame batching (--qr-grid, parsed by parse_camera_flag above) isn't
        // compatible with this mode: BroadcastSender never calls try_receive(), so a batch that
        // didn't fill to qr_codes_per_frame would never get flushed to screen at all (see
        // OpticalLink::try_receive_multi()'s flush-on-poll logic, never reached here). Forced to 1
        // regardless of what was passed.
        ocfg.qr_codes_per_frame = 1;
        bcfg.log = [](const std::string& m) { std::printf("[qrtp] %s\n", m.c_str()); };
        ocfg.log = bcfg.log;
        qrtp::OpticalLink link(ocfg);
        qrtp::BroadcastSender sender(link, bcfg);
        const auto result = sender.send_file(path);
        std::printf("%s: %s\n", result.success ? "OK" : "FAILED", result.message.c_str());
        return result.success ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "broadcast failed: %s\n", e.what());
        return 1;
    }
}

int cmd_listen_optical(int argc, char** argv) {
    qrtp::OpticalLinkConfig ocfg;
    qrtp::BroadcastConfig bcfg;
    std::string out_dir = ".";
    std::chrono::seconds timeout(1800); // 30 minutes, matches `qrtp receive`'s own default
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fps" && i + 1 < argc) bcfg.fps = std::stod(argv[++i]);
        else if (a == "--chunk-size" && i + 1 < argc) bcfg.chunk_size = static_cast<size_t>(std::stoul(argv[++i]));
        else if (a == "--ec-level" && i + 1 < argc) cmd_ec_level_from_flag(argv[++i], bcfg.ec_level);
        else if (a == "--timeout" && i + 1 < argc) timeout = std::chrono::seconds(std::stoi(argv[++i]));
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else parse_camera_flag(a, argc, argv, i, ocfg);
    }
    try {
        ocfg.fps = bcfg.fps;
        ocfg.qr_codes_per_frame = 1; // see cmd_broadcast_optical's own comment on this
        bcfg.log = [](const std::string& m) { std::printf("[qrtp] %s\n", m.c_str()); };
        ocfg.log = bcfg.log;
        qrtp::OpticalLink link(ocfg);
        qrtp::BroadcastReceiver receiver(link, bcfg);
        qrtp::TransferMetadata meta;
        auto received = receiver.receive_file(meta, timeout);
        if (!received) {
            std::fprintf(stderr, "listen failed (timeout or hash mismatch)\n");
            return 1;
        }
        const std::filesystem::path out_path = std::filesystem::path(out_dir) / meta.filename;
        std::ofstream f(out_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(received->data()), static_cast<std::streamsize>(received->size()));
        std::printf("OK, wrote %zu bytes to %s\n", received->size(), out_path.string().c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "listen failed: %s\n", e.what());
        return 1;
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Must happen before any window is created, or Windows may bitmap-stretch/blur this process's
    // windows on a scaled display — fatal for QR rendering specifically.
    SetProcessDPIAware();
#endif
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
    if (cmd == "broadcast") return cmd_broadcast_optical(argc, argv);
    if (cmd == "listen") return cmd_listen_optical(argc, argv);
#else
    if (cmd == "send" || cmd == "receive" || cmd == "broadcast" || cmd == "listen") {
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
