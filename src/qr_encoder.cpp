#include "qrtp/qr_encoder.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <fstream>
#include <stdexcept>

namespace qrtp {
namespace {

// The runtime shared library for libqrencode is often present on a machine without its -devel
// package / import headers (no qrencode.h, no dev symlink/import lib). Rather than requiring a
// system package install or hand-rolling a from-scratch QR generator (risky to get byte-perfect
// against the ISO/IEC 18004 tables without a real decoder on hand to verify against), this loads
// whatever's already installed at runtime (dlopen on Linux/macOS, LoadLibrary on Windows) and
// declares just the small, long-stable subset of its public C ABI actually needed here.
extern "C" struct QRcodeAbi {
    int version;
    int width;
    unsigned char* data;
};

using QRcodeEncodeDataFn = QRcodeAbi* (*)(int size, const unsigned char* data, int version, int level);
using QRcodeFreeFn = void (*)(QRcodeAbi*);

#ifdef _WIN32

struct LibQrEncode {
    HMODULE handle = nullptr;
    QRcodeEncodeDataFn encode_data = nullptr;
    QRcodeFreeFn free_fn = nullptr;

    LibQrEncode() {
        // Name varies by how the DLL was built; try the common ones in order.
        for (const char* name : {"qrencode.dll", "libqrencode.dll", "libqrencode-4.dll"}) {
            handle = LoadLibraryA(name);
            if (handle) break;
        }
        if (!handle) {
            throw std::runtime_error(
                "qr_encoder: cannot load qrencode.dll (place it next to qrtp.exe or add its folder to PATH)");
        }
        encode_data = reinterpret_cast<QRcodeEncodeDataFn>(GetProcAddress(handle, "QRcode_encodeData"));
        free_fn = reinterpret_cast<QRcodeFreeFn>(GetProcAddress(handle, "QRcode_free"));
        if (!encode_data || !free_fn) {
            throw std::runtime_error("qr_encoder: qrencode.dll is missing expected exports");
        }
    }

    ~LibQrEncode() {
        if (handle) FreeLibrary(handle);
    }
};

#else

struct LibQrEncode {
    void* handle = nullptr;
    QRcodeEncodeDataFn encode_data = nullptr;
    QRcodeFreeFn free_fn = nullptr;

    LibQrEncode() {
        handle = dlopen("libqrencode.so.4", RTLD_NOW | RTLD_LOCAL);
        if (!handle) handle = dlopen("libqrencode.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            throw std::runtime_error(std::string("qr_encoder: cannot load libqrencode: ") + dlerror());
        }
        encode_data = reinterpret_cast<QRcodeEncodeDataFn>(dlsym(handle, "QRcode_encodeData"));
        free_fn = reinterpret_cast<QRcodeFreeFn>(dlsym(handle, "QRcode_free"));
        if (!encode_data || !free_fn) {
            throw std::runtime_error("qr_encoder: libqrencode is missing expected symbols");
        }
    }

    ~LibQrEncode() {
        if (handle) dlclose(handle);
    }
};

#endif

LibQrEncode& lib() {
    static LibQrEncode instance;
    return instance;
}

} // namespace

QrMatrix qr_encode_bytes(const std::vector<uint8_t>& data, QrEcLevel level) {
    auto& l = lib();
    // version=0 tells libqrencode to pick the smallest version that fits the payload.
    QRcodeAbi* qr = l.encode_data(static_cast<int>(data.size()), data.data(), 0, static_cast<int>(level));
    if (!qr) throw std::runtime_error("qr_encoder: encoding failed (payload likely too large for a QR code)");

    QrMatrix out;
    out.version = qr->version;
    out.width = qr->width;
    out.modules.resize(static_cast<size_t>(qr->width) * static_cast<size_t>(qr->width));
    for (size_t i = 0; i < out.modules.size(); ++i) {
        out.modules[i] = qr->data[i] & 0x01u; // bit 0 = dark module; other bits are library-internal
    }
    l.free_fn(qr);

    // Sanity check against the ISO/IEC 18004 version<->size formula. If this ever fails it means
    // the dlopen ABI assumption above no longer matches the installed library.
    if (out.width != 4 * out.version + 17) {
        throw std::runtime_error("qr_encoder: unexpected width/version relationship — ABI assumption may be stale");
    }
    return out;
}

void write_pbm(const QrMatrix& qr, const std::string& path, int scale, int quiet_zone) {
    const int side = qr.width + 2 * quiet_zone;
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("qr_encoder: cannot open output file: " + path);

    f << "P1\n" << (side * scale) << " " << (side * scale) << "\n";
    for (int y = 0; y < side * scale; ++y) {
        const int my = y / scale - quiet_zone;
        for (int x = 0; x < side * scale; ++x) {
            const int mx = x / scale - quiet_zone;
            bool dark = false;
            if (mx >= 0 && mx < qr.width && my >= 0 && my < qr.width) {
                dark = qr.modules[static_cast<size_t>(my) * qr.width + mx] != 0;
            }
            f << (dark ? '1' : '0') << (x + 1 < side * scale ? ' ' : '\n');
        }
    }
}

} // namespace qrtp
