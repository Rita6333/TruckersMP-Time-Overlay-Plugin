#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <string>
#include <utility>

namespace {

constexpr wchar_t kWindowClass[] = L"TmpTimeOverlayWindow";
constexpr wchar_t kEditorWindowClass[] = L"TmpTimeOverlayEditorWindow";
constexpr COLORREF kTransparentColor = RGB(1, 2, 3);
constexpr int kEditorWidth = 390;
constexpr int kEditorHeight = 430;

enum EditorControlId {
    kFontValue = 100,
    kFontDecrease,
    kFontIncrease,
    kBottomValue,
    kBottomDecrease,
    kBottomIncrease,
    kHorizontalValue,
    kHorizontalDecrease,
    kHorizontalIncrease,
    kTextColor,
    kShowPrefix,
    kHideUnfocused,
    kResetDefaults,
    kSaveAndClose,
};

HMODULE g_module = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_overlay_thread = nullptr;
std::atomic<bool> g_started{false};
std::atomic<bool> g_toggle_editor_requested{false};

struct OverlayConfig {
    int font_size = 16;
    int bottom_margin = 12;
    int horizontal_percent = 50;
    COLORREF text_color = RGB(255, 255, 255);
    bool hide_when_unfocused = true;
    bool show_prefix = true;
    std::wstring font_name = L"Segoe UI";
};

struct OverlayState {
    HWND overlay = nullptr;
    HWND editor = nullptr;
    HWND game = nullptr;
    HFONT font = nullptr;
    HFONT editor_font = nullptr;
    HBRUSH editor_background = nullptr;
    OverlayConfig config;
    wchar_t time_text[128]{};
    int last_second = -1;
    bool editor_visible = false;
    bool hotkey_was_down = false;
};

OverlayState g_overlay;

void UpdateTimeText();

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

std::wstring IniPath() {
    return ModuleDirectory() + L"\\tmp_time_overlay.ini";
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
    const std::wstring ini_path = IniPath();
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
    config.horizontal_percent = std::clamp(
        static_cast<int>(
            GetPrivateProfileIntW(L"overlay", L"horizontal_percent", config.horizontal_percent, ini_path.c_str())),
        0,
        100);
    config.hide_when_unfocused =
        GetPrivateProfileIntW(L"overlay", L"hide_when_unfocused", 1, ini_path.c_str()) != 0;
    config.show_prefix = GetPrivateProfileIntW(L"overlay", L"show_prefix", 1, ini_path.c_str()) != 0;
    config.text_color = ReadColor(ini_path, L"text_color", config.text_color);

    wchar_t font_name[LF_FACESIZE]{};
    GetPrivateProfileStringW(
        L"overlay", L"font_name", config.font_name.c_str(), font_name, LF_FACESIZE, ini_path.c_str());
    config.font_name = font_name;
    return config;
}

void SaveConfig() {
    const std::wstring ini_path = IniPath();
    wchar_t value[64]{};
    auto write_int = [&](const wchar_t* key, int number) {
        swprintf_s(value, L"%d", number);
        WritePrivateProfileStringW(L"overlay", key, value, ini_path.c_str());
    };
    auto write_bool = [&](const wchar_t* key, bool enabled) {
        WritePrivateProfileStringW(L"overlay", key, enabled ? L"1" : L"0", ini_path.c_str());
    };
    auto write_color = [&](const wchar_t* key, COLORREF color) {
        swprintf_s(value, L"%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
        WritePrivateProfileStringW(L"overlay", key, value, ini_path.c_str());
    };

    write_int(L"font_size", g_overlay.config.font_size);
    write_int(L"bottom_margin", g_overlay.config.bottom_margin);
    write_int(L"horizontal_percent", g_overlay.config.horizontal_percent);
    WritePrivateProfileStringW(L"overlay", L"font_name", g_overlay.config.font_name.c_str(), ini_path.c_str());
    write_color(L"text_color", g_overlay.config.text_color);
    write_bool(L"hide_when_unfocused", g_overlay.config.hide_when_unfocused);
    write_bool(L"show_prefix", g_overlay.config.show_prefix);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini_path.c_str());
}

void RecreateOverlayFont() {
    HFONT replacement = CreateFontW(
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
    if (replacement != nullptr) {
        if (g_overlay.font != nullptr) {
            DeleteObject(g_overlay.font);
        }
        g_overlay.font = replacement;
    }
}

HWND EditorControl(int id) {
    return g_overlay.editor == nullptr ? nullptr : GetDlgItem(g_overlay.editor, id);
}

void SetControlFont(HWND control) {
    if (control != nullptr && g_overlay.editor_font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_overlay.editor_font), TRUE);
    }
}

HWND CreateEditorControl(
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id) {
    HWND control = CreateWindowExW(
        0,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        g_overlay.editor,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        g_module,
        nullptr);
    SetControlFont(control);
    return control;
}

void UpdateEditorControls() {
    if (!IsWindow(g_overlay.editor)) {
        return;
    }

    wchar_t value[32]{};
    swprintf_s(value, L"%d px", g_overlay.config.font_size);
    SetWindowTextW(EditorControl(kFontValue), value);
    swprintf_s(value, L"%d px", g_overlay.config.bottom_margin);
    SetWindowTextW(EditorControl(kBottomValue), value);
    swprintf_s(value, L"%d%%", g_overlay.config.horizontal_percent);
    SetWindowTextW(EditorControl(kHorizontalValue), value);
    SendMessageW(
        EditorControl(kShowPrefix),
        BM_SETCHECK,
        g_overlay.config.show_prefix ? BST_CHECKED : BST_UNCHECKED,
        0);
    SendMessageW(
        EditorControl(kHideUnfocused),
        BM_SETCHECK,
        g_overlay.config.hide_when_unfocused ? BST_CHECKED : BST_UNCHECKED,
        0);
}

void ApplyEditorChanges() {
    UpdateTimeText();
    RecreateOverlayFont();
    UpdateEditorControls();
    InvalidateRect(g_overlay.overlay, nullptr, FALSE);
    InvalidateRect(g_overlay.editor, nullptr, TRUE);
}

bool ChooseOverlayColor(COLORREF& color) {
    static COLORREF custom_colors[16]{};
    CHOOSECOLORW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = g_overlay.editor;
    picker.rgbResult = color;
    picker.lpCustColors = custom_colors;
    picker.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&picker)) {
        return false;
    }
    color = picker.rgbResult;
    return true;
}

BOOL CALLBACK FindGameWindowCallback(HWND window, LPARAM parameter) {
    auto* best = reinterpret_cast<std::pair<HWND, long long>*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != GetCurrentProcessId() || window == g_overlay.overlay || window == g_overlay.editor ||
        !IsWindowVisible(window) ||
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
    SIZE text_size{};
    GetTextExtentPoint32W(dc, g_overlay.time_text, static_cast<int>(wcslen(g_overlay.time_text)), &text_size);
    const int target_x =
        client.left + ((client.right - client.left) * g_overlay.config.horizontal_percent / 100);
    RECT text_rect = client;
    text_rect.left = target_x - (text_size.cx / 2);
    text_rect.right = text_rect.left + text_size.cx;
    text_rect.bottom -= g_overlay.config.bottom_margin;

    SetTextColor(dc, g_overlay.config.text_color);
    DrawTextW(dc, g_overlay.time_text, -1, &text_rect, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
    EndPaint(window, &paint);
}

void DrawEditor(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);

    HBRUSH background = CreateSolidBrush(RGB(29, 32, 38));
    FillRect(dc, &client, background);
    DeleteObject(background);

    RECT header{0, 0, client.right, 50};
    HBRUSH header_brush = CreateSolidBrush(RGB(19, 21, 26));
    FillRect(dc, &header, header_brush);
    DeleteObject(header_brush);

    const HFONT old_font = static_cast<HFONT>(SelectObject(dc, g_overlay.editor_font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(242, 244, 247));
    RECT title{18, 0, client.right - 18, 50};
    DrawTextW(dc, L"TMP 时间显示设置", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, RGB(194, 199, 208));
    const wchar_t* labels[] = {L"字体大小", L"底部距离", L"水平位置"};
    const int label_y[] = {74, 126, 178};
    for (int index = 0; index < 3; ++index) {
        RECT label{20, label_y[index], 190, label_y[index] + 30};
        DrawTextW(dc, labels[index], -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    RECT color_label{20, 232, 175, 262};
    DrawTextW(dc, L"文字颜色", -1, &color_label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
    EndPaint(window, &paint);
}

void ToggleEditor(bool show) {
    if (!IsWindow(g_overlay.editor)) {
        return;
    }
    g_overlay.editor_visible = show;
    if (!show) {
        SaveConfig();
        ShowWindow(g_overlay.editor, SW_HIDE);
        if (IsWindow(g_overlay.game)) {
            SetForegroundWindow(g_overlay.game);
        }
        return;
    }

    UpdateEditorControls();
    RECT game_rect{};
    GetWindowRect(g_overlay.game, &game_rect);
    const int x = std::max(game_rect.left + 12, game_rect.right - kEditorWidth - 24);
    const int centered_y =
        static_cast<int>((game_rect.bottom - game_rect.top - kEditorHeight) / 2);
    const int y = game_rect.top + std::max(24, centered_y);
    SetWindowPos(g_overlay.editor, HWND_TOPMOST, x, y, kEditorWidth, kEditorHeight, SWP_SHOWWINDOW);
    SetForegroundWindow(g_overlay.editor);
}

LRESULT CALLBACK EditorWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            g_overlay.editor = window;
            CreateEditorControl(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, 190, 74, 70, 30, kFontValue);
            CreateEditorControl(L"BUTTON", L"-", BS_PUSHBUTTON, 268, 74, 42, 30, kFontDecrease);
            CreateEditorControl(L"BUTTON", L"+", BS_PUSHBUTTON, 318, 74, 42, 30, kFontIncrease);
            CreateEditorControl(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, 190, 126, 70, 30, kBottomValue);
            CreateEditorControl(L"BUTTON", L"-", BS_PUSHBUTTON, 268, 126, 42, 30, kBottomDecrease);
            CreateEditorControl(L"BUTTON", L"+", BS_PUSHBUTTON, 318, 126, 42, 30, kBottomIncrease);
            CreateEditorControl(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, 190, 178, 70, 30, kHorizontalValue);
            CreateEditorControl(L"BUTTON", L"-", BS_PUSHBUTTON, 268, 178, 42, 30, kHorizontalDecrease);
            CreateEditorControl(L"BUTTON", L"+", BS_PUSHBUTTON, 318, 178, 42, 30, kHorizontalIncrease);
            CreateEditorControl(L"BUTTON", L"选择颜色", BS_PUSHBUTTON, 250, 232, 110, 30, kTextColor);
            CreateEditorControl(L"BUTTON", L"显示 Current Time 前缀", BS_AUTOCHECKBOX, 20, 280, 260, 26, kShowPrefix);
            CreateEditorControl(L"BUTTON", L"切出游戏时隐藏", BS_AUTOCHECKBOX, 20, 312, 240, 26, kHideUnfocused);
            CreateEditorControl(L"BUTTON", L"恢复默认", BS_PUSHBUTTON, 20, 366, 110, 34, kResetDefaults);
            CreateEditorControl(L"BUTTON", L"保存并关闭", BS_DEFPUSHBUTTON, 236, 366, 124, 34, kSaveAndClose);
            UpdateEditorControls();
            return 0;
        }
        case WM_PAINT:
            DrawEditor(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST: {
            const LRESULT hit = DefWindowProcW(window, message, wparam, lparam);
            if (hit == HTCLIENT) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(window, &point);
                if (point.y < 50) {
                    return HTCAPTION;
                }
            }
            return hit;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, RGB(242, 244, 247));
            SetBkColor(dc, RGB(29, 32, 38));
            return reinterpret_cast<LRESULT>(g_overlay.editor_background);
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            switch (id) {
                case kFontDecrease:
                    g_overlay.config.font_size = std::max(10, g_overlay.config.font_size - 1);
                    break;
                case kFontIncrease:
                    g_overlay.config.font_size = std::min(72, g_overlay.config.font_size + 1);
                    break;
                case kBottomDecrease:
                    g_overlay.config.bottom_margin = std::max(0, g_overlay.config.bottom_margin - 4);
                    break;
                case kBottomIncrease:
                    g_overlay.config.bottom_margin = std::min(300, g_overlay.config.bottom_margin + 4);
                    break;
                case kHorizontalDecrease:
                    g_overlay.config.horizontal_percent = std::max(0, g_overlay.config.horizontal_percent - 5);
                    break;
                case kHorizontalIncrease:
                    g_overlay.config.horizontal_percent = std::min(100, g_overlay.config.horizontal_percent + 5);
                    break;
                case kTextColor:
                    ChooseOverlayColor(g_overlay.config.text_color);
                    break;
                case kShowPrefix:
                    g_overlay.config.show_prefix =
                        SendMessageW(EditorControl(kShowPrefix), BM_GETCHECK, 0, 0) == BST_CHECKED;
                    break;
                case kHideUnfocused:
                    g_overlay.config.hide_when_unfocused =
                        SendMessageW(EditorControl(kHideUnfocused), BM_GETCHECK, 0, 0) == BST_CHECKED;
                    break;
                case kResetDefaults:
                    g_overlay.config = OverlayConfig{};
                    break;
                case kSaveAndClose:
                    ToggleEditor(false);
                    return 0;
                default:
                    return DefWindowProcW(window, message, wparam, lparam);
            }
            ApplyEditorChanges();
            return 0;
        }
        case WM_HOTKEY:
            ToggleEditor(!g_overlay.editor_visible);
            return 0;
        case WM_CLOSE:
            ToggleEditor(false);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
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
    if (g_overlay.editor_visible && IsWindow(g_overlay.editor)) {
        SetWindowPos(
            g_overlay.editor,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

DWORD WINAPI OverlayThread(void*) {
    g_overlay.config = LoadConfig();
    RecreateOverlayFont();
    g_overlay.editor_font = CreateFontW(
        -16,
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
        L"Microsoft YaHei UI");
    g_overlay.editor_background = CreateSolidBrush(RGB(29, 32, 38));

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = OverlayWindowProc;
    window_class.hInstance = g_module;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    RegisterClassExW(&window_class);

    WNDCLASSEXW editor_class{};
    editor_class.cbSize = sizeof(editor_class);
    editor_class.lpfnWndProc = EditorWindowProc;
    editor_class.hInstance = g_module;
    editor_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    editor_class.lpszClassName = kEditorWindowClass;
    RegisterClassExW(&editor_class);

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

    g_overlay.editor = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kEditorWindowClass,
        L"TMP 时间显示设置",
        WS_POPUP,
        0,
        0,
        kEditorWidth,
        kEditorHeight,
        nullptr,
        nullptr,
        g_module,
        nullptr);
    if (g_overlay.editor == nullptr) {
        DestroyWindow(g_overlay.overlay);
        g_overlay.overlay = nullptr;
        if (g_overlay.font != nullptr) {
            DeleteObject(g_overlay.font);
            g_overlay.font = nullptr;
        }
        UnregisterClassW(kEditorWindowClass, g_module);
        UnregisterClassW(kWindowClass, g_module);
        return 1;
    }
    RegisterHotKey(g_overlay.editor, 1, MOD_CONTROL | MOD_NOREPEAT, VK_F10);

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

    SaveConfig();
    if (IsWindow(g_overlay.editor)) {
        UnregisterHotKey(g_overlay.editor, 1);
        DestroyWindow(g_overlay.editor);
        g_overlay.editor = nullptr;
    }
    DestroyWindow(g_overlay.overlay);
    g_overlay.overlay = nullptr;
    if (g_overlay.font != nullptr) {
        DeleteObject(g_overlay.font);
        g_overlay.font = nullptr;
    }
    if (g_overlay.editor_font != nullptr) {
        DeleteObject(g_overlay.editor_font);
        g_overlay.editor_font = nullptr;
    }
    if (g_overlay.editor_background != nullptr) {
        DeleteObject(g_overlay.editor_background);
        g_overlay.editor_background = nullptr;
    }
    UnregisterClassW(kEditorWindowClass, g_module);
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
