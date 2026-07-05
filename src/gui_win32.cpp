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
void RunTransfer(AppState* app, bool is_send, std::wstring path_w, int camera_index, double fps,
                  int camera_width, int camera_height) {
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
        ocfg.log = post_log;
        qrtp::OpticalLink link(ocfg);

        qrtp::ProtocolConfig cfg;
        cfg.cancel = &app->cancel;
        cfg.log = post_log;

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
            mk(L"EDIT", L"3", WS_BORDER | ES_AUTOHSCROLL, 220, 77, 50, 22, IDC_FPS_EDIT);
            mk(L"STATIC", L"Ris. (LxA):", 0, 285, 80, 85, 20, 0);
            // Blank by default = leave the camera's own resolution alone (see OpticalLinkConfig —
            // forcing one automatically was tried and helped one camera while hurting another on
            // real hardware). Opt-in: fill in e.g. "1280x720" to try a specific one on this machine.
            mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 375, 77, 100, 22, IDC_RES_EDIT);

            mk(L"BUTTON", L"Avvia", BS_PUSHBUTTON, 10, 115, 100, 28, IDC_START_BTN);
            HWND stop = mk(L"BUTTON", L"Ferma", BS_PUSHBUTTON, 120, 115, 100, 28, IDC_STOP_BTN);
            EnableWindow(stop, FALSE);
            mk(L"STATIC", L"Pronto.", 0, 235, 120, 260, 20, IDC_STATUS_STATIC);

            HWND log_edit = mk(L"EDIT", L"",
                                WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 10, 155, 485,
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

                // Optional "WxH" (e.g. "1280x720"); left blank, both stay 0 and OpticalLink leaves
                // the camera's resolution alone (see the field's own comment above for why this is
                // opt-in rather than an automatic default).
                int camera_width = 0, camera_height = 0;
                wchar_t res_buf[32];
                GetDlgItemTextW(hwnd, IDC_RES_EDIT, res_buf, 32);
                if (const wchar_t* x_pos = wcschr(res_buf, L'x')) {
                    camera_width = _wtoi(res_buf);
                    camera_height = _wtoi(x_pos + 1);
                    if (camera_width <= 0 || camera_height <= 0) camera_width = camera_height = 0;
                }

                SetWindowTextW(GetDlgItem(hwnd, IDC_LOG_EDIT), L"");
                SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS_STATIC), is_send ? L"Invio in corso..." : L"Ricezione in corso...");
                EnableWindow(GetDlgItem(hwnd, IDC_START_BTN), FALSE);
                EnableWindow(GetDlgItem(hwnd, IDC_STOP_BTN), TRUE);

                app->cancel.store(false);
                app->running.store(true);
                if (app->worker.joinable()) app->worker.join(); // previous run already finished
                app->worker =
                    std::thread(RunTransfer, app, is_send, path_w, camera_index, fps, camera_width, camera_height);
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
                                 CW_USEDEFAULT, CW_USEDEFAULT, 520, 430, nullptr, nullptr, hInstance, &app);
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
