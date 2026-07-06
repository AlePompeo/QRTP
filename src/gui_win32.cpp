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

#include "qrtp/optical_link.hpp"
#include "qrtp/protocol.hpp"

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

// Custom messages posted from the worker thread to the UI thread. Never touch a control directly
// from the worker thread — Win32 windows are single-threaded-affine, all updates must happen here.
constexpr UINT WM_APP_LOG = WM_APP + 1;   // lParam = new std::wstring*, UI thread deletes it
constexpr UINT WM_APP_DONE = WM_APP + 2;  // wParam = success (0/1), lParam = new std::wstring*

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

struct AppState {
    HWND hwnd = nullptr;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
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

// Runs on the worker thread. Never touches Win32 controls directly.
void RunTransfer(AppState* app, bool is_send, std::wstring path_w, int camera_index, double fps, int camera_width,
                  int camera_height, int qr_grid, bool request_mjpeg, bool disable_autofocus,
                  bool use_wechat_qrcode, bool enhance_contrast, size_t chunk_max) {
    HWND hwnd = app->hwnd;
    auto post_log = [hwnd](const std::string& msg) {
        auto* copy = new std::wstring(Utf8ToWide(msg));
        PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy));
    };
    auto post_done = [hwnd](bool success, const std::string& msg) {
        auto* copy = new std::wstring(Utf8ToWide(msg));
        PostMessageW(hwnd, WM_APP_DONE, success ? 1 : 0, reinterpret_cast<LPARAM>(copy));
    };

    try {
        qrtp::OpticalLinkConfig ocfg;
        ocfg.camera_index = camera_index;
        ocfg.fps = fps;
        ocfg.camera_width = camera_width;
        ocfg.camera_height = camera_height;
        ocfg.qr_codes_per_frame = qr_grid;
        ocfg.request_mjpeg = request_mjpeg;
        ocfg.disable_autofocus = disable_autofocus;
        ocfg.use_wechat_qrcode = use_wechat_qrcode;
        ocfg.enhance_camera_contrast = enhance_contrast;
        ocfg.log = post_log;
        qrtp::OpticalLink link(ocfg);

        qrtp::ProtocolConfig cfg;
        cfg.cancel = &app->cancel;
        cfg.log = post_log;
        cfg.adaptive.chunk_size_max = chunk_max;

        if (is_send) {
            qrtp::Sender sender(link, cfg);
            auto result = sender.send_file(std::filesystem::path(path_w));
            post_done(result.success, result.message);
        } else {
            qrtp::Receiver receiver(link, cfg);
            qrtp::TransferMetadata meta;
            auto received = receiver.receive_file(meta, std::chrono::minutes(30));
            if (!received) {
                post_done(false, app->cancel.load() ? "cancelled" : "receive failed (timeout or hash mismatch)");
                return;
            }
            auto out_path = std::filesystem::path(path_w) / meta.filename;
            std::ofstream f(out_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(received->data()), static_cast<std::streamsize>(received->size()));
            post_done(true, "wrote " + std::to_string(received->size()) + " bytes to " + out_path.string());
        }
    } catch (const std::exception& e) {
        post_done(false, e.what());
    }
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

            mk(L"BUTTON", L"Invia file", BS_AUTORADIOBUTTON | WS_GROUP, 10, 10, 150, 20, IDC_RADIO_SEND);
            mk(L"BUTTON", L"Ricevi file", BS_AUTORADIOBUTTON, 170, 10, 150, 20, IDC_RADIO_RECEIVE);
            CheckDlgButton(hwnd, IDC_RADIO_SEND, BST_CHECKED);

            mk(L"STATIC", L"File / cartella:", 0, 10, 45, 100, 20, 0);
            mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 110, 42, 290, 22, IDC_PATH_EDIT);
            mk(L"BUTTON", L"Sfoglia...", BS_PUSHBUTTON, 405, 42, 90, 22, IDC_BROWSE_BTN);

            mk(L"STATIC", L"Indice camera:", 0, 10, 80, 100, 20, 0);
            mk(L"EDIT", L"0", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 110, 77, 50, 22, IDC_CAMERA_EDIT);
            mk(L"STATIC", L"FPS:", 0, 175, 80, 40, 20, 0);
            // GUI-only default of 30 (2026-07-05) — deliberately higher than the library/CLI
            // default (OpticalLinkConfig::fps, still used by `qrtp send`/`qrtp receive` and as the
            // AdaptiveController's own fps_initial — raised from 3.0 to 10.0 on 2026-07-06, still
            // below this GUI-only value): requested specifically for GUI use, paired with the lower
            // 640x480 resolution default and WeChatQRCode below. Still just the pre-filled text of
            // an editable field, not a code-level change to the safe handshake-survives-camera-
            // settling starting point documented in AdaptiveConfig.
            mk(L"EDIT", L"30", WS_BORDER | ES_AUTOHSCROLL, 220, 77, 50, 22, IDC_FPS_EDIT);
            mk(L"STATIC", L"Ris. (LxA):", 0, 285, 80, 85, 20, 0);
            // 640x480 (2026-07-05) — matches OpticalLinkConfig::camera_width/height, the
            // library/CLI default too (paired with WeChatQRCode below; only the 30fps default
            // above is GUI-only). Still fully editable per machine: type "max" to opt back into
            // auto-detection, "0x0" to skip any request and leave the camera fully alone, or any
            // other specific WxH.
            mk(L"EDIT", L"640x480", WS_BORDER | ES_AUTOHSCROLL, 375, 77, 100, 22, IDC_RES_EDIT);

            mk(L"STATIC", L"QR per frame:", 0, 10, 106, 100, 20, 0);
            // Classic default "1" — exactly the original single-QR-at-a-time behavior. >1 is the
            // experimental multi-QR-per-frame mode (see OpticalLinkConfig::qr_codes_per_frame): a
            // runtime toggle, not a hard swap, specifically so it can be set back to 1 without
            // needing a rebuild if it doesn't hold up on real hardware. Must match on both machines.
            mk(L"EDIT", L"1", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 130, 103, 50, 22, IDC_QRGRID_EDIT);

            // Checked by default (see OpticalLinkConfig::request_mjpeg): a direct A/B on real
            // hardware confirmed raw capture costing ~100x more per frame than MJPEG on this
            // machine, with no measurable reliability benefit from turning it off. Still a runtime
            // toggle, not hard-coded, since a specific camera could still disagree — must match on
            // both sides of the transfer.
            mk(L"BUTTON", L"Richiedi MJPEG", BS_AUTOCHECKBOX, 200, 104, 160, 20, IDC_MJPEG_CHECK);
            CheckDlgButton(hwnd, IDC_MJPEG_CHECK, BST_CHECKED);

            // Unchecked by default (see OpticalLinkConfig::disable_autofocus): untested on real
            // hardware, unlike the MJPEG checkbox above which reflects a confirmed A/B result.
            // Offered as the next experiment for the still-open avg_capture_ms/avg_decode_ms
            // correlation-with-fps question (not_working.md, 2026-07-05) — autofocus hunting under
            // fast-changing on-screen content is a candidate, not a confirmed fix.
            mk(L"BUTTON", L"Blocca autofocus", BS_AUTOCHECKBOX, 10, 127, 160, 20, IDC_FOCUS_CHECK);

            // Checked by default (2026-07-05) — matches OpticalLinkConfig::use_wechat_qrcode, the
            // library/CLI default too, though still not verified end-to-end on real camera
            // hardware. Needs detect.prototxt/detect.caffemodel/sr.prototxt/sr.caffemodel in a
            // "models/wechat_qrcode" folder next to this executable — see models/wechat_qrcode/
            // LICENSE. Must match on both sides of the transfer.
            mk(L"BUTTON", L"Usa WeChatQRCode (CNN)", BS_AUTOCHECKBOX, 200, 127, 200, 20, IDC_WECHAT_CHECK);
            CheckDlgButton(hwnd, IDC_WECHAT_CHECK, BST_CHECKED);

            // Checked by default (see OpticalLinkConfig::enhance_camera_contrast) — requests higher
            // camera contrast and zero saturation, aimed at the washed-out/low-contrast captures
            // that trip up a QR decoder; still a runtime toggle in case a specific camera's default
            // contrast is already well-tuned.
            mk(L"BUTTON", L"Migliora contrasto fotocamera", BS_AUTOCHECKBOX, 200, 149, 260, 20, IDC_CONTRAST_CHECK);
            CheckDlgButton(hwnd, IDC_CONTRAST_CHECK, BST_CHECKED);

            mk(L"STATIC", L"Chunk max:", 0, 10, 150, 100, 20, 0);
            // Opt-in override for AdaptiveConfig::chunk_size_max — the AIMD's ceiling on how many
            // bytes of file payload a single DATA frame/QR code is allowed to grow to. Higher packs
            // more bytes per QR (fewer QR codes per file) at the cost of a higher QR version
            // (larger, more finely-detailed codes, more sensitive to camera resolution/focus/
            // distance); lower does the opposite. Safe regardless of value — upshift() also caps
            // growth by what the current EC level can actually encode, so this alone can't ask for
            // a payload too large to fit any QR version. Must match on both sides of the transfer.
            // 1024 -> 180 (2026-07-06), explicit user request, matching the new library/CLI default
            // (AdaptiveConfig::chunk_size_max/chunk_size_initial) — paired with the fps field's own
            // ramp-up now starting from a faster base too.
            mk(L"EDIT", L"180", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 130, 147, 60, 22, IDC_CHUNKMAX_EDIT);

            mk(L"BUTTON", L"Avvia", BS_PUSHBUTTON, 10, 186, 100, 28, IDC_START_BTN);
            HWND stop = mk(L"BUTTON", L"Ferma", BS_PUSHBUTTON, 120, 186, 100, 28, IDC_STOP_BTN);
            EnableWindow(stop, FALSE);
            mk(L"STATIC", L"Pronto.", 0, 235, 191, 260, 20, IDC_STATUS_STATIC);

            HWND log_edit = mk(L"EDIT", L"",
                                WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 10, 226, 485,
                                220, IDC_LOG_EDIT);
            // Default EDIT control limit is ~32K characters; past that it silently stops accepting
            // more text. 0 asks for the implementation's own maximum instead of a fixed number.
            SendMessageW(log_edit, EM_LIMITTEXT, 0, 0);
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_BROWSE_BTN) {
                bool is_send = IsDlgButtonChecked(hwnd, IDC_RADIO_SEND) == BST_CHECKED;
                std::wstring picked = is_send ? BrowseForFile(hwnd) : BrowseForFolder(hwnd);
                if (!picked.empty()) SetDlgItemTextW(hwnd, IDC_PATH_EDIT, picked.c_str());
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
                UINT chunk_max = GetDlgItemInt(hwnd, IDC_CHUNKMAX_EDIT, nullptr, FALSE);
                if (chunk_max == 0) chunk_max = 1024;

                SetWindowTextW(GetDlgItem(hwnd, IDC_LOG_EDIT), L"");
                SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS_STATIC), is_send ? L"Invio in corso..." : L"Ricezione in corso...");
                EnableWindow(GetDlgItem(hwnd, IDC_START_BTN), FALSE);
                EnableWindow(GetDlgItem(hwnd, IDC_STOP_BTN), TRUE);

                app->cancel.store(false);
                app->running.store(true);
                if (app->worker.joinable()) app->worker.join(); // previous run already finished
                app->worker = std::thread(RunTransfer, app, is_send, path_w, camera_index, fps, camera_width,
                                           camera_height, qr_grid, request_mjpeg, disable_autofocus,
                                           use_wechat_qrcode, enhance_contrast, static_cast<size_t>(chunk_max));
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
    RegisterClassExW(&wc);

    AppState app;

    HWND hwnd = CreateWindowExW(0, kClassName, L"QRTP — Trasferimento file via QR", WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 520, 501, nullptr, nullptr, hInstance, &app);
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
