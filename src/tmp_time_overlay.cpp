#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>

#include <MinHook.h>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using PresentFunction = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFunction =
    HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using SetCursorPosFunction = BOOL(WINAPI*)(int, int);

HMODULE g_module = nullptr;
std::atomic<bool> g_started{false};
std::atomic<bool> g_shutting_down{false};
std::mutex g_render_mutex;

PresentFunction g_original_present = nullptr;
ResizeBuffersFunction g_original_resize_buffers = nullptr;
SetCursorPosFunction g_original_set_cursor_pos = nullptr;
void* g_present_address = nullptr;
void* g_resize_buffers_address = nullptr;
void* g_set_cursor_pos_address = nullptr;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_render_target = nullptr;
HWND g_game_window = nullptr;
WNDPROC g_original_window_proc = nullptr;
bool g_imgui_ready = false;
bool g_panel_visible = false;
std::atomic<bool> g_panel_captures_mouse{false};
bool g_hotkey_was_down = false;
bool g_unsaved_changes = false;

struct OverlayConfig {
    int font_size = 16;
    int bottom_margin = 12;
    int horizontal_percent = 50;
    float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool show_prefix = true;
};

OverlayConfig g_config;
OverlayConfig g_edit_config;

std::wstring ConfigPath() {
    wchar_t module_path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(g_module, module_path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"tmp_time_overlay.ini";
    }
    wchar_t* separator = wcsrchr(module_path, L'\\');
    if (separator != nullptr) {
        separator[1] = L'\0';
    }
    return std::wstring(module_path) + L"tmp_time_overlay.ini";
}

COLORREF ReadColor(const std::wstring& path, COLORREF fallback) {
    wchar_t value[16]{};
    GetPrivateProfileStringW(L"overlay", L"text_color", L"", value, 16, path.c_str());
    const wchar_t* color = value[0] == L'#' ? value + 1 : value;
    wchar_t* end = nullptr;
    const unsigned long rgb = wcstoul(color, &end, 16);
    if (wcslen(color) != 6 || end == color || *end != L'\0') {
        return fallback;
    }
    return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

OverlayConfig LoadConfig(const std::wstring& path) {
    OverlayConfig config;
    config.font_size = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"overlay", L"font_size", config.font_size, path.c_str())),
        10,
        72);
    config.bottom_margin = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"overlay", L"bottom_margin", config.bottom_margin, path.c_str())),
        0,
        300);
    config.horizontal_percent = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"overlay", L"horizontal_percent", config.horizontal_percent, path.c_str())),
        0,
        100);
    config.show_prefix =
        GetPrivateProfileIntW(L"overlay", L"show_prefix", 1, path.c_str()) != 0;

    const COLORREF color = ReadColor(path, RGB(255, 255, 255));
    config.text_color[0] = static_cast<float>(GetRValue(color)) / 255.0f;
    config.text_color[1] = static_cast<float>(GetGValue(color)) / 255.0f;
    config.text_color[2] = static_cast<float>(GetBValue(color)) / 255.0f;
    return config;
}

bool SaveConfig(const OverlayConfig& config, const std::wstring& path) {
    wchar_t value[32]{};
    wchar_t color_value[16]{};
    auto write_int = [&](const wchar_t* key, int number) {
        swprintf_s(value, L"%d", number);
        return WritePrivateProfileStringW(L"overlay", key, value, path.c_str()) != FALSE;
    };

    const int red = static_cast<int>(config.text_color[0] * 255.0f + 0.5f);
    const int green = static_cast<int>(config.text_color[1] * 255.0f + 0.5f);
    const int blue = static_cast<int>(config.text_color[2] * 255.0f + 0.5f);
    swprintf_s(color_value, L"%02X%02X%02X", red, green, blue);

    const bool saved = write_int(L"font_size", config.font_size) &&
                       write_int(L"bottom_margin", config.bottom_margin) &&
                       write_int(L"horizontal_percent", config.horizontal_percent) &&
                       WritePrivateProfileStringW(
                           L"overlay", L"text_color", color_value, path.c_str()) != FALSE &&
                       WritePrivateProfileStringW(
                           L"overlay",
                           L"show_prefix",
                           config.show_prefix ? L"1" : L"0",
                           path.c_str()) != FALSE;
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return saved;
}

std::string CurrentUtcText() {
    SYSTEMTIME utc{};
    FILETIME precise_time{};
    GetSystemTimePreciseAsFileTime(&precise_time);
    FileTimeToSystemTime(&precise_time, &utc);
    static constexpr const char* months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    char value[128]{};
    std::snprintf(
        value,
        sizeof(value),
        "%s%04u-%s-%02u %02u:%02u:%02u UTC",
        g_config.show_prefix ? "Current Time: " : "",
        utc.wYear,
        months[std::clamp<int>(utc.wMonth, 1, 12) - 1],
        utc.wDay,
        utc.wHour,
        utc.wMinute,
        utc.wSecond);
    return value;
}

void ReleaseRenderTarget() {
    if (g_render_target != nullptr) {
        g_render_target->Release();
        g_render_target = nullptr;
    }
}

bool CreateRenderTarget(IDXGISwapChain* swap_chain) {
    ReleaseRenderTarget();
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        return false;
    }
    const HRESULT result = g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
    back_buffer->Release();
    return SUCCEEDED(result);
}

void ApplyImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(9.0f, 5.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.98f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.07f, 0.08f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.30f, 0.35f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.33f, 0.38f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.36f, 0.39f, 0.45f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.24f, 0.70f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.70f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.38f, 0.80f, 1.00f, 1.00f);
}

bool IsMouseMessage(UINT message) {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_SETCURSOR;
}

bool IsKeyboardMessage(UINT message) {
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_CHAR ||
           message == WM_SYSCHAR || message == WM_UNICHAR;
}

BOOL WINAPI HookedSetCursorPos(int x, int y) {
    if (g_panel_captures_mouse.load()) {
        return TRUE;
    }
    return g_original_set_cursor_pos(x, y);
}

LRESULT CALLBACK HookedWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_imgui_ready && g_panel_visible) {
        ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        const ImGuiIO& io = ImGui::GetIO();
        if ((IsMouseMessage(message) && io.WantCaptureMouse) ||
            (IsKeyboardMessage(message) && io.WantCaptureKeyboard)) {
            return 1;
        }
    }
    return CallWindowProcW(g_original_window_proc, window, message, wparam, lparam);
}

bool InitializeImGui(IDXGISwapChain* swap_chain) {
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swap_chain->GetDesc(&description))) {
        return false;
    }
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&g_device)))) {
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    g_game_window = description.OutputWindow;
    if (!CreateRenderTarget(swap_chain)) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    const char* font_path = "C:\\Windows\\Fonts\\msyh.ttc";
    ImFont* font = io.Fonts->AddFontFromFileTTF(font_path, 18.0f);
    if (font == nullptr) {
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    }
    if (font != nullptr) {
        io.FontDefault = font;
    }

    ApplyImGuiStyle();
    if (!ImGui_ImplWin32_Init(g_game_window) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        return false;
    }
    g_original_window_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWindowProc)));
    g_imgui_ready = g_original_window_proc != nullptr;
    return g_imgui_ready;
}

void DrawTime() {
    const std::string text = CurrentUtcText();
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float font_size = static_cast<float>(g_config.font_size);
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str());
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float anchor_x = display.x * static_cast<float>(g_config.horizontal_percent) / 100.0f;
    const ImVec2 position(
        std::round(anchor_x - text_size.x * 0.5f),
        std::round(display.y - static_cast<float>(g_config.bottom_margin) - text_size.y));
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(
        g_config.text_color[0],
        g_config.text_color[1],
        g_config.text_color[2],
        g_config.text_color[3]));
    draw_list->AddText(font, font_size, position, color, text.c_str());
}

void DrawSettingsPanel() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 320.0f), ImVec2(520.0f, 520.0f));
    if (!ImGui::Begin("TMP 时间显示设置", &g_panel_visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Ctrl+F9 打开或关闭面板");
    ImGui::Separator();
    g_unsaved_changes |= ImGui::SliderInt("字体大小", &g_edit_config.font_size, 10, 72, "%d px");
    g_unsaved_changes |=
        ImGui::SliderInt("底部距离", &g_edit_config.bottom_margin, 0, 300, "%d px");
    g_unsaved_changes |=
        ImGui::SliderInt("水平位置", &g_edit_config.horizontal_percent, 0, 100, "%d%%");
    g_unsaved_changes |= ImGui::ColorEdit3("文字颜色", g_edit_config.text_color);
    g_unsaved_changes |= ImGui::Checkbox("显示 Current Time 前缀", &g_edit_config.show_prefix);

    ImGui::Spacing();
    if (ImGui::Button("恢复默认")) {
        g_edit_config = OverlayConfig{};
        g_unsaved_changes = true;
    }
    ImGui::SameLine();

    if (g_unsaved_changes) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.25f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.35f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.18f, 0.08f, 1.0f));
    }
    ImGui::BeginDisabled(!g_unsaved_changes);
    if (ImGui::Button("应用并保存")) {
        g_config = g_edit_config;
        if (SaveConfig(g_config, ConfigPath())) {
            g_unsaved_changes = false;
        }
    }
    ImGui::EndDisabled();
    if (g_unsaved_changes) {
        ImGui::PopStyleColor(3);
    }
    ImGui::SameLine();
    if (ImGui::Button("关闭")) {
        g_panel_visible = false;
    }

    ImGui::End();
}

void HandlePanelHotkey() {
    const bool pressed =
        (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (pressed && !g_hotkey_was_down) {
        g_panel_visible = !g_panel_visible;
        if (g_panel_visible) {
            g_edit_config = g_config;
            g_unsaved_changes = false;
            ReleaseCapture();
            ClipCursor(nullptr);
        }
        g_panel_captures_mouse.store(g_panel_visible);
        ImGui::GetIO().MouseDrawCursor = g_panel_visible;
    }
    g_hotkey_was_down = pressed;
}

HRESULT __stdcall HookedPresent(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    if (g_shutting_down.load()) {
        return g_original_present(swap_chain, sync_interval, flags);
    }

    std::lock_guard<std::mutex> lock(g_render_mutex);
    if (!g_imgui_ready && !InitializeImGui(swap_chain)) {
        return g_original_present(swap_chain, sync_interval, flags);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    HandlePanelHotkey();
    DrawTime();
    if (g_panel_visible) {
        DrawSettingsPanel();
    }
    g_panel_captures_mouse.store(g_panel_visible);
    ImGui::GetIO().MouseDrawCursor = g_panel_visible;
    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    return g_original_present(swap_chain, sync_interval, flags);
}

HRESULT __stdcall HookedResizeBuffers(
    IDXGISwapChain* swap_chain,
    UINT buffer_count,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    UINT swap_chain_flags) {
    std::lock_guard<std::mutex> lock(g_render_mutex);
    if (g_imgui_ready) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ReleaseRenderTarget();
    }
    const HRESULT result = g_original_resize_buffers(
        swap_chain, buffer_count, width, height, format, swap_chain_flags);
    if (SUCCEEDED(result) && g_imgui_ready) {
        CreateRenderTarget(swap_chain);
        ImGui_ImplDX11_CreateDeviceObjects();
    }
    return result;
}

LRESULT CALLBACK DummyWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

bool InstallHooks() {
    constexpr wchar_t class_name[] = L"TmpTimeOverlayDx11Probe";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DummyWindowProc;
    window_class.hInstance = g_module;
    window_class.lpszClassName = class_name;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    HWND window = CreateWindowExW(
        0, class_name, L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, g_module, nullptr);
    if (window == nullptr) {
        UnregisterClassW(class_name, g_module);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 1;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap_chain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL feature_level{};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &description,
        &swap_chain,
        &device,
        &feature_level,
        &context);
    if (FAILED(result)) {
        DestroyWindow(window);
        UnregisterClassW(class_name, g_module);
        return false;
    }

    void** virtual_table = *reinterpret_cast<void***>(swap_chain);
    g_present_address = virtual_table[8];
    g_resize_buffers_address = virtual_table[13];
    g_set_cursor_pos_address = reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetCursorPos"));

    context->Release();
    device->Release();
    swap_chain->Release();
    DestroyWindow(window);
    UnregisterClassW(class_name, g_module);

    if (MH_Initialize() != MH_OK) {
        return false;
    }
    if (g_set_cursor_pos_address == nullptr ||
        MH_CreateHook(
            g_present_address,
            reinterpret_cast<void*>(HookedPresent),
            reinterpret_cast<void**>(&g_original_present)) != MH_OK ||
        MH_CreateHook(
            g_resize_buffers_address,
            reinterpret_cast<void*>(HookedResizeBuffers),
            reinterpret_cast<void**>(&g_original_resize_buffers)) != MH_OK ||
        MH_CreateHook(
            g_set_cursor_pos_address,
            reinterpret_cast<void*>(HookedSetCursorPos),
            reinterpret_cast<void**>(&g_original_set_cursor_pos)) != MH_OK) {
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        return false;
    }
    return true;
}

void ShutdownHooks() {
    g_shutting_down = true;
    g_panel_captures_mouse = false;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    std::lock_guard<std::mutex> lock(g_render_mutex);
    if (g_imgui_ready) {
        if (g_original_window_proc != nullptr && IsWindow(g_game_window)) {
            SetWindowLongPtrW(
                g_game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_window_proc));
        }
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui_ready = false;
    }
    ReleaseRenderTarget();
    if (g_context != nullptr) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
    g_game_window = nullptr;
    g_original_window_proc = nullptr;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl scs_telemetry_init(unsigned int, const void*) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return 0;
    }
    g_shutting_down = false;
    const std::wstring config_path = ConfigPath();
    const bool config_exists =
        GetFileAttributesW(config_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    g_config = LoadConfig(config_path);
    if (!config_exists) {
        SaveConfig(g_config, config_path);
    }
    g_edit_config = g_config;
    if (!InstallHooks()) {
        g_started = false;
        return -1;
    }
    return 0;
}

extern "C" __declspec(dllexport) void __cdecl scs_telemetry_shutdown() {
    if (!g_started.exchange(false)) {
        return;
    }
    ShutdownHooks();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
