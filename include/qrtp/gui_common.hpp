#pragma once
#ifdef QRTP_WITH_OPENCV

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

#include "qrtp/optical_link.hpp"

namespace qrtp {

// Parameters every GUI frontend (gui_win32.cpp on Windows, gui_gtk.cpp on Linux) needs to start a
// transfer — toolkit-agnostic, built from whatever each one's own controls hold.
struct GuiTransferParams {
    bool is_send = true;
    std::filesystem::path path; // file to send, or destination directory to receive into
    int camera_index = 0;
    double fps = 30.0;
    int camera_width = OpticalLinkConfig::kAutoMaxResolution;
    int camera_height = OpticalLinkConfig::kAutoMaxResolution;
    int qr_grid = 1;
    bool request_mjpeg = true;
    bool disable_autofocus = false;
    bool use_wechat_qrcode = true;
    bool enhance_camera_contrast = true;
    // AdaptiveConfig::chunk_size_max (bidirezionale) or BroadcastConfig::chunk_size
    // (unidirezionale) — same "bytes of file payload per QR" meaning either way.
    size_t chunk_value = 180;
    bool broadcast_mode = false;
    int loops = 0; // unidirezionale only (BroadcastConfig::loops)
};

// Builds the right OpticalLink and dispatches to Sender/Receiver (bidirectional,
// protocol.hpp) or BroadcastSender/BroadcastReceiver (params.broadcast_mode, broadcast.hpp),
// reporting progress/completion through the two callbacks instead of touching any
// toolkit-specific widget directly — callers marshal these onto their own UI thread however
// their toolkit requires (PostMessageW on Win32, g_idle_add on GTK). Runs synchronously on
// whatever thread calls it; every GUI frontend calls this from its own background worker
// thread, never its UI thread.
void RunGuiTransfer(const GuiTransferParams& params, std::atomic<bool>* cancel,
                     const std::function<void(const std::string&)>& log,
                     const std::function<void(bool, const std::string&)>& done);

} // namespace qrtp

#endif // QRTP_WITH_OPENCV
