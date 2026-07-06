// Native GTK3 GUI for QRTP on Linux — every control built programmatically like gui_win32.cpp,
// no Glade/.ui file, so there's nothing extra to ship beyond GTK3 itself (present in the package
// repositories of virtually every mainstream Linux distribution: gtk3-devel on Fedora,
// libgtk-3-dev on Debian/Ubuntu, gtk3 on Arch). Shares the actual transfer logic with
// gui_win32.cpp via qrtp::RunGuiTransfer (gui_common.hpp) — this file is only the GTK-specific
// widget tree and thread-marshaling. Only compiled when GTK3 is found (see CMakeLists.txt/
// Makefile) and WITH_OPENCV=1, since the entire point of this window is driving a real optical
// (screen+camera) transfer.
#ifdef QRTP_WITH_OPENCV

#include <gtk/gtk.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "qrtp/gui_common.hpp"

namespace {

struct AppWidgets {
    GtkWidget* window = nullptr;
    GtkWidget* radio_send = nullptr;
    GtkWidget* radio_receive = nullptr;
    GtkWidget* radio_bidirectional = nullptr;
    GtkWidget* radio_broadcast = nullptr;
    GtkWidget* path_entry = nullptr;
    GtkWidget* camera_entry = nullptr;
    GtkWidget* res_entry = nullptr;
    GtkWidget* mjpeg_check = nullptr;
    GtkWidget* focus_check = nullptr;
    GtkWidget* wechat_check = nullptr;
    GtkWidget* contrast_check = nullptr;
    GtkWidget* fps_entry = nullptr;           // bidirezionale
    GtkWidget* qrgrid_entry = nullptr;        // bidirezionale
    GtkWidget* chunkmax_entry = nullptr;      // bidirezionale
    GtkWidget* fps_broadcast_entry = nullptr; // unidirezionale — always insensitive, shows "3"
    GtkWidget* chunksize_entry = nullptr;     // unidirezionale
    GtkWidget* loops_entry = nullptr;         // unidirezionale
    GtkWidget* start_button = nullptr;
    GtkWidget* stop_button = nullptr;
    GtkWidget* status_label = nullptr;
    GtkWidget* log_view = nullptr;
    GtkTextBuffer* log_buffer = nullptr;

    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
};

// Single-window app — same pattern as gui_win32.cpp's static AppState* in WndProc, not
// multi-window-safe but matching this whole app's single-window nature.
AppWidgets* g_app = nullptr;

// Matches gui_win32.cpp's kLogMaxChars: past this, a big/slow transfer's log starts visibly
// slowing the whole window down, so the oldest third is dropped rather than trimming line by
// line (cheap, and avoids doing this on every single append once near the cap).
constexpr int kLogMaxChars = 200000;

void AppendLogText(const std::string& text) {
    GtkTextBuffer* buffer = g_app->log_buffer;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    const std::string line = text + "\n";
    gtk_text_buffer_insert(buffer, &end, line.c_str(), -1);

    if (gtk_text_buffer_get_char_count(buffer) > kLogMaxChars) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(buffer, &start);
        gtk_text_buffer_get_iter_at_offset(buffer, &cut, gtk_text_buffer_get_char_count(buffer) / 3);
        gtk_text_iter_forward_to_line_end(&cut);
        gtk_text_buffer_delete(buffer, &start, &cut);
    }

    gtk_text_buffer_get_end_iter(buffer, &end);
    GtkTextMark* mark = gtk_text_buffer_create_mark(buffer, nullptr, &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(g_app->log_view), mark);
    gtk_text_buffer_delete_mark(buffer, mark);
}

// g_idle_add callbacks — the only thread-safe way to touch widgets from the worker thread (GTK's
// equivalent of gui_win32.cpp's PostMessageW/WM_APP_LOG marshaling).
struct LogMsg {
    std::string text;
};
gboolean OnIdleLog(gpointer data) {
    auto* msg = static_cast<LogMsg*>(data);
    AppendLogText(msg->text);
    delete msg;
    return G_SOURCE_REMOVE;
}

struct DoneMsg {
    bool success;
    std::string text;
};
gboolean OnIdleDone(gpointer data) {
    auto* msg = static_cast<DoneMsg*>(data);
    AppendLogText((msg->success ? "OK: " : "ERRORE: ") + msg->text);
    gtk_label_set_text(GTK_LABEL(g_app->status_label), msg->success ? "Completato." : "Terminato con errore.");
    g_app->running.store(false);
    gtk_widget_set_sensitive(g_app->start_button, TRUE);
    gtk_widget_set_sensitive(g_app->stop_button, FALSE);
    delete msg;
    return G_SOURCE_REMOVE;
}

void PostLog(const std::string& msg) { g_idle_add(OnIdleLog, new LogMsg{msg}); }
void PostDone(bool success, const std::string& msg) { g_idle_add(OnIdleDone, new DoneMsg{success, msg}); }

// Runs on the worker thread. Never touches GTK widgets directly — see PostLog/PostDone above.
// The actual transfer logic (which OpticalLink config to build, which protocol class to dispatch
// to) lives in qrtp::RunGuiTransfer (gui_common.hpp), shared with the Win32 Windows frontend.
void RunTransferThread(qrtp::GuiTransferParams params) {
    qrtp::RunGuiTransfer(params, &g_app->cancel, PostLog, PostDone);
}

std::string EntryText(GtkWidget* entry) { return gtk_entry_get_text(GTK_ENTRY(entry)); }

// Grays out whichever mode-specific parameter group doesn't apply to the currently-selected mode
// — same role as gui_win32.cpp's UpdateModeUI. fps_broadcast_entry is never touched here: it's
// permanently insensitive (set once at creation), purely displaying the fixed value.
void UpdateModeUI() {
    const bool broadcast = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app->radio_broadcast));
    gtk_widget_set_sensitive(g_app->fps_entry, !broadcast);
    gtk_widget_set_sensitive(g_app->qrgrid_entry, !broadcast);
    gtk_widget_set_sensitive(g_app->chunkmax_entry, !broadcast);
    gtk_widget_set_sensitive(g_app->chunksize_entry, broadcast);
    gtk_widget_set_sensitive(g_app->loops_entry, broadcast);
}

void OnModeToggled(GtkToggleButton*, gpointer) { UpdateModeUI(); }

void OnBrowseClicked(GtkButton*, gpointer) {
    const bool is_send = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app->radio_send));
    GtkFileChooserAction action = is_send ? GTK_FILE_CHOOSER_ACTION_OPEN : GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        is_send ? "Scegli il file da inviare" : "Scegli la cartella di destinazione", GTK_WINDOW(g_app->window),
        action, "_Annulla", GTK_RESPONSE_CANCEL, is_send ? "_Apri" : "_Seleziona", GTK_RESPONSE_ACCEPT, nullptr);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gtk_entry_set_text(GTK_ENTRY(g_app->path_entry), filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

void OnStartClicked(GtkButton*, gpointer) {
    if (g_app->running.load()) return;

    const std::string path_str = EntryText(g_app->path_entry);
    if (path_str.empty()) {
        GtkWidget* dialog =
            gtk_message_dialog_new(GTK_WINDOW(g_app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                                    "Scegli prima un file (invio) o una cartella (ricezione).");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    qrtp::GuiTransferParams params;
    params.is_send = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app->radio_send));
    params.path = std::filesystem::path(path_str);
    params.broadcast_mode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app->radio_broadcast));
    params.camera_index = std::atoi(EntryText(g_app->camera_entry).c_str());

    // Bidirezionale reads its own FPS field (an AIMD starting point); Unidirezionale is fixed to
    // 3.0 per explicit user request, never read from a field at all — the Unidirezionale group's
    // own FPS field is permanently insensitive, purely displaying "3" (matches gui_win32.cpp).
    double fps = std::atof(EntryText(g_app->fps_entry).c_str());
    if (fps <= 0.0) fps = 3.0;
    params.fps = params.broadcast_mode ? 3.0 : fps;

    // "max"/blank -> auto-detect the camera's highest mode; "0x0" -> leave the camera fully
    // alone; any other "WxH" -> request that specific resolution. See
    // OpticalLinkConfig::kAutoMaxResolution.
    const std::string res = EntryText(g_app->res_entry);
    params.camera_width = qrtp::OpticalLinkConfig::kAutoMaxResolution;
    params.camera_height = qrtp::OpticalLinkConfig::kAutoMaxResolution;
    if (!res.empty() && res != "max" && res != "MAX") {
        const auto x_pos = res.find('x');
        if (x_pos != std::string::npos) {
            const int w = std::atoi(res.substr(0, x_pos).c_str());
            const int h = std::atoi(res.substr(x_pos + 1).c_str());
            if (w >= 0 && h >= 0) {
                params.camera_width = w;
                params.camera_height = h;
            }
        }
    }

    params.qr_grid = std::atoi(EntryText(g_app->qrgrid_entry).c_str());
    if (params.qr_grid <= 0) params.qr_grid = 1;
    params.request_mjpeg = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app->mjpeg_check));
    params.disable_autofocus = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app->focus_check));
    params.use_wechat_qrcode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app->wechat_check));
    params.enhance_camera_contrast = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app->contrast_check));

    if (params.broadcast_mode) {
        // Multi-QR-per-frame batching isn't compatible with the broadcast sender, which never
        // listens for anything — see the same note in main.cpp/gui_win32.cpp.
        params.qr_grid = 1;
        const int chunk = std::atoi(EntryText(g_app->chunksize_entry).c_str());
        params.chunk_value = chunk > 0 ? static_cast<size_t>(chunk) : 180;
        params.loops = std::atoi(EntryText(g_app->loops_entry).c_str());
    } else {
        const int chunk = std::atoi(EntryText(g_app->chunkmax_entry).c_str());
        params.chunk_value = chunk > 0 ? static_cast<size_t>(chunk) : 180;
    }

    gtk_text_buffer_set_text(g_app->log_buffer, "", -1);
    gtk_label_set_text(GTK_LABEL(g_app->status_label),
                        params.is_send ? "Invio in corso..." : "Ricezione in corso...");
    gtk_widget_set_sensitive(g_app->start_button, FALSE);
    gtk_widget_set_sensitive(g_app->stop_button, TRUE);

    g_app->cancel.store(false);
    g_app->running.store(true);
    if (g_app->worker.joinable()) g_app->worker.join(); // previous run already finished
    g_app->worker = std::thread(RunTransferThread, params);
}

void OnStopClicked(GtkButton*, gpointer) {
    g_app->cancel.store(true);
    gtk_label_set_text(GTK_LABEL(g_app->status_label), "Arresto in corso...");
}

gboolean OnWindowDelete(GtkWidget*, GdkEvent*, gpointer) {
    if (g_app->running.load()) {
        g_app->cancel.store(true);
        if (g_app->worker.joinable()) g_app->worker.join();
    }
    return FALSE; // allow the window to actually close
}

GtkWidget* MakeFrame(GtkWidget* parent_box, const char* title) {
    GtkWidget* frame = gtk_frame_new(title);
    gtk_box_pack_start(GTK_BOX(parent_box), frame, FALSE, FALSE, 0);
    return frame;
}

// Tries the icon next to the running executable first (this project's usual "next to the binary"
// convention — see qr_encoder.cpp's LoadLibrary/wechat model dir comments — so a built qrtp_gui
// picks it up regardless of the current working directory), then falls back to a path relative to
// the repo root (works when running straight from a `make`/`cmake --build` output during
// development). Missing entirely isn't fatal — just no window icon.
void SetWindowIcon(GtkWidget* window) {
    std::string exe_relative;
    char buf[4096];
    const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        exe_relative = (std::filesystem::path(buf).parent_path() / "icon" / "qrtp.png").string();
    }
    GError* error = nullptr;
    if (exe_relative.empty() || !gtk_window_set_icon_from_file(GTK_WINDOW(window), exe_relative.c_str(), &error)) {
        if (error) g_error_free(error);
        error = nullptr;
        gtk_window_set_icon_from_file(GTK_WINDOW(window), "linux/icon/qrtp.png", &error);
        if (error) g_error_free(error);
    }
}

} // namespace

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);

    auto app = std::make_unique<AppWidgets>();
    g_app = app.get();

    g_app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_app->window), "QRTP — Trasferimento file via QR");
    gtk_window_set_default_size(GTK_WINDOW(g_app->window), 560, 640);
    gtk_container_set_border_width(GTK_CONTAINER(g_app->window), 10);
    SetWindowIcon(g_app->window);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(g_app->window), root);

    // --- Ruolo e modalità ---
    GtkWidget* role_frame = MakeFrame(root, "Ruolo e modalità");
    GtkWidget* role_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(role_box), 8);
    gtk_container_add(GTK_CONTAINER(role_frame), role_box);

    GtkWidget* role_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    g_app->radio_send = gtk_radio_button_new_with_label(nullptr, "Invia file");
    g_app->radio_receive =
        gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(g_app->radio_send), "Ricevi file");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_app->radio_send), TRUE);
    gtk_box_pack_start(GTK_BOX(role_row), g_app->radio_send, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(role_row), g_app->radio_receive, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(role_box), role_row, FALSE, FALSE, 0);

    // Bidirezionale (default): the existing ARQ/AIMD-driven Sender/Receiver (protocol.hpp).
    // Unidirezionale: BroadcastSender/BroadcastReceiver (broadcast.hpp) — no return channel at
    // all, reliability from looping the whole file instead of retransmission.
    GtkWidget* mode_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    g_app->radio_bidirectional = gtk_radio_button_new_with_label(nullptr, "Bidirezionale (TCP-like)");
    g_app->radio_broadcast = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(g_app->radio_bidirectional), "Unidirezionale (broadcast)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_app->radio_bidirectional), TRUE);
    gtk_box_pack_start(GTK_BOX(mode_row), g_app->radio_bidirectional, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mode_row), g_app->radio_broadcast, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(role_box), mode_row, FALSE, FALSE, 0);

    GtkWidget* hint = gtk_label_new("In unidirezionale il ricevitore non risponde mai; FPS è fisso a 3.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_box_pack_start(GTK_BOX(role_box), hint, FALSE, FALSE, 0);

    g_signal_connect(g_app->radio_bidirectional, "toggled", G_CALLBACK(OnModeToggled), nullptr);
    g_signal_connect(g_app->radio_broadcast, "toggled", G_CALLBACK(OnModeToggled), nullptr);

    // --- File ---
    GtkWidget* file_frame = MakeFrame(root, "File");
    GtkWidget* file_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(file_row), 8);
    gtk_container_add(GTK_CONTAINER(file_frame), file_row);
    g_app->path_entry = gtk_entry_new();
    gtk_widget_set_hexpand(g_app->path_entry, TRUE);
    GtkWidget* browse_button = gtk_button_new_with_label("Sfoglia...");
    gtk_box_pack_start(GTK_BOX(file_row), g_app->path_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(file_row), browse_button, FALSE, FALSE, 0);
    g_signal_connect(browse_button, "clicked", G_CALLBACK(OnBrowseClicked), nullptr);

    // --- Fotocamera (comune a entrambe le modalità) ---
    GtkWidget* cam_frame = MakeFrame(root, "Fotocamera (comune a entrambe le modalità)");
    GtkWidget* cam_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(cam_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(cam_grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(cam_grid), 8);
    gtk_container_add(GTK_CONTAINER(cam_frame), cam_grid);

    g_app->camera_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_app->camera_entry), "0");
    gtk_grid_attach(GTK_GRID(cam_grid), gtk_label_new("Indice camera:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cam_grid), g_app->camera_entry, 1, 0, 1, 1);

    // 640x480 (matches OpticalLinkConfig::camera_width/height, the library/CLI default too).
    // Fully editable: type "max" to opt back into auto-detection, "0x0" to skip any request and
    // leave the camera fully alone, or any other specific WxH.
    g_app->res_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_app->res_entry), "640x480");
    gtk_grid_attach(GTK_GRID(cam_grid), gtk_label_new("Ris. (LxA):"), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cam_grid), g_app->res_entry, 3, 0, 1, 1);

    g_app->mjpeg_check = gtk_check_button_new_with_label("Richiedi MJPEG");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_app->mjpeg_check), TRUE);
    gtk_grid_attach(GTK_GRID(cam_grid), g_app->mjpeg_check, 0, 1, 2, 1);

    g_app->focus_check = gtk_check_button_new_with_label("Blocca autofocus");
    gtk_grid_attach(GTK_GRID(cam_grid), g_app->focus_check, 2, 1, 2, 1);

    g_app->wechat_check = gtk_check_button_new_with_label("Usa WeChatQRCode (CNN)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_app->wechat_check), TRUE);
    gtk_grid_attach(GTK_GRID(cam_grid), g_app->wechat_check, 0, 2, 2, 1);

    g_app->contrast_check = gtk_check_button_new_with_label("Migliora contrasto fotocamera");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_app->contrast_check), TRUE);
    gtk_grid_attach(GTK_GRID(cam_grid), g_app->contrast_check, 2, 2, 2, 1);

    // --- Parametri specifici di modalità, affiancati; UpdateModeUI() (sotto) disabilita quello
    // non pertinente alla modalità selezionata ---
    GtkWidget* modes_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(root), modes_row, FALSE, FALSE, 0);

    GtkWidget* bidi_frame = gtk_frame_new("Bidirezionale (TCP-like)");
    gtk_box_pack_start(GTK_BOX(modes_row), bidi_frame, TRUE, TRUE, 0);
    GtkWidget* bidi_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(bidi_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(bidi_grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(bidi_grid), 8);
    gtk_container_add(GTK_CONTAINER(bidi_frame), bidi_grid);

    // GUI-only default of 30 — deliberately higher than the library/CLI default
    // (OpticalLinkConfig::fps/AdaptiveConfig::fps_initial, 10.0). Just the AIMD's starting
    // point, not a fixed value — it ramps from here.
    g_app->fps_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_app->fps_entry), "30");
    gtk_grid_attach(GTK_GRID(bidi_grid), gtk_label_new("FPS iniziale:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(bidi_grid), g_app->fps_entry, 1, 0, 1, 1);

    // Classic default "1" — the original single-QR-at-a-time behavior. >1 is the experimental
    // multi-QR-per-frame mode (OpticalLinkConfig::qr_codes_per_frame); not compatible with
    // Unidirezionale, forced to 1 there regardless of this field (see OnStartClicked).
    g_app->qrgrid_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_app->qrgrid_entry), "1");
    gtk_grid_attach(GTK_GRID(bidi_grid), gtk_label_new("QR per frame:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(bidi_grid), g_app->qrgrid_entry, 1, 1, 1, 1);

    g_app->chunkmax_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_app->chunkmax_entry), "180");
    gtk_grid_attach(GTK_GRID(bidi_grid), gtk_label_new("Chunk max (byte):"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(bidi_grid), g_app->chunkmax_entry, 1, 2, 1, 1);

    GtkWidget* broadcast_frame = gtk_frame_new("Unidirezionale (broadcast)");
    gtk_box_pack_start(GTK_BOX(modes_row), broadcast_frame, TRUE, TRUE, 0);
    GtkWidget* broadcast_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(broadcast_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(broadcast_grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(broadcast_grid), 8);
    gtk_container_add(GTK_CONTAINER(broadcast_frame), broadcast_grid);

    // Always insensitive — this mode has no AIMD to adapt fps from at all, so per explicit user
    // request it's hard-coded in OnStartClicked, not read from here. Shown, not editable, purely
    // so the fixed value is visible rather than silently implicit.
    g_app->fps_broadcast_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_app->fps_broadcast_entry), "3");
    gtk_widget_set_sensitive(g_app->fps_broadcast_entry, FALSE);
    gtk_grid_attach(GTK_GRID(broadcast_grid), gtk_label_new("FPS (fisso):"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(broadcast_grid), g_app->fps_broadcast_entry, 1, 0, 1, 1);

    g_app->chunksize_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_app->chunksize_entry), "180");
    gtk_grid_attach(GTK_GRID(broadcast_grid), gtk_label_new("Chunk (byte):"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(broadcast_grid), g_app->chunksize_entry, 1, 1, 1, 1);

    // BroadcastConfig::loops — 0 (default) loops the whole file indefinitely until "Ferma" is
    // pressed, the natural default for a continuous broadcast with no return channel to ever
    // signal "the receiver has everything, stop."
    g_app->loops_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_app->loops_entry), "0");
    gtk_grid_attach(GTK_GRID(broadcast_grid), gtk_label_new("Loop (0=infinito):"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(broadcast_grid), g_app->loops_entry, 1, 2, 1, 1);

    // --- Avvia/Ferma/stato ---
    GtkWidget* actions_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(root), actions_row, FALSE, FALSE, 0);
    g_app->start_button = gtk_button_new_with_label("Avvia");
    g_app->stop_button = gtk_button_new_with_label("Ferma");
    gtk_widget_set_sensitive(g_app->stop_button, FALSE);
    g_app->status_label = gtk_label_new("Pronto.");
    gtk_box_pack_start(GTK_BOX(actions_row), g_app->start_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions_row), g_app->stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions_row), g_app->status_label, FALSE, FALSE, 8);
    g_signal_connect(g_app->start_button, "clicked", G_CALLBACK(OnStartClicked), nullptr);
    g_signal_connect(g_app->stop_button, "clicked", G_CALLBACK(OnStopClicked), nullptr);

    // --- Log ---
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    g_app->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_app->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(g_app->log_view), TRUE);
    g_app->log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_app->log_view));
    gtk_container_add(GTK_CONTAINER(scroll), g_app->log_view);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    g_signal_connect(g_app->window, "delete-event", G_CALLBACK(OnWindowDelete), nullptr);
    g_signal_connect(g_app->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    UpdateModeUI();
    gtk_widget_show_all(g_app->window);
    gtk_main();

    if (g_app->worker.joinable()) g_app->worker.join();
    return 0;
}

#endif // QRTP_WITH_OPENCV
