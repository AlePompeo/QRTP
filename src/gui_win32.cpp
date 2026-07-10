// Native Win32 GUI for QRTP. Deliberately plain Win32 API (no extra GUI framework/runtime): every
// control is created programmatically with CreateWindowExW, so the built .exe has zero external
// dependencies beyond OpenCV (for the actual camera/QR window) and the C runtime — as portable as
// a single-file .exe can be. Only compiled on Windows, and only when built WITH_OPENCV=1, since the
// entire point of this window is driving a real optical (screen+camera) transfer.
#ifdef _WIN32
#ifdef QRTP_WITH_OPENCV

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <commdlg.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "qrtp/gui_common.hpp"
#include "qrtp/optical_link.hpp"

#include "resource.h" // IDI_APP_ICON — shared with windows/qrtp.rc

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

// Control IDs
constexpr int IDC_RADIO_SEND = 101;
constexpr int IDC_RADIO_RECEIVE = 102;
constexpr int IDC_PATH_EDIT = 103;
constexpr int IDC_BROWSE_BTN = 104;
constexpr int IDC_CAMERA_EDIT = 105;
constexpr int IDC_FPS_EDIT = 106;
constexpr int IDC_START_BTN = 107;
constexpr int IDC_STOP_BTN = 108;
constexpr int IDC_LOG_EDIT = 109;
constexpr int IDC_STATUS_STATIC = 110;
constexpr int IDC_RES_EDIT = 111;
constexpr int IDC_QRGRID_EDIT = 112;
constexpr int IDC_MJPEG_CHECK = 113;
constexpr int IDC_FOCUS_CHECK = 114;
constexpr int IDC_WECHAT_CHECK = 115;
constexpr int IDC_CHUNKMAX_EDIT = 116;
constexpr int IDC_CONTRAST_CHECK = 117;
constexpr int IDC_LOOPS_EDIT = 119;
constexpr int IDC_RADIO_BIDIRECTIONAL = 120;
constexpr int IDC_RADIO_BROADCAST = 121;
constexpr int IDC_FPS_BROADCAST_EDIT = 122; // combobox: 3/5/7, enabled only in Unidirezionale mode
constexpr int IDC_CHUNKSIZE_EDIT = 123;     // unidirezionale's own chunk-size field

// "Strumento" selector (Trasferimento file / Messaggi cifrati) and the "Messaggi cifrati" tool's
// own controls -- see AppState::transfer_only/messenger_only and UpdateToolUI below.
constexpr int IDC_RADIO_TOOL_TRANSFER = 124;
constexpr int IDC_RADIO_TOOL_MESSENGER = 125;
constexpr int IDC_MSG_RADIO_WRITE = 126;
constexpr int IDC_MSG_RADIO_READ = 127;
constexpr int IDC_MSG_PASSPHRASE_EDIT = 128;
constexpr int IDC_MSG_TEXT_EDIT = 129;
constexpr int IDC_MSG_CHARCOUNT_STATIC = 130;
constexpr int IDC_MSG_RESULT_EDIT = 131;

// Custom messages posted from the worker thread to the UI thread. Never touch a control directly
// from the worker thread — Win32 windows are single-threaded-affine, all updates must happen here.
constexpr UINT WM_APP_LOG = WM_APP + 1;   // lParam = new std::wstring*, UI thread deletes it
constexpr UINT WM_APP_DONE = WM_APP + 2;  // wParam = success (0/1), lParam = new std::wstring*
// Messenger read mode's own completion message: on success, lParam is the decrypted plaintext
// (routed into IDC_MSG_RESULT_EDIT) rather than just a log line -- everything else (wParam
// success flag, ownership/deletion) matches WM_APP_DONE exactly.
constexpr UINT WM_APP_MSG_READ_DONE = WM_APP + 3;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

struct AppState {
    HWND hwnd = nullptr;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    // Populated at WM_CREATE by the mkT/mkM wrappers around mk() (see WM_CREATE) -- every control
    // belonging to the "Trasferimento file" tool vs. the "Messaggi cifrati" tool, respectively, so
    // UpdateToolUI can show/hide each set as a block. The "Fotocamera" group and the shared
    // Avvia/Ferma/status/log controls at the bottom use plain mk() and are always visible.
    std::vector<HWND> transfer_only;
    std::vector<HWND> messenger_only;
};

// A plain Win32 EDIT control silently stops accepting new text past a default ~32K-character
// limit unless EM_LIMITTEXT explicitly raises it (done once at creation, see WM_CREATE) — without
// that, a long transfer's log looked like it had simply frozen partway through, when actually the
// control was just refusing anything more. Also cap how much text we let it hold: an unbounded
// multi-line EDIT gets visibly slower to update the longer it grows, which on a big/slow transfer
// with many log lines can make the whole window feel like it's hanging even with the limit fixed.
constexpr int kLogMaxChars = 200000;

void AppendLog(HWND log_edit, const std::wstring& line) {
    int len = GetWindowTextLengthW(log_edit);
    if (len > kLogMaxChars) {
        // Drop the oldest third rather than trimming line-by-line — cheap, and avoids doing this
        // on every single append once near the cap.
        std::vector<wchar_t> buf(static_cast<size_t>(len) + 1);
        GetWindowTextW(log_edit, buf.data(), len + 1);
        std::wstring text(buf.data());
        size_t cut = text.find(L'\n', text.size() / 3);
        if (cut != std::wstring::npos) text.erase(0, cut + 1);
        SetWindowTextW(log_edit, text.c_str());
        len = GetWindowTextLengthW(log_edit);
    }
    SendMessageW(log_edit, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    std::wstring withNewline = line + L"\r\n";
    SendMessageW(log_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(withNewline.c_str()));
}

std::wstring BrowseForFile(HWND owner) {
    wchar_t buffer[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Tutti i file\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) return buffer;
    return L"";
}

std::wstring BrowseForFolder(HWND owner) {
    wchar_t path[MAX_PATH] = L"";
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Scegli la cartella di destinazione";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";
    if (!SHGetPathFromIDListW(pidl, path)) path[0] = L'\0';
    CoTaskMemFree(pidl);
    return path;
}

// Runs on the worker thread. Never touches Win32 controls directly — marshals progress/
// completion back to the UI thread via PostMessageW. The actual transfer logic (which
// OpticalLink config to build, which protocol class to dispatch to) lives in
// qrtp::RunGuiTransfer (gui_common.hpp), shared with the GTK Linux frontend (gui_gtk.cpp).
void RunTransfer(AppState* app, bool is_send, std::wstring path_w, int camera_index, double fps, int camera_width,
                  int camera_height, int qr_grid, bool request_mjpeg, bool disable_autofocus,
                  bool use_wechat_qrcode, bool enhance_contrast, size_t chunk_max, bool broadcast_mode, int loops) {
    HWND hwnd = app->hwnd;
    auto post_log = [hwnd](const std::string& msg) {
        auto* copy = new std::wstring(Utf8ToWide(msg));
        PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy));
    };
    auto post_done = [hwnd](bool success, const std::string& msg) {
        auto* copy = new std::wstring(Utf8ToWide(msg));
        PostMessageW(hwnd, WM_APP_DONE, success ? 1 : 0, reinterpret_cast<LPARAM>(copy));
    };

    qrtp::GuiTransferParams params;
    params.is_send = is_send;
    params.path = std::filesystem::path(path_w);
    params.camera_index = camera_index;
    params.fps = fps;
    params.camera_width = camera_width;
    params.camera_height = camera_height;
    params.qr_grid = qr_grid;
    params.request_mjpeg = request_mjpeg;
    params.disable_autofocus = disable_autofocus;
    params.use_wechat_qrcode = use_wechat_qrcode;
    params.enhance_camera_contrast = enhance_contrast;
    params.chunk_value = chunk_max;
    params.broadcast_mode = broadcast_mode;
    params.loops = loops;

    qrtp::RunGuiTransfer(params, &app->cancel, post_log, post_done);
}

// Same role as RunTransfer above, for the "Messaggi cifrati" tool (qrtp::RunGuiMessenger,
// gui_common.hpp). Read mode reports the decrypted plaintext through WM_APP_MSG_READ_DONE instead
// of WM_APP_DONE, so WndProc knows to route it into IDC_MSG_RESULT_EDIT rather than just the log.
void RunMessenger(AppState* app, bool is_write, std::wstring passphrase_w, std::wstring message_w, int camera_index,
                   int camera_width, int camera_height, bool request_mjpeg, bool disable_autofocus,
                   bool use_wechat_qrcode, bool enhance_contrast) {
    HWND hwnd = app->hwnd;
    auto post_log = [hwnd](const std::string& msg) {
        auto* copy = new std::wstring(Utf8ToWide(msg));
        PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy));
    };
    auto post_done = [hwnd](bool success, const std::string& msg) {
        auto* copy = new std::wstring(Utf8ToWide(msg));
        PostMessageW(hwnd, WM_APP_DONE, success ? 1 : 0, reinterpret_cast<LPARAM>(copy));
    };
    auto post_read_done = [hwnd](bool success, const std::string& msg) {
        auto* copy = new std::wstring(Utf8ToWide(msg));
        PostMessageW(hwnd, WM_APP_MSG_READ_DONE, success ? 1 : 0, reinterpret_cast<LPARAM>(copy));
    };

    qrtp::GuiMessengerParams params;
    params.is_write = is_write;
    params.passphrase = WideToUtf8(passphrase_w);
    params.message = WideToUtf8(message_w);
    params.camera_index = camera_index;
    params.camera_width = camera_width;
    params.camera_height = camera_height;
    params.request_mjpeg = request_mjpeg;
    params.disable_autofocus = disable_autofocus;
    params.use_wechat_qrcode = use_wechat_qrcode;
    params.enhance_camera_contrast = enhance_contrast;

    // Not a ternary here: post_done/post_read_done are distinct lambda closure types, which the
    // ternary operator can't unify directly (unlike gui_gtk.cpp's PostDone/PostMessengerReadDone,
    // which are plain functions and do unify) -- both branches implicitly convert to
    // std::function<void(bool,const std::string&)> when passed as the argument instead.
    if (is_write) {
        qrtp::RunGuiMessenger(params, &app->cancel, post_log, post_done);
    } else {
        qrtp::RunGuiMessenger(params, &app->cancel, post_log, post_read_done);
    }
}

// Grays out whichever mode-specific parameter group doesn't apply to the currently-selected mode
// (see the "Bidirezionale"/"Unidirezionale" groups in WM_CREATE) — called once at window creation
// and again every time the mode radio buttons change.
void UpdateModeUI(HWND hwnd) {
    const bool broadcast = IsDlgButtonChecked(hwnd, IDC_RADIO_BROADCAST) == BST_CHECKED;
    EnableWindow(GetDlgItem(hwnd, IDC_FPS_EDIT), !broadcast);
    EnableWindow(GetDlgItem(hwnd, IDC_QRGRID_EDIT), !broadcast);
    EnableWindow(GetDlgItem(hwnd, IDC_CHUNKMAX_EDIT), !broadcast);
    EnableWindow(GetDlgItem(hwnd, IDC_FPS_BROADCAST_EDIT), broadcast);
    EnableWindow(GetDlgItem(hwnd, IDC_CHUNKSIZE_EDIT), broadcast);
    EnableWindow(GetDlgItem(hwnd, IDC_LOOPS_EDIT), broadcast);
}

constexpr int kMsgMaxChars = 1000; // matches web/messenger.js's textarea maxlength="1000"

// Shows/hides the "Trasferimento file" vs. "Messaggi cifrati" control sets as a block (see
// AppState::transfer_only/messenger_only, populated at WM_CREATE) — Win32 has no lightweight tab
// control equivalent to GTK's GtkNotebook, so both tools' controls occupy the same screen
// rectangle and only one set is ever visible at a time. The "Fotocamera" group and the shared
// Avvia/Ferma/status/log controls stay visible regardless (plain mk(), never added to either
// vector).
void UpdateToolUI(AppState* app) {
    const bool messenger = IsDlgButtonChecked(app->hwnd, IDC_RADIO_TOOL_MESSENGER) == BST_CHECKED;
    for (HWND h : app->transfer_only) ShowWindow(h, messenger ? SW_HIDE : SW_SHOW);
    for (HWND h : app->messenger_only) ShowWindow(h, messenger ? SW_SHOW : SW_HIDE);
}

// Grays out the compose textbox when "Leggi messaggio" is selected (mirrors UpdateModeUI's role
// for the transfer tool) and keeps the live character counter current.
void UpdateMessengerModeUI(HWND hwnd) {
    const bool is_write = IsDlgButtonChecked(hwnd, IDC_MSG_RADIO_WRITE) == BST_CHECKED;
    EnableWindow(GetDlgItem(hwnd, IDC_MSG_TEXT_EDIT), is_write);
}

void UpdateMessengerCharCount(HWND hwnd) {
    int len = GetWindowTextLengthW(GetDlgItem(hwnd, IDC_MSG_TEXT_EDIT));
    std::wstring text = std::to_wstring(len) + L"/" + std::to_wstring(kMsgMaxChars);
    SetWindowTextW(GetDlgItem(hwnd, IDC_MSG_CHARCOUNT_STATIC), text.c_str());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static AppState* app = nullptr;

    switch (msg) {
        case WM_CREATE: {
            app = reinterpret_cast<AppState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            app->hwnd = hwnd;

            HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            auto mk = [&](LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int w, int h, int id) {
                HWND c = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                          GetModuleHandleW(nullptr), nullptr);
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                return c;
            };
            // Same as mk(), but also remembers the control as belonging to one tool or the other
            // (see AppState::transfer_only/messenger_only and UpdateToolUI) so it can be shown/
            // hidden as a block when the "Strumento" selector below changes.
            auto mkT = [&](LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int w, int h, int id) {
                HWND c = mk(cls, text, style, x, y, w, h, id);
                app->transfer_only.push_back(c);
                return c;
            };
            auto mkM = [&](LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int w, int h, int id) {
                HWND c = mk(cls, text, style, x, y, w, h, id);
                app->messenger_only.push_back(c);
                return c;
            };

            // --- Group 0: selettore strumento (sempre visibile) ---
            mk(L"BUTTON", L"Strumento", BS_GROUPBOX, 10, 5, 495, 38, 0);
            mk(L"BUTTON", L"Trasferimento file", BS_AUTORADIOBUTTON | WS_GROUP, 25, 22, 170, 18,
               IDC_RADIO_TOOL_TRANSFER);
            mk(L"BUTTON", L"Messaggi cifrati", BS_AUTORADIOBUTTON, 215, 22, 170, 18, IDC_RADIO_TOOL_MESSENGER);
            CheckDlgButton(hwnd, IDC_RADIO_TOOL_TRANSFER, BST_CHECKED);

            // --- Group 1: ruolo (invia/ricevi) + modalità (bidirezionale/unidirezionale) ---
            // Two independent choices, each its own radio-button group (WS_GROUP starts a new
            // group; every following button until the next WS_GROUP joins it) — grouped together
            // visually since both must be picked before starting a transfer.
            mkT(L"BUTTON", L"Ruolo e modalità", BS_GROUPBOX, 10, 45, 495, 80, 0);
            mkT(L"BUTTON", L"Invia file", BS_AUTORADIOBUTTON | WS_GROUP, 25, 64, 110, 18, IDC_RADIO_SEND);
            mkT(L"BUTTON", L"Ricevi file", BS_AUTORADIOBUTTON, 145, 64, 110, 18, IDC_RADIO_RECEIVE);
            CheckDlgButton(hwnd, IDC_RADIO_SEND, BST_CHECKED);
            // Bidirezionale (default): the existing ARQ/AIMD-driven Sender/Receiver (protocol.hpp).
            // Unidirezionale: BroadcastSender/BroadcastReceiver (broadcast.hpp) — no return channel
            // at all, reliability from looping the whole file instead of retransmission. See
            // RunTransfer and UpdateModeUI (below) for how each field's availability follows this.
            mkT(L"BUTTON", L"Bidirezionale (TCP-like)", BS_AUTORADIOBUTTON | WS_GROUP, 25, 86, 190, 18, IDC_RADIO_BIDIRECTIONAL);
            mkT(L"BUTTON", L"Unidirezionale (broadcast)", BS_AUTORADIOBUTTON, 225, 86, 220, 18, IDC_RADIO_BROADCAST);
            CheckDlgButton(hwnd, IDC_RADIO_BIDIRECTIONAL, BST_CHECKED);
            mkT(L"STATIC", L"In unidirezionale il ricevitore non risponde mai; FPS è fisso a 3.", 0, 25, 106, 460, 14, 0);

            // --- Group 2: file/cartella (comune a entrambe le modalità) ---
            mkT(L"BUTTON", L"File", BS_GROUPBOX, 10, 132, 495, 45, 0);
            mkT(L"STATIC", L"File / cartella:", 0, 20, 150, 95, 18, 0);
            mkT(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 120, 148, 280, 22, IDC_PATH_EDIT);
            mkT(L"BUTTON", L"Sfoglia...", BS_PUSHBUTTON, 405, 148, 90, 22, IDC_BROWSE_BTN);

            // --- Messaggi cifrati, parte 1: chiave condivisa + modalità (occupa lo stesso
            // rettangolo dei Group 1+2 sopra, nascosto/mostrato in blocco da UpdateToolUI) ---
            mkM(L"BUTTON", L"Chiave condivisa", BS_GROUPBOX, 10, 45, 495, 45, 0);
            mkM(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD, 20, 63, 475, 22, IDC_MSG_PASSPHRASE_EDIT);

            mkM(L"BUTTON", L"Modalità", BS_GROUPBOX, 10, 95, 495, 77, 0);
            mkM(L"BUTTON", L"Scrivi messaggio", BS_AUTORADIOBUTTON | WS_GROUP, 25, 114, 200, 18, IDC_MSG_RADIO_WRITE);
            mkM(L"BUTTON", L"Leggi messaggio", BS_AUTORADIOBUTTON, 235, 114, 200, 18, IDC_MSG_RADIO_READ);
            CheckDlgButton(hwnd, IDC_MSG_RADIO_WRITE, BST_CHECKED);
            mkM(L"STATIC",
                L"Scrivi: il messaggio viene cifrato e mostrato come QR. Leggi: la fotocamera "
                L"scansiona e decifra.",
                0, 25, 138, 460, 28, 0);

            // --- Group 3: parametri fotocamera, comuni a entrambe le modalità (riguardano solo la
            // cattura/visualizzazione ottica, non il protocollo) ---
            mk(L"BUTTON", L"Fotocamera (comune a entrambe le modalità)", BS_GROUPBOX, 10, 182, 495, 96, 0);
            mk(L"STATIC", L"Indice camera:", 0, 20, 200, 95, 18, 0);
            mk(L"EDIT", L"0", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 118, 197, 50, 22, IDC_CAMERA_EDIT);
            mk(L"STATIC", L"Ris. (LxA):", 0, 180, 200, 90, 18, 0);
            // 640x480 (2026-07-05) — matches OpticalLinkConfig::camera_width/height, the
            // library/CLI default too. Fully editable per machine: type "max" to opt back into
            // auto-detection, "0x0" to skip any request and leave the camera fully alone, or any
            // other specific WxH.
            mk(L"EDIT", L"640x480", WS_BORDER | ES_AUTOHSCROLL, 275, 197, 90, 22, IDC_RES_EDIT);
            // Checked by default (see OpticalLinkConfig::request_mjpeg): a direct A/B on real
            // hardware confirmed raw capture costing ~100x more per frame than MJPEG on this
            // machine, with no measurable reliability benefit from turning it off. Still a runtime
            // toggle, not hard-coded, since a specific camera could still disagree — must match on
            // both sides of the transfer.
            mk(L"BUTTON", L"Richiedi MJPEG", BS_AUTOCHECKBOX, 20, 224, 160, 18, IDC_MJPEG_CHECK);
            CheckDlgButton(hwnd, IDC_MJPEG_CHECK, BST_CHECKED);
            // Unchecked by default (see OpticalLinkConfig::disable_autofocus): untested on real
            // hardware, unlike the MJPEG checkbox above which reflects a confirmed A/B result.
            mk(L"BUTTON", L"Blocca autofocus", BS_AUTOCHECKBOX, 200, 224, 160, 18, IDC_FOCUS_CHECK);
            // Checked by default (2026-07-05) — matches OpticalLinkConfig::use_wechat_qrcode, the
            // library/CLI default too, though still not verified end-to-end on real camera
            // hardware. Needs detect.prototxt/detect.caffemodel/sr.prototxt/sr.caffemodel in a
            // "models/wechat_qrcode" folder next to this executable. Must match on both sides.
            mk(L"BUTTON", L"Usa WeChatQRCode (CNN)", BS_AUTOCHECKBOX, 20, 246, 220, 18, IDC_WECHAT_CHECK);
            CheckDlgButton(hwnd, IDC_WECHAT_CHECK, BST_CHECKED);
            // Checked by default (see OpticalLinkConfig::enhance_camera_contrast) — requests higher
            // camera contrast and zero saturation, aimed at the washed-out/low-contrast captures
            // that trip up a QR decoder; still a runtime toggle in case a specific camera's default
            // contrast is already well-tuned.
            mk(L"BUTTON", L"Migliora contrasto fotocamera", BS_AUTOCHECKBOX, 250, 246, 240, 18, IDC_CONTRAST_CHECK);
            CheckDlgButton(hwnd, IDC_CONTRAST_CHECK, BST_CHECKED);

            // --- Group 4: parametri specifici della modalità bidirezionale (grigi/disabilitati
            // quando è selezionata Unidirezionale — vedi UpdateModeUI) ---
            mkT(L"BUTTON", L"Bidirezionale (TCP-like)", BS_GROUPBOX, 10, 288, 240, 100, 0);
            mkT(L"STATIC", L"FPS iniziale:", 0, 20, 308, 85, 18, 0);
            // GUI-only default of 30 (2026-07-05) — deliberately higher than the library/CLI
            // default (OpticalLinkConfig::fps/AdaptiveConfig::fps_initial, 10.0 as of 2026-07-06):
            // requested specifically for GUI use, paired with the lower 640x480 resolution default
            // and WeChatQRCode. Just the AIMD's *starting* point, not a fixed value — it ramps from
            // here. Unlike the fixed FPS in the Unidirezionale group to the right, this field is
            // only used when Bidirezionale is selected.
            mkT(L"EDIT", L"30", WS_BORDER | ES_AUTOHSCROLL, 110, 305, 50, 22, IDC_FPS_EDIT);
            mkT(L"STATIC", L"QR per frame:", 0, 20, 332, 85, 18, 0);
            // Classic default "1" — exactly the original single-QR-at-a-time behavior. >1 is the
            // experimental multi-QR-per-frame mode (see OpticalLinkConfig::qr_codes_per_frame): a
            // runtime toggle, not a hard swap, specifically so it can be set back to 1 without
            // needing a rebuild if it doesn't hold up on real hardware. Must match on both machines.
            // Not compatible with Unidirezionale (the broadcast sender never listens for anything,
            // so a batch that didn't fill would never get flushed to screen at all) — forced to 1
            // and disabled whenever that mode is selected, see UpdateModeUI/IDC_START_BTN handling.
            mkT(L"EDIT", L"1", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 110, 329, 50, 22, IDC_QRGRID_EDIT);
            mkT(L"STATIC", L"Chunk max (byte):", 0, 20, 356, 100, 18, 0);
            // Opt-in override for AdaptiveConfig::chunk_size_max — the AIMD's ceiling on how many
            // bytes of file payload a single DATA frame/QR code is allowed to grow to. Higher packs
            // more bytes per QR (fewer QR codes per file) at the cost of a higher QR version
            // (larger, more finely-detailed codes, more sensitive to camera resolution/focus/
            // distance); lower does the opposite. Safe regardless of value — upshift() also caps
            // growth by what the current EC level can actually encode. Must match on both sides.
            mkT(L"EDIT", L"180", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 125, 353, 60, 22, IDC_CHUNKMAX_EDIT);

            // --- Group 5: parametri specifici della modalità unidirezionale (grigi/disabilitati
            // quando è selezionata Bidirezionale — vedi UpdateModeUI) ---
            mkT(L"BUTTON", L"Unidirezionale (broadcast)", BS_GROUPBOX, 260, 288, 245, 100, 0);
            mkT(L"STATIC", L"FPS:", 0, 270, 308, 75, 18, 0);
            // Selezionabile tra 3, 5, 7, 10, 15, 24 (CBS_DROPDOWNLIST: nessun testo libero) — questa
            // modalità non ha AIMD da cui adattare un fps (nessun canale di feedback), quindi resta
            // un piccolo insieme di valori sicuri predefiniti invece di un unico valore forzato.
            // L'altezza (160) include lo spazio per la lista a tendina aperta (6 voci) — quirk di
            // Win32, la finestra visibile a riposo resta comunque alta quanto un EDIT singola riga.
            {
                HWND fps_bc = mkT(L"COMBOBOX", L"", WS_BORDER | CBS_DROPDOWNLIST, 350, 305, 55, 160, IDC_FPS_BROADCAST_EDIT);
                SendMessageW(fps_bc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"3"));
                SendMessageW(fps_bc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"5"));
                SendMessageW(fps_bc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"7"));
                SendMessageW(fps_bc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"10"));
                SendMessageW(fps_bc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"15"));
                SendMessageW(fps_bc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"24"));
                SendMessageW(fps_bc, CB_SETCURSEL, 0, 0); // default: 3
            }
            mkT(L"STATIC", L"Chunk (byte):", 0, 270, 332, 85, 18, 0);
            // Same "bytes of file payload per QR" meaning as the Bidirezionale group's own Chunk
            // max field, just a separate control (own default 180) so switching modes never leaves
            // an ambiguous shared value on screen.
            mkT(L"EDIT", L"180", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 360, 329, 60, 22, IDC_CHUNKSIZE_EDIT);
            mkT(L"STATIC", L"Loop (0=infinito):", 0, 270, 356, 100, 18, 0);
            // BroadcastConfig::loops — 0 (default) loops the whole file indefinitely until "Ferma"
            // is pressed, the natural default for a continuous broadcast with no return channel to
            // ever signal "the receiver has everything, stop."
            mkT(L"EDIT", L"0", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 375, 353, 50, 22, IDC_LOOPS_EDIT);

            // --- Messaggi cifrati, parte 2: messaggio da scrivere + risultato letto (occupa lo
            // stesso rettangolo dei Group 4+5 sopra) ---
            mkM(L"BUTTON", L"Messaggio da scrivere / ricevuto", BS_GROUPBOX, 10, 288, 495, 105, 0);
            HWND msg_text_edit = mkM(L"EDIT", L"",
                WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL, 20, 306, 465, 34, IDC_MSG_TEXT_EDIT);
            // Hard cap at the control level (unlike the file-transfer path, Win32 EDIT supports this
            // directly) -- matches web/messenger.js's textarea maxlength="1000" exactly.
            SendMessageW(msg_text_edit, EM_LIMITTEXT, kMsgMaxChars, 0);
            mkM(L"STATIC", L"0/1000", SS_RIGHT, 380, 342, 105, 14, IDC_MSG_CHARCOUNT_STATIC);
            mkM(L"EDIT", L"",
                WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 20, 360, 465, 28,
                IDC_MSG_RESULT_EDIT);

            mk(L"BUTTON", L"Avvia", BS_PUSHBUTTON, 10, 398, 100, 28, IDC_START_BTN);
            HWND stop = mk(L"BUTTON", L"Ferma", BS_PUSHBUTTON, 120, 398, 100, 28, IDC_STOP_BTN);
            EnableWindow(stop, FALSE);
            mk(L"STATIC", L"Pronto.", 0, 235, 403, 260, 20, IDC_STATUS_STATIC);

            HWND log_edit = mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 10, 434, 485, 220, IDC_LOG_EDIT);
            // Default EDIT control limit is ~32K characters; past that it silently stops accepting
            // more text. 0 asks for the implementation's own maximum instead of a fixed number.
            SendMessageW(log_edit, EM_LIMITTEXT, 0, 0);
            UpdateModeUI(hwnd); // matches the default Bidirezionale selection above
            UpdateToolUI(app);  // matches the default Trasferimento file selection above
            UpdateMessengerModeUI(hwnd);
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_BROWSE_BTN) {
                bool is_send = IsDlgButtonChecked(hwnd, IDC_RADIO_SEND) == BST_CHECKED;
                std::wstring picked = is_send ? BrowseForFile(hwnd) : BrowseForFolder(hwnd);
                if (!picked.empty()) SetDlgItemTextW(hwnd, IDC_PATH_EDIT, picked.c_str());
            } else if (id == IDC_RADIO_BIDIRECTIONAL || id == IDC_RADIO_BROADCAST) {
                UpdateModeUI(hwnd);
            } else if (id == IDC_RADIO_TOOL_TRANSFER || id == IDC_RADIO_TOOL_MESSENGER) {
                UpdateToolUI(app);
            } else if (id == IDC_MSG_RADIO_WRITE || id == IDC_MSG_RADIO_READ) {
                UpdateMessengerModeUI(hwnd);
            } else if (id == IDC_MSG_TEXT_EDIT && HIWORD(wParam) == EN_CHANGE) {
                UpdateMessengerCharCount(hwnd);
            } else if (id == IDC_START_BTN && IsDlgButtonChecked(hwnd, IDC_RADIO_TOOL_MESSENGER) == BST_CHECKED) {
                if (app->running.load()) break;

                wchar_t pass_buf[256];
                GetDlgItemTextW(hwnd, IDC_MSG_PASSPHRASE_EDIT, pass_buf, 256);
                std::wstring passphrase_w(pass_buf);
                if (passphrase_w.empty()) {
                    MessageBoxW(hwnd, L"Imposta prima una chiave condivisa (identica sull'altro dispositivo).",
                                L"QRTP", MB_ICONWARNING);
                    break;
                }
                bool is_write = IsDlgButtonChecked(hwnd, IDC_MSG_RADIO_WRITE) == BST_CHECKED;
                std::wstring message_w;
                if (is_write) {
                    int len = GetWindowTextLengthW(GetDlgItem(hwnd, IDC_MSG_TEXT_EDIT));
                    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1);
                    GetDlgItemTextW(hwnd, IDC_MSG_TEXT_EDIT, buf.data(), len + 1);
                    message_w = buf.data();
                    if (message_w.empty()) {
                        MessageBoxW(hwnd, L"Scrivi prima un messaggio.", L"QRTP", MB_ICONWARNING);
                        break;
                    }
                }
                int camera_index = GetDlgItemInt(hwnd, IDC_CAMERA_EDIT, nullptr, FALSE);
                int camera_width = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                int camera_height = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                wchar_t res_buf[32];
                GetDlgItemTextW(hwnd, IDC_RES_EDIT, res_buf, 32);
                if (res_buf[0] != L'\0' && _wcsicmp(res_buf, L"max") != 0) {
                    if (const wchar_t* x_pos = wcschr(res_buf, L'x')) {
                        camera_width = _wtoi(res_buf);
                        camera_height = _wtoi(x_pos + 1);
                        if (camera_width < 0 || camera_height < 0) {
                            camera_width = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                            camera_height = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                        }
                    } else {
                        camera_width = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                        camera_height = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                    }
                }
                bool request_mjpeg = IsDlgButtonChecked(hwnd, IDC_MJPEG_CHECK) == BST_CHECKED;
                bool disable_autofocus = IsDlgButtonChecked(hwnd, IDC_FOCUS_CHECK) == BST_CHECKED;
                bool use_wechat_qrcode = IsDlgButtonChecked(hwnd, IDC_WECHAT_CHECK) == BST_CHECKED;
                bool enhance_contrast = IsDlgButtonChecked(hwnd, IDC_CONTRAST_CHECK) == BST_CHECKED;

                SetWindowTextW(GetDlgItem(hwnd, IDC_LOG_EDIT), L"");
                SetWindowTextW(GetDlgItem(hwnd, IDC_MSG_RESULT_EDIT), L"");
                SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS_STATIC),
                               is_write ? L"QR mostrato — in attesa di chiusura..." : L"In attesa di un QR...");
                EnableWindow(GetDlgItem(hwnd, IDC_START_BTN), FALSE);
                EnableWindow(GetDlgItem(hwnd, IDC_STOP_BTN), TRUE);

                app->cancel.store(false);
                app->running.store(true);
                if (app->worker.joinable()) app->worker.join();
                app->worker = std::thread(RunMessenger, app, is_write, passphrase_w, message_w, camera_index,
                                           camera_width, camera_height, request_mjpeg, disable_autofocus,
                                           use_wechat_qrcode, enhance_contrast);
            } else if (id == IDC_START_BTN) {
                if (app->running.load()) break;

                wchar_t path_buf[MAX_PATH];
                GetDlgItemTextW(hwnd, IDC_PATH_EDIT, path_buf, MAX_PATH);
                std::wstring path_w(path_buf);
                if (path_w.empty()) {
                    MessageBoxW(hwnd, L"Scegli prima un file (invio) o una cartella (ricezione).", L"QRTP",
                                MB_ICONWARNING);
                    break;
                }
                bool is_send = IsDlgButtonChecked(hwnd, IDC_RADIO_SEND) == BST_CHECKED;
                int camera_index = GetDlgItemInt(hwnd, IDC_CAMERA_EDIT, nullptr, FALSE);
                wchar_t fps_buf[32];
                GetDlgItemTextW(hwnd, IDC_FPS_EDIT, fps_buf, 32);
                double fps = wcstod(fps_buf, nullptr);
                if (fps <= 0.0) fps = 3.0;

                // "max" or blank (the default) -> auto-detect the camera's highest mode; "0x0" ->
                // skip any request and leave the camera fully alone; any other "WxH" -> request
                // that specific resolution. See OpticalLinkConfig::kAutoMaxResolution.
                int camera_width = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                int camera_height = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                wchar_t res_buf[32];
                GetDlgItemTextW(hwnd, IDC_RES_EDIT, res_buf, 32);
                if (res_buf[0] != L'\0' && _wcsicmp(res_buf, L"max") != 0) {
                    if (const wchar_t* x_pos = wcschr(res_buf, L'x')) {
                        camera_width = _wtoi(res_buf);
                        camera_height = _wtoi(x_pos + 1);
                        if (camera_width < 0 || camera_height < 0) {
                            camera_width = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                            camera_height = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                        }
                    } else {
                        camera_width = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                        camera_height = qrtp::OpticalLinkConfig::kAutoMaxResolution;
                    }
                }

                int qr_grid = GetDlgItemInt(hwnd, IDC_QRGRID_EDIT, nullptr, FALSE);
                if (qr_grid <= 0) qr_grid = 1;
                bool request_mjpeg = IsDlgButtonChecked(hwnd, IDC_MJPEG_CHECK) == BST_CHECKED;
                bool disable_autofocus = IsDlgButtonChecked(hwnd, IDC_FOCUS_CHECK) == BST_CHECKED;
                bool use_wechat_qrcode = IsDlgButtonChecked(hwnd, IDC_WECHAT_CHECK) == BST_CHECKED;
                bool enhance_contrast = IsDlgButtonChecked(hwnd, IDC_CONTRAST_CHECK) == BST_CHECKED;
                bool broadcast_mode = IsDlgButtonChecked(hwnd, IDC_RADIO_BROADCAST) == BST_CHECKED;
                UINT chunk_max = GetDlgItemInt(hwnd, broadcast_mode ? IDC_CHUNKSIZE_EDIT : IDC_CHUNKMAX_EDIT,
                                                nullptr, FALSE);
                if (chunk_max == 0) chunk_max = 180;
                int loops = static_cast<int>(GetDlgItemInt(hwnd, IDC_LOOPS_EDIT, nullptr, FALSE));

                if (broadcast_mode) {
                    // Read from the Unidirezionale group's own combobox (3/5/7), not IDC_FPS_EDIT
                    // (that field belongs to the Bidirezionale group and is disabled in this mode)
                    // — BroadcastConfig has no AIMD to ramp this from, so it's one of a small,
                    // known-safe preset choices instead of arbitrary free text.
                    wchar_t fps_bc_buf[8];
                    GetDlgItemTextW(hwnd, IDC_FPS_BROADCAST_EDIT, fps_bc_buf, 8);
                    fps = wcstod(fps_bc_buf, nullptr);
                    if (fps <= 0.0) fps = 3.0;
                    // Multi-QR-per-frame batching isn't compatible with the broadcast sender, which
                    // never listens for anything: a batch that didn't fill would never get flushed
                    // to screen at all (see OpticalLink::try_receive_multi()'s flush-on-poll logic
                    // in optical_link.cpp, which the broadcast path never reaches). Forced to 1
                    // regardless of IDC_QRGRID_EDIT's value (disabled in this mode anyway).
                    qr_grid = 1;
                }

                SetWindowTextW(GetDlgItem(hwnd, IDC_LOG_EDIT), L"");
                SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS_STATIC), is_send ? L"Invio in corso..." : L"Ricezione in corso...");
                EnableWindow(GetDlgItem(hwnd, IDC_START_BTN), FALSE);
                EnableWindow(GetDlgItem(hwnd, IDC_STOP_BTN), TRUE);

                app->cancel.store(false);
                app->running.store(true);
                if (app->worker.joinable()) app->worker.join(); // previous run already finished
                app->worker = std::thread(RunTransfer, app, is_send, path_w, camera_index, fps, camera_width,
                                           camera_height, qr_grid, request_mjpeg, disable_autofocus,
                                           use_wechat_qrcode, enhance_contrast, static_cast<size_t>(chunk_max),
                                           broadcast_mode, loops);
            } else if (id == IDC_STOP_BTN) {
                app->cancel.store(true);
                SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS_STATIC), L"Arresto in corso...");
            }
            return 0;
        }

        case WM_APP_LOG: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            AppendLog(GetDlgItem(hwnd, IDC_LOG_EDIT), *text);
            delete text;
            return 0;
        }

        case WM_APP_DONE: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            bool success = wParam != 0;
            AppendLog(GetDlgItem(hwnd, IDC_LOG_EDIT), (success ? L"OK: " : L"ERRORE: ") + *text);
            SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS_STATIC), success ? L"Completato." : L"Terminato con errore.");
            delete text;
            app->running.store(false);
            EnableWindow(GetDlgItem(hwnd, IDC_START_BTN), TRUE);
            EnableWindow(GetDlgItem(hwnd, IDC_STOP_BTN), FALSE);
            return 0;
        }

        // Messenger read mode's own success path -- `text` is the decrypted plaintext itself,
        // routed into IDC_MSG_RESULT_EDIT instead of just the log (see RunMessenger).
        case WM_APP_MSG_READ_DONE: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            bool success = wParam != 0;
            if (success) {
                SetWindowTextW(GetDlgItem(hwnd, IDC_MSG_RESULT_EDIT), text->c_str());
                AppendLog(GetDlgItem(hwnd, IDC_LOG_EDIT), L"OK: messaggio decifrato.");
            } else {
                AppendLog(GetDlgItem(hwnd, IDC_LOG_EDIT), L"ERRORE: " + *text);
            }
            SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS_STATIC), success ? L"Completato." : L"Terminato con errore.");
            delete text;
            app->running.store(false);
            EnableWindow(GetDlgItem(hwnd, IDC_START_BTN), TRUE);
            EnableWindow(GetDlgItem(hwnd, IDC_STOP_BTN), FALSE);
            return 0;
        }

        case WM_CLOSE:
            if (app && app->running.load()) {
                app->cancel.store(true);
                if (app->worker.joinable()) app->worker.join();
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // Must happen before any window (including this one) is created, or Windows may have already
    // decided to bitmap-stretch/blur this process's windows on a scaled display.
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t* kClassName = L"QrtpMainWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    // Embedded via windows/qrtp.rc (IDI_APP_ICON, windows/resource.h) — shows in the title bar,
    // taskbar, Alt-Tab, and Explorer; hIconSm specifically for the small title-bar/taskbar variant
    // so it isn't just hIcon crudely downscaled.
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    AppState app;

    // Height grown from the original 501 (2026-07-06), then again to 700 (2026-07-10) to fit the
    // "Strumento" selector and the "Messaggi cifrati" tool's own controls, which occupy the same
    // rectangle as the "Trasferimento file" tool's own controls (see UpdateToolUI) since Win32 has
    // no lightweight tab control equivalent to GTK's GtkNotebook (gui_gtk.cpp).
    HWND hwnd = CreateWindowExW(0, kClassName, L"QRTP — Trasferimento file e messaggi via QR", WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 520, 700, nullptr, nullptr, hInstance, &app);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (app.worker.joinable()) app.worker.join();
    CoUninitialize();
    return 0;
}

#endif // QRTP_WITH_OPENCV
#endif // _WIN32
