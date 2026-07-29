#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <string>
#include <utility>

namespace {

constexpr wchar_t kWindowClass[] = L"TmpTimeOverlayWindow";
constexpr COLORREF kTransparentColor = RGB(1, 2, 3);

HMODULE g_module = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_overlay_thread = nullptr;
std::atomic<bool> g_started{false};

struct OverlayConfig {
    int font_size = 16;
    int bottom_margin = 12;
    COLORREF text_color = RGB(255, 255, 255);
    COLORREF shadow_color = RGB(0, 0, 0);
    bool hide_when_unfocused = true;
    bool show_prefix = true;
    std::wstring font_name = L"Segoe UI";
};

struct OverlayState {
    HWND overlay = nullptr;
    HWND game = nullptr;
    HFONT font = nullptr;
    OverlayConfig config;
    wchar_t time_text[128]{};
    int last_second = -1;
};

OverlayState g_overlay;

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L".";
    }

    wchar_t* separator = wcsrchr(path, L'\\');
    if (separator != nullptr) {
        *separator = L'\0';
    }
    return path;
}

COLORREF ReadColor(const std::wstring& ini_path, const wchar_t* key, COLORREF fallback) {
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"overlay", key, L"", value, 32, ini_path.c_str());
    if (value[0] == L'\0') {
        return fallback;
    }

    const wchar_t* color = value[0] == L'#' ? value + 1 : value;
    wchar_t* end = nullptr;
    const unsigned long rgb = wcstoul(color, &end, 16);
    if (end == color || *end != L'\0' || wcslen(color) != 6) {
        return fallback;
    }

    return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

OverlayConfig LoadConfig() {
    const std::wstring ini_path = ModuleDirectory() + L"\\tmp_time_overlay.ini";
    OverlayConfig config;
    config.font_size = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"overlay", L"font_size", config.font_size, ini_path.c_str())),
        10,
        72);
    config.bottom_margin = std::clamp(
        static_cast<int>(
            GetPrivateProfileIntW(L"overlay", L"bottom_margin", config.bottom_margin, ini_path.c_str())),
        0,
        300);
    config.hide_when_unfocused =
        GetPrivateProfileIntW(L"overlay", L"hide_when_unfocused", 1, ini_path.c_str()) != 0;
    config.show_prefix = GetPrivateProfileIntW(L"overlay", L"show_prefix", 1, ini_path.c_str()) != 0;
    config.text_color = ReadColor(ini_path, L"text_color", config.text_color);
    config.shadow_color = ReadColor(ini_path, L"shadow_color", config.shadow_color);

    wchar_t font_name[LF_FACESIZE]{};
    GetPrivateProfileStringW(
        L"overlay", L"font_name", config.font_name.c_str(), font_name, LF_FACESIZE, ini_path.c_str());
    config.font_name = font_name;
    return config;
}

BOOL CALLBACK FindGameWindowCallback(HWND window, LPARAM parameter) {
    auto* best = reinterpret_cast<std::pair<HWND, long long>*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != GetCurrentProcessId() || window == g_overlay.overlay || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }

    RECT rect{};
    if (!GetClientRect(window, &rect)) {
        return TRUE;
    }
    const long long area = static_cast<long long>(rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > best->second) {
        best->first = window;
        best->second = area;
    }
    return TRUE;
}

HWND FindGameWindow() {
    std::pair<HWND, long long> best{nullptr, 0};
    EnumWindows(FindGameWindowCallback, reinterpret_cast<LPARAM>(&best));
    return best.first;
}

void UpdateTimeText() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    static constexpr const wchar_t* kMonths[] = {
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec",
    };
    const wchar_t* prefix = g_overlay.config.show_prefix ? L"Current Time: " : L"";
    swprintf_s(
        g_overlay.time_text,
        L"%ls%04u-%ls-%02u %02u:%02u:%02u UTC",
        prefix,
        utc.wYear,
        kMonths[std::clamp<int>(utc.wMonth, 1, 12) - 1],
        utc.wDay,
        utc.wHour,
        utc.wMinute,
        utc.wSecond);
    g_overlay.last_second = utc.wSecond;
}

void DrawOverlay(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);

    HBRUSH background = CreateSolidBrush(kTransparentColor);
    FillRect(dc, &client, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);
    const HFONT old_font = static_cast<HFONT>(SelectObject(dc, g_overlay.font));
    RECT text_rect = client;
    text_rect.bottom -= g_overlay.config.bottom_margin;

    RECT shadow_rect = text_rect;
    OffsetRect(&shadow_rect, 1, 1);
    SetTextColor(dc, g_overlay.config.shadow_color);
    DrawTextW(dc, g_overlay.time_text, -1, &shadow_rect, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, g_overlay.config.text_color);
    DrawTextW(dc, g_overlay.time_text, -1, &text_rect, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
    EndPaint(window, &paint);
}

LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_PAINT:
            DrawOverlay(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

void UpdateOverlayPositionAndVisibility() {
    if (!IsWindow(g_overlay.game)) {
        g_overlay.game = FindGameWindow();
    }

    if (!IsWindow(g_overlay.game) || IsIconic(g_overlay.game)) {
        ShowWindow(g_overlay.overlay, SW_HIDE);
        return;
    }

    DWORD foreground_process_id = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process_id);
    if (g_overlay.config.hide_when_unfocused && foreground_process_id != GetCurrentProcessId()) {
        ShowWindow(g_overlay.overlay, SW_HIDE);
        return;
    }

    RECT client{};
    POINT top_left{};
    if (!GetClientRect(g_overlay.game, &client) || !ClientToScreen(g_overlay.game, &top_left)) {
        ShowWindow(g_overlay.overlay, SW_HIDE);
        return;
    }

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    SetWindowPos(
        g_overlay.overlay,
        HWND_TOPMOST,
        top_left.x,
        top_left.y,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

DWORD WINAPI OverlayThread(void*) {
    g_overlay.config = LoadConfig();
    g_overlay.font = CreateFontW(
        -g_overlay.config.font_size,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        g_overlay.config.font_name.c_str());

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = OverlayWindowProc;
    window_class.hInstance = g_module;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    RegisterClassExW(&window_class);

    g_overlay.overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass,
        L"TMP UTC Time Overlay",
        WS_POPUP,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        g_module,
        nullptr);

    if (g_overlay.overlay == nullptr) {
        if (g_overlay.font != nullptr) {
            DeleteObject(g_overlay.font);
            g_overlay.font = nullptr;
        }
        return 1;
    }

    SetLayeredWindowAttributes(g_overlay.overlay, kTransparentColor, 255, LWA_COLORKEY);
    UpdateTimeText();

    MSG message{};
    while (WaitForSingleObject(g_stop_event, 100) == WAIT_TIMEOUT) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        if (utc.wSecond != g_overlay.last_second) {
            UpdateTimeText();
            InvalidateRect(g_overlay.overlay, nullptr, FALSE);
        }
        UpdateOverlayPositionAndVisibility();
    }

    DestroyWindow(g_overlay.overlay);
    g_overlay.overlay = nullptr;
    if (g_overlay.font != nullptr) {
        DeleteObject(g_overlay.font);
        g_overlay.font = nullptr;
    }
    UnregisterClassW(kWindowClass, g_module);
    return 0;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl scs_telemetry_init(unsigned int, const void*) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return 0;
    }

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        g_started = false;
        return -1;
    }

    g_overlay_thread = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    if (g_overlay_thread == nullptr) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        g_started = false;
        return -1;
    }
    return 0;
}

extern "C" __declspec(dllexport) void __cdecl scs_telemetry_shutdown() {
    if (!g_started.exchange(false)) {
        return;
    }

    if (g_stop_event != nullptr) {
        SetEvent(g_stop_event);
    }
    if (g_overlay_thread != nullptr) {
        WaitForSingleObject(g_overlay_thread, 3000);
        CloseHandle(g_overlay_thread);
        g_overlay_thread = nullptr;
    }
    if (g_stop_event != nullptr) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
