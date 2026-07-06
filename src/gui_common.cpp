#include "qrtp/gui_common.hpp"
#ifdef QRTP_WITH_OPENCV

#include <fstream>

#include "qrtp/broadcast.hpp"
#include "qrtp/protocol.hpp"

namespace qrtp {

void RunGuiTransfer(const GuiTransferParams& params, std::atomic<bool>* cancel,
                     const std::function<void(const std::string&)>& log,
                     const std::function<void(bool, const std::string&)>& done) {
    try {
        OpticalLinkConfig ocfg;
        ocfg.camera_index = params.camera_index;
        ocfg.fps = params.fps;
        ocfg.camera_width = params.camera_width;
        ocfg.camera_height = params.camera_height;
        ocfg.qr_codes_per_frame = params.qr_grid;
        ocfg.request_mjpeg = params.request_mjpeg;
        ocfg.disable_autofocus = params.disable_autofocus;
        ocfg.use_wechat_qrcode = params.use_wechat_qrcode;
        ocfg.enhance_camera_contrast = params.enhance_camera_contrast;
        ocfg.log = log;
        OpticalLink link(ocfg);

        // Unidirezionale (broadcast/listen): coexists with the bidirectional path below as a
        // completely separate protocol — no ARQ/handshake, no AIMD, fixed fps/chunk size.
        if (params.broadcast_mode) {
            BroadcastConfig bcfg;
            bcfg.fps = params.fps;
            bcfg.chunk_size = params.chunk_value;
            bcfg.loops = params.loops;
            bcfg.cancel = cancel;
            bcfg.log = log;

            if (params.is_send) {
                BroadcastSender sender(link, bcfg);
                auto result = sender.send_file(params.path);
                done(result.success, result.message);
            } else {
                BroadcastReceiver receiver(link, bcfg);
                TransferMetadata meta;
                auto received = receiver.receive_file(meta, std::chrono::minutes(30));
                if (!received) {
                    done(false,
                         (cancel && cancel->load()) ? "cancelled" : "listen failed (timeout or hash mismatch)");
                    return;
                }
                auto out_path = params.path / meta.filename;
                std::ofstream f(out_path, std::ios::binary);
                f.write(reinterpret_cast<const char*>(received->data()),
                        static_cast<std::streamsize>(received->size()));
                done(true, "wrote " + std::to_string(received->size()) + " bytes to " + out_path.string());
            }
            return;
        }

        ProtocolConfig cfg;
        cfg.cancel = cancel;
        cfg.log = log;
        cfg.adaptive.chunk_size_max = params.chunk_value;

        if (params.is_send) {
            Sender sender(link, cfg);
            auto result = sender.send_file(params.path);
            done(result.success, result.message);
        } else {
            Receiver receiver(link, cfg);
            TransferMetadata meta;
            auto received = receiver.receive_file(meta, std::chrono::minutes(30));
            if (!received) {
                done(false, (cancel && cancel->load()) ? "cancelled" : "receive failed (timeout or hash mismatch)");
                return;
            }
            auto out_path = params.path / meta.filename;
            std::ofstream f(out_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(received->data()), static_cast<std::streamsize>(received->size()));
            done(true, "wrote " + std::to_string(received->size()) + " bytes to " + out_path.string());
        }
    } catch (const std::exception& e) {
        done(false, e.what());
    }
}

} // namespace qrtp

#endif // QRTP_WITH_OPENCV
