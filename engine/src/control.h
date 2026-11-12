// Papyrus - control window and tray icon.
//
// This is the small always-there part: a tray icon, a tiny window with the switches, and nothing else.
// The capture/encode pipeline does NOT run in this process. When the replay buffer is switched on, this
// process starts the same exe again as a hidden worker ("--replay --child"); switching it off asks the worker
// to exit, which hands every byte of video memory and RAM back to Windows. Plain Win32, no UI framework.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>

namespace ctl {

static const wchar_t* const kStopEvent = L"Local\\Papyrus.Worker.Stop";
static const wchar_t* const kSaveEvent = L"Local\\Papyrus.Worker.Save";
static const wchar_t* const kWndClass  = L"PapyrusControl";
static const wchar_t* const kMutex     = L"PapyrusControllerMutex";

enum { IDC_TOGGLE = 101, IDC_ALWAYS, IDC_SAVE, IDC_OPEN, IDM_SHOW = 201, IDM_EXIT };
enum { WM_TRAY = WM_APP + 1, WM_SHOWME = WM_APP + 2, TIMER_WATCH = 1 };

static HWND   g_wnd, g_toggle, g_always, g_save, g_open, g_status;
static HANDLE g_worker, g_evStop, g_evSave;
static bool   g_alwaysOn, g_balloonShown, g_exiting;
static wchar_t g_out[MAX_PATH], g_ini[MAX_PATH], g_err[MAX_PATH], g_stat[MAX_PATH], g_exe[MAX_PATH];
static wchar_t g_lastStatus[400];
static HICON  g_icoOn, g_icoOff;
static HFONT  g_font, g_fontBig;
static UINT   g_msgTaskbar;
static NOTIFYICONDATAW g_nid;
static int    g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

// A round dot drawn at runtime, so the exe needs no icon resource: red = buffer on, grey = off.
static HICON makeIcon(COLORREF dot) {
    const int n = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi; bi.bV5Width = n; bi.bV5Height = -n; bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    uint32_t* px = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !px) return LoadIconW(nullptr, IDI_APPLICATION);
    const uint32_t rgb = ((uint32_t)GetRValue(dot) << 16) | ((uint32_t)GetGValue(dot) << 8) | GetBValue(dot);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const double dx = x - 15.5, dy = y - 15.5, d2 = dx * dx + dy * dy;
            uint32_t c = 0;
            if (d2 <= 15.0 * 15.0) c = 0xFF1E1E1E;          // dark disc
            if (d2 <= 13.0 * 13.0) c = 0xFFF2F2F2;          // light ring
            if (d2 <= 11.0 * 11.0) c = 0xFF1E1E1E;
            if (d2 <= 7.0 * 7.0)   c = 0xFF000000 | rgb;    // state dot
            px[y * n + x] = c;
        }
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON ico = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return ico ? ico : LoadIconW(nullptr, IDI_APPLICATION);
}

static bool workerOn() { return g_worker != nullptr; }

static void readSmallFile(const wchar_t* path, wchar_t* out, int cap) {
    out[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[600];
    DWORD got = 0;
    ReadFile(f, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(f);
    buf[got] = 0;
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, out, cap);
    out[cap - 1] = 0;
}

static void setTray(DWORD action) {
    g_nid.hIcon = workerOn() ? g_icoOn : g_icoOff;
    swprintf(g_nid.szTip, 128, L"Papyrus - replay buffer %ls", workerOn() ? L"ON" : L"OFF");
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    Shell_NotifyIconW(action, &g_nid);
}

static void updateUi(const wchar_t* status) {
    const bool on = workerOn();
    SetWindowTextW(g_toggle, on ? L"Replay buffer is ON      (click to turn off)" : L"Replay buffer is OFF      (click to turn on)");
    SendMessageW(g_toggle, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_always, BM_SETCHECK, g_alwaysOn ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(g_save, on);
    if (status && wcscmp(status, g_lastStatus) != 0) {
        wcsncpy(g_lastStatus, status, 399);
        SetWindowTextW(g_status, status);
    }
    setTray(NIM_MODIFY);
}

static void startWorker() {
    if (workerOn()) return;
    ResetEvent(g_evStop);
    ResetEvent(g_evSave);
    DeleteFileW(g_err);
    DeleteFileW(g_stat);
    wchar_t cmd[MAX_PATH + 96];
    swprintf(cmd, MAX_PATH + 96, L"\"%ls\" --replay --child --parent %lu", g_exe, GetCurrentProcessId());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        wchar_t msg[160];
        swprintf(msg, 160, L"Could not start the recorder (Windows error %lu).", GetLastError());
        updateUi(msg);
        return;
    }
    CloseHandle(pi.hThread);
    g_worker = pi.hProcess;
    updateUi(L"Starting...");
}

static void stopWorker() {
    if (!workerOn()) return;
    SetEvent(g_evStop);
    if (WaitForSingleObject(g_worker, 8000) != WAIT_OBJECT_0) TerminateProcess(g_worker, 1);
    CloseHandle(g_worker);
    g_worker = nullptr;
    updateUi(L"The replay buffer is off. Nothing is being recorded.");
}

static void setAlwaysOn(bool on) {
    g_alwaysOn = on;
    WritePrivateProfileStringW(L"papyrus", L"always_on", on ? L"1" : L"0", g_ini);
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        if (on) {
            wchar_t cmd[MAX_PATH + 16];
            swprintf(cmd, MAX_PATH + 16, L"\"%ls\" --tray", g_exe);
            RegSetValueExW(key, L"Papyrus", 0, REG_SZ, (const BYTE*)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        } else RegDeleteValueW(key, L"Papyrus");
        RegCloseKey(key);
    }
}

static void showWindow() {
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_wnd);
}

static void quit() {
    g_exiting = true;
    stopWorker();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyWindow(g_wnd);
}

static void trayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (workerOn() ? MF_CHECKED : 0), IDC_TOGGLE, L"Replay buffer on");
    AppendMenuW(m, MF_STRING | (g_alwaysOn ? MF_CHECKED : 0), IDC_ALWAYS, L"Always on (start with Windows)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (workerOn() ? 0 : MF_GRAYED), IDC_SAVE, L"Save clip now");
    AppendMenuW(m, MF_STRING, IDC_OPEN, L"Open clips folder");
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"Show Papyrus window");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit Papyrus");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK wndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgTaskbar && g_msgTaskbar) { setTray(NIM_ADD); return 0; }      // Explorer restarted
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TOGGLE: if (workerOn()) stopWorker(); else startWorker(); return 0;
        case IDC_ALWAYS: setAlwaysOn(!g_alwaysOn); updateUi(nullptr); return 0;
        case IDC_SAVE:   if (workerOn()) SetEvent(g_evSave); return 0;
        case IDC_OPEN:   ShellExecuteW(nullptr, L"open", g_out, nullptr, nullptr, SW_SHOWNORMAL); return 0;
        case IDM_SHOW:   showWindow(); return 0;
        case IDM_EXIT:   quit(); return 0;
        }
        break;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) showWindow();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) trayMenu();
        return 0;
    case WM_SHOWME:
        showWindow();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_WATCH && workerOn()) {
            if (WaitForSingleObject(g_worker, 0) == WAIT_OBJECT_0) {         // the recorder stopped by itself
                CloseHandle(g_worker);
                g_worker = nullptr;
                wchar_t err[400], msg[480];
                readSmallFile(g_err, err, 400);
                swprintf(msg, 480, L"The recorder stopped: %ls", err[0] ? err : L"unknown reason - see the log folder.");
                updateUi(msg);
                showWindow();
            } else {
                wchar_t st[400];
                readSmallFile(g_stat, st, 400);
                if (st[0]) updateUi(st);
            }
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        ShowWindow(w, SW_HIDE);                                               // keep running in the tray
        if (!g_balloonShown) {
            g_balloonShown = true;
            g_nid.uFlags = NIF_INFO;
            wcscpy(g_nid.szInfoTitle, L"Papyrus is still running");
            wcscpy(g_nid.szInfo, L"Click the Papyrus icon near the clock to open it again. Right-click it to exit.");
            g_nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) { stopWorker(); Shell_NotifyIconW(NIM_DELETE, &g_nid); }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int cx, int cy, int id, HFONT f) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(cx), S(cy), g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    return h;
}

// outDir = clips folder, dataDir = %LOCALAPPDATA%\Papyrus, hotkeyText e.g. "Ctrl+Shift+F9"
static int run(bool startHidden, const wchar_t* outDir, const wchar_t* dataDir, const wchar_t* versionText, const wchar_t* hotkeyText) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutex);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {                    // already running: just show it
        if (HWND other = FindWindowW(kWndClass, nullptr)) { if (!startHidden) PostMessageW(other, WM_SHOWME, 0, 0); }
        return 0;
    }
    wcscpy(g_out, outDir);
    swprintf(g_ini, MAX_PATH, L"%ls\\settings.ini", dataDir);
    swprintf(g_err, MAX_PATH, L"%ls\\last-error.txt", dataDir);
    swprintf(g_stat, MAX_PATH, L"%ls\\status.txt", dataDir);
    GetModuleFileNameW(nullptr, g_exe, MAX_PATH);
    g_alwaysOn = GetPrivateProfileIntW(L"papyrus", L"always_on", 0, g_ini) != 0;
    g_evStop = CreateEventW(nullptr, TRUE, FALSE, kStopEvent);
    g_evSave = CreateEventW(nullptr, FALSE, FALSE, kSaveEvent);

    g_dpi = (int)GetDpiForSystem();
    g_font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBig = CreateFontW(-S(22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_icoOn = makeIcon(RGB(230, 40, 40));
    g_icoOff = makeIcon(RGB(140, 140, 140));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kWndClass;
    wc.hIcon = g_icoOff;
    wc.hIconSm = g_icoOff;
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{ 0, 0, S(440), S(318) };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    g_wnd = CreateWindowExW(0, kWndClass, L"Papyrus", style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                            ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_wnd) return 1;

    wchar_t line[300];
    mk(L"STATIC", L"Papyrus", 0, 20, 14, 200, 32, 0, g_fontBig);
    mk(L"STATIC", versionText, SS_RIGHT, 220, 24, 200, 20, 0, g_font);
    g_toggle = mk(L"BUTTON", L"", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, 20, 56, 400, 46, IDC_TOGGLE, g_font);
    g_status = mk(L"STATIC", L"", 0, 20, 110, 400, 54, 0, g_font);
    g_always = mk(L"BUTTON", L"Always on: start Papyrus when Windows starts and turn the replay buffer on", BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP | WS_TABSTOP,
                  20, 170, 400, 40, IDC_ALWAYS, g_font);
    g_save = mk(L"BUTTON", L"Save clip now", BS_PUSHBUTTON | WS_TABSTOP, 20, 218, 194, 34, IDC_SAVE, g_font);
    g_open = mk(L"BUTTON", L"Open clips folder", BS_PUSHBUTTON | WS_TABSTOP, 226, 218, 194, 34, IDC_OPEN, g_font);
    swprintf(line, 300, L"While the buffer is on, %ls saves the last 30 seconds. Closing this window keeps Papyrus in the tray near the clock; right-click its icon to exit.", hotkeyText);
    mk(L"STATIC", line, 0, 20, 262, 400, 52, 0, g_font);

    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_TRAY;
    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    setTray(NIM_ADD);
    SetTimer(g_wnd, TIMER_WATCH, 500, nullptr);

    updateUi(L"The replay buffer is off. Nothing is being recorded.");
    if (g_alwaysOn) startWorker();
    if (!startHidden) showWindow();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (!g_exiting) stopWorker();
    if (mutex) CloseHandle(mutex);
    return 0;
}

} // namespace ctl
