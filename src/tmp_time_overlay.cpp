// TruckersMP Time Overlay Plugin
//
// An in-game clock overlay for Euro Truck Simulator 2 / American Truck Simulator
// built on the TruckersMP Client SDK.
//
//   https://docs.truckersmp.com/client-sdk/getting-started
//
// The plugin exports the two entry points the TruckersMP client looks for
// (truckersmp_init / truckersmp_shutdown) and draws its interface through the
// SDK's render module:
//
//   * Render().OnPostRender  - frame boundary, the only place we touch the device
//   * Input()                - the F8 hotkey
//   * Network()              - connection state, used to tell driving from menus
//   * UserInterface()        - in-game notifications
//   * Core()                 - logging
//
// It also keeps the SCS telemetry entry points so the clock can still tell
// "driving" from "menu / paused" when the game offers that information. The two
// SDKs are designed to co-exist in a single DLL; when the game does not provide
// the telemetry events the plugin falls back to the TruckersMP connection state.
//
// ---------------------------------------------------------------------------
// There is no in-game settings panel
//
// Everything is configured through tmp_time_overlay.ini next to the DLL; the
// file is created with the defaults on first load. The only hotkey is F8, which
// shows or hides the clock and writes the choice back to the ini.
//
// The panel was removed on purpose. Drawing one meant taking the client's input
// capture (mouse reference + game mouse/keyboard locks) every time it opened,
// which is a lot of moving parts for a feature nobody needed in the middle of a
// drive. Without it the plugin never touches the client's input state at all.
// ---------------------------------------------------------------------------
//
// ---------------------------------------------------------------------------
// Performance notes
//
// The overlay runs inside the game's frame, so everything here is written to
// stay off the critical path:
//
//   * When the clock is off screen the whole ImGui frame is skipped - no
//     NewFrame/Render, no D3D11 state backup and restore.
//   * No input event is ever pushed into ImGui; the interface is draw-only.
//   * Backend initialisation (font load, shader compilation) runs outside every
//     lock so a one-off hitch cannot block the game thread.
//   * The D3D11 calls in ImGui_ImplDX11_RenderDrawData run with no lock held.
//   * SDK getters (renderer, device handle, connection state), the back buffer
//     size and the UTC offset are cached instead of queried every frame.
//   * No heap allocation and no time zone lookup happen per frame.
// ---------------------------------------------------------------------------

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

#include <TruckersMP/TruckersMP.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>

namespace {

using TruckersMP::LogLevel;
using TruckersMP::NotificationType;
using TruckersMP::RendererID;
using TruckersMP::Uint64;

constexpr const char* kPluginName = "TMP Time Overlay";
constexpr const char* kPluginAuthor = "TruckersMP Time Overlay Plugin";
constexpr const char* kPluginVersion = "2.1.0";
constexpr const char* kPluginDescription = "Real-time clock overlay for Euro Truck Simulator 2.";

// How often the client is asked for state that changes slowly.
constexpr ULONGLONG kNetworkRefreshMs = 250;
constexpr ULONGLONG kTimeZoneRefreshMs = 30000;
constexpr int kRenderDeviceRecheckFrames = 600;

// ---------------------------------------------------------------------------
// Minimal SCS Telemetry ABI.
//
// Keeping these local avoids requiring users to install the SCS SDK headers
// while preserving the official x64 structure layout.
// ---------------------------------------------------------------------------

using ScsResult = std::int32_t;
using ScsEvent = std::uint32_t;
using ScsContext = void*;
using ScsEventCallback = void(__stdcall*)(ScsEvent, const void*, ScsContext);
using ScsRegisterEvent = ScsResult(__stdcall*)(ScsEvent, ScsEventCallback, ScsContext);
using ScsUnregisterEvent = ScsResult(__stdcall*)(ScsEvent);

struct ScsTelemetryInitParamsV101 {
    const char* game_name;
    const char* game_id;
    std::uint32_t game_version;
    std::uint32_t padding;
    void(__stdcall* log)(std::int32_t, const char*);
    ScsRegisterEvent register_for_event;
    ScsUnregisterEvent unregister_from_event;
    void* register_for_channel;
    void* unregister_from_channel;
};

static_assert(sizeof(ScsTelemetryInitParamsV101) == 64);
constexpr std::uint32_t kScsTelemetryVersion100 = (1u << 16) | 0u;
constexpr std::uint32_t kScsTelemetryVersion101 = (1u << 16) | 1u;
constexpr ScsEvent kScsTelemetryEventPaused = 3;
constexpr ScsEvent kScsTelemetryEventStarted = 4;
constexpr ScsResult kScsResultOk = 0;
constexpr ScsResult kScsResultUnsupported = -1;
constexpr ScsResult kScsResultInvalidParameter = -2;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

HMODULE g_module = nullptr;
std::unique_ptr<TruckersMP::Session> g_session;

// Guards the ImGui context: frames and the font atlas. Never held across a
// D3D11 call, so the game thread can never stall behind the renderer.
std::mutex g_imgui_mutex;
// Guards g_config. Held for short CPU-only sections.
std::mutex g_config_mutex;
// Serialises backend initialisation and teardown.
std::mutex g_init_mutex;

std::atomic<bool> g_shutting_down{false};
std::atomic<bool> g_imgui_ready{false};

// SCS reports "started" while driving and "paused" for menus and loading screens.
std::atomic<bool> g_scs_in_game{false};
// Set when the game accepted our SCS event registrations.
std::atomic<bool> g_scs_ready{false};
ScsUnregisterEvent g_scs_unregister = nullptr;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
bool g_frame_timer_ready = false;
LARGE_INTEGER g_frame_frequency{};
LARGE_INTEGER g_frame_last{};

// Render-thread only.
float g_time_animation = 0.0f;

// Cached render target description: the back buffer only changes on a resize.
ID3D11RenderTargetView* g_cached_target = nullptr;
float g_cached_target_width = 0.0f;
float g_cached_target_height = 0.0f;

// Cached client queries.
RendererID g_cached_renderer = RendererID::OpenGL;
ID3D11Device* g_cached_device = nullptr;
bool g_render_device_ready = false;
int g_render_device_countdown = 0;
struct NetworkCache {
    ULONGLONG refreshed_at = 0;
    bool available = false;
    bool connected = false;
};
NetworkCache g_network_cache;
LONG g_utc_offset_minutes = 0;
ULONGLONG g_utc_offset_refreshed_at = 0;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr int kCustomTextCapacity = 128;
constexpr int kCustomTextLines = 4;
constexpr int kTimeFormatCapacity = 64;
constexpr const char* kDefaultTimeFormat = "%d.%m.%Y %H:%M:%S";

// The first key keeps the name used by earlier releases so existing
// configuration files load without any change.
constexpr const wchar_t* kCustomTextKeys[kCustomTextLines] = {
    L"custom_text",
    L"custom_text_2",
    L"custom_text_3",
    L"custom_text_4"};

struct OverlayConfig {
    int font_size = 16;
    int bottom_margin = 12;
    int horizontal_percent = 50;
    float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool time_visible = true;
    bool show_prefix = true;
    bool show_timezone = true;
    // How many custom text rows are drawn; empty rows are skipped.
    int custom_text_lines = 1;
    char custom_text[kCustomTextLines][kCustomTextCapacity] = {};
    char time_format[kTimeFormatCapacity] = "%d.%m.%Y %H:%M:%S";
};

OverlayConfig g_config;

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

void WideToUtf8(const wchar_t* text, char* buffer, int capacity) {
    if (buffer == nullptr || capacity <= 0) {
        return;
    }
    buffer[0] = '\0';
    if (text == nullptr || text[0] == L'\0') {
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, capacity, nullptr, nullptr);
    buffer[capacity - 1] = '\0';
}

std::wstring Utf8ToWide(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1) {
        return std::wstring();
    }
    std::wstring result(static_cast<std::size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    return result;
}

void ReadConfigText(
    const std::wstring& path,
    const wchar_t* key,
    const wchar_t* fallback,
    char* buffer,
    int capacity) {
    wchar_t value[256]{};
    GetPrivateProfileStringW(L"overlay", key, fallback, value, 256, path.c_str());
    WideToUtf8(value, buffer, capacity);
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
    config.time_visible =
        GetPrivateProfileIntW(L"overlay", L"time_visible", 1, path.c_str()) != 0;
    config.show_prefix =
        GetPrivateProfileIntW(L"overlay", L"show_prefix", 1, path.c_str()) != 0;
    config.show_timezone =
        GetPrivateProfileIntW(L"overlay", L"show_timezone", 1, path.c_str()) != 0;
    config.custom_text_lines = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"overlay", L"custom_text_lines", config.custom_text_lines, path.c_str())),
        1,
        kCustomTextLines);
    for (int line = 0; line < kCustomTextLines; ++line) {
        ReadConfigText(
            path, kCustomTextKeys[line], L"", config.custom_text[line], kCustomTextCapacity);
    }
    ReadConfigText(
        path, L"time_format", L"%d.%m.%Y %H:%M:%S", config.time_format, kTimeFormatCapacity);
    if (config.time_format[0] == '\0') {
        std::snprintf(config.time_format, kTimeFormatCapacity, "%s", kDefaultTimeFormat);
    }

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

    bool saved = write_int(L"font_size", config.font_size) &&
                 write_int(L"bottom_margin", config.bottom_margin) &&
                 write_int(L"horizontal_percent", config.horizontal_percent) &&
                 write_int(L"time_visible", config.time_visible ? 1 : 0) &&
                 write_int(L"show_timezone", config.show_timezone ? 1 : 0) &&
                 write_int(
                     L"custom_text_lines",
                     std::clamp(config.custom_text_lines, 1, kCustomTextLines)) &&
                 WritePrivateProfileStringW(
                     L"overlay", L"text_color", color_value, path.c_str()) != FALSE &&
                 WritePrivateProfileStringW(
                     L"overlay",
                     L"show_prefix",
                     config.show_prefix ? L"1" : L"0",
                     path.c_str()) != FALSE &&
                 WritePrivateProfileStringW(
                     L"overlay",
                     L"time_format",
                     Utf8ToWide(config.time_format).c_str(),
                     path.c_str()) != FALSE;
    for (int line = 0; line < kCustomTextLines; ++line) {
        saved = saved && WritePrivateProfileStringW(
                             L"overlay",
                             kCustomTextKeys[line],
                             Utf8ToWide(config.custom_text[line]).c_str(),
                             path.c_str()) != FALSE;
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return saved;
}

// ---------------------------------------------------------------------------
// Time formatting
// ---------------------------------------------------------------------------

// GetTimeZoneInformation is a surprisingly heavy call, so the offset is cached
// and only refreshed every kTimeZoneRefreshMs.
LONG UtcOffsetMinutes() {
    const ULONGLONG now = GetTickCount64();
    if (g_utc_offset_refreshed_at != 0 &&
        now - g_utc_offset_refreshed_at < kTimeZoneRefreshMs) {
        return g_utc_offset_minutes;
    }
    g_utc_offset_refreshed_at = now;

    TIME_ZONE_INFORMATION time_zone{};
    const DWORD time_zone_id = GetTimeZoneInformation(&time_zone);
    if (time_zone_id == TIME_ZONE_ID_INVALID) {
        return g_utc_offset_minutes;
    }
    LONG bias = time_zone.Bias;
    if (time_zone_id == TIME_ZONE_ID_STANDARD) {
        bias += time_zone.StandardBias;
    } else if (time_zone_id == TIME_ZONE_ID_DAYLIGHT) {
        bias += time_zone.DaylightBias;
    }
    g_utc_offset_minutes = -bias;
    return g_utc_offset_minutes;
}

// Writes the clock line into a caller supplied buffer: no allocation per frame.
void FormatTimeText(const OverlayConfig& config, char* buffer, std::size_t capacity) {
    if (buffer == nullptr || capacity == 0) {
        return;
    }
    buffer[0] = '\0';

    SYSTEMTIME local{};
    FILETIME precise_time{};
    GetSystemTimePreciseAsFileTime(&precise_time);
    FILETIME local_file_time{};
    if (!FileTimeToLocalFileTime(&precise_time, &local_file_time) ||
        !FileTimeToSystemTime(&local_file_time, &local)) {
        GetLocalTime(&local);
    }

    // The format string is user supplied, so a broken pattern falls back to the default one.
    const char* pattern =
        config.time_format[0] != '\0' ? config.time_format : kDefaultTimeFormat;
    std::tm time_parts{};
    time_parts.tm_year = static_cast<int>(local.wYear) - 1900;
    time_parts.tm_mon = static_cast<int>(local.wMonth) - 1;
    time_parts.tm_mday = static_cast<int>(local.wDay);
    time_parts.tm_hour = static_cast<int>(local.wHour);
    time_parts.tm_min = static_cast<int>(local.wMinute);
    time_parts.tm_sec = static_cast<int>(local.wSecond);
    time_parts.tm_wday = static_cast<int>(local.wDayOfWeek);

    char formatted[192]{};
    if (std::strftime(formatted, sizeof(formatted), pattern, &time_parts) == 0) {
        std::snprintf(
            formatted,
            sizeof(formatted),
            "%02u.%02u.%04u %02u:%02u:%02u",
            static_cast<unsigned>(local.wDay),
            static_cast<unsigned>(local.wMonth),
            static_cast<unsigned>(local.wYear),
            static_cast<unsigned>(local.wHour),
            static_cast<unsigned>(local.wMinute),
            static_cast<unsigned>(local.wSecond));
    }

    char suffix[24]{};
    if (config.show_timezone) {
        const LONG offset_minutes = UtcOffsetMinutes();
        const char offset_sign = offset_minutes < 0 ? '-' : '+';
        const LONG absolute_offset = std::abs(offset_minutes);
        std::snprintf(
            suffix,
            sizeof(suffix),
            " (%c%02ld%02ld)",
            offset_sign,
            static_cast<long>(absolute_offset / 60),
            static_cast<long>(absolute_offset % 60));
    }
    std::snprintf(
        buffer,
        capacity,
        "%s%s%s",
        config.show_prefix ? "Current time: " : "",
        formatted,
        suffix);
}

// ---------------------------------------------------------------------------
// Client helpers
// ---------------------------------------------------------------------------

void Log(LogLevel level, const char* text) {
    if (g_session == nullptr) {
        return;
    }
    g_session->Core().LogMessage(level, text);
}

void LogFormat(LogLevel level, const char* format, ...) {
    char line[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    Log(level, line);
}

void Notify(NotificationType type, const char* message) {
    if (g_session == nullptr || !g_session->UserInterface().IsAvailable()) {
        return;
    }
    g_session->UserInterface().ShowNotification(type, message);
}

// ---------------------------------------------------------------------------
// Cached client queries
// ---------------------------------------------------------------------------

// The renderer and the device never change during a session, so they are looked
// up once. A slow re-check covers the rare case of a device recreation.
void RefreshRenderDevice() {
    if (g_render_device_ready && --g_render_device_countdown > 0) {
        return;
    }
    g_render_device_countdown = kRenderDeviceRecheckFrames;
    if (g_session == nullptr || !g_session->Render().IsAvailable()) {
        return;
    }
    const RendererID renderer =
        g_session->Render().GetRendererID().value_or(RendererID::OpenGL);
    if (renderer != RendererID::DirectX11) {
        g_cached_renderer = renderer;
        g_cached_device = nullptr;
        g_render_device_ready = true;
        return;
    }
    const Uint64 handle = g_session->Render().GetDeviceHandle().value_or(0);
    if (handle == 0) {
        // The renderer is not up yet; keep asking on the next frame.
        g_render_device_countdown = 1;
        return;
    }
    g_cached_renderer = renderer;
    g_cached_device = reinterpret_cast<ID3D11Device*>(static_cast<uintptr_t>(handle));
    g_render_device_ready = true;
}

// Only the connection flag is read from the client's network module.
//
// TruckersMP client 0.7.7.6 ships a null-pointer bug in Network().GetLastPing():
// the getter walks an internal session object and dereferences it without a null
// check, so calling it before the client has an active server session - which is
// exactly the situation in the first frames after the game starts - crashes the
// whole game with an access violation inside core_ets2mp.dll.
//
// The two neighbouring getters are fine: IsConnected() only reads a state field
// and GetGameServerID() checks its pointer first. Only the ping getter is broken,
// so it is deliberately never called.
void RefreshNetworkCache() {
    const ULONGLONG now = GetTickCount64();
    if (g_network_cache.refreshed_at != 0 &&
        now - g_network_cache.refreshed_at < kNetworkRefreshMs) {
        return;
    }
    g_network_cache.refreshed_at = now;
    g_network_cache.available = false;
    g_network_cache.connected = false;
    if (g_session == nullptr || !g_session->Network().IsAvailable()) {
        return;
    }
    g_network_cache.available = true;
    g_network_cache.connected = g_session->Network().IsConnected().value_or(false);
}

// Whether the player is in the world, where the clock belongs.
//
// The SCS telemetry events are the precise source. Without them the TruckersMP
// connection state is the next best thing, and when neither answers the clock is
// simply shown rather than hidden forever.
bool IsInGame() {
    RefreshNetworkCache();
    if (g_scs_ready.load()) {
        return g_scs_in_game.load();
    }
    if (g_network_cache.available) {
        return g_network_cache.connected;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The one hotkey
// ---------------------------------------------------------------------------

// Requires g_config_mutex.
void ToggleClockVisibilityLocked() {
    g_config.time_visible = !g_config.time_visible;
    SaveConfig(g_config, ConfigPath());
    Notify(
        g_config.time_visible ? NotificationType::Success : NotificationType::Normal,
        g_config.time_visible ? "时间显示已开启" : "时间显示已关闭");
}

// ---------------------------------------------------------------------------
// ImGui setup
// ---------------------------------------------------------------------------

// Requires g_imgui_mutex.
void ShutdownImGuiLocked() {
    if (g_imgui_ready.load()) {
        g_imgui_ready.store(false);
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
    }
    if (g_context != nullptr) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
    g_frame_timer_ready = false;
    g_cached_target = nullptr;
    g_cached_target_width = 0.0f;
    g_cached_target_height = 0.0f;
}

// The device comes from the client, so we only borrow it (and hold a reference
// of our own for as long as we use it).
//
// This is the expensive path - font loading and shader compilation - and it runs
// with no lock held so the game thread is never blocked by it.
bool InitializeImGui(ID3D11Device* device) {
    std::lock_guard<std::mutex> init_lock(g_init_mutex);

    {
        std::lock_guard<std::mutex> imgui_lock(g_imgui_mutex);
        ShutdownImGuiLocked();
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendPlatformName = "TruckersMP Client SDK";
    io.BackendRendererName = "imgui_impl_dx11";

    // The overlay draws text only, so the interface never asks ImGui for input.
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f);
    if (font == nullptr) {
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    }
    if (font != nullptr) {
        io.FontDefault = font;
    }

    if (!ImGui_ImplDX11_Init(device, context)) {
        ImGui::DestroyContext();
        context->Release();
        return false;
    }

    device->AddRef();
    std::lock_guard<std::mutex> imgui_lock(g_imgui_mutex);
    g_device = device;
    g_context = context;
    g_imgui_ready.store(true);
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void DrawTime(const OverlayConfig& config, float visibility) {
    char time_text[256]{};
    FormatTimeText(config, time_text, sizeof(time_text));

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float remaining = 1.0f - visibility;
    const float eased = 1.0f - remaining * remaining * remaining;
    const float font_size = static_cast<float>(config.font_size) * (0.90f + 0.10f * eased);
    const float line_gap = std::round(font_size * 0.25f);

    const ImVec2 time_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, time_text);
    ImVec2 custom_sizes[kCustomTextLines];
    float block_height = time_size.y;
    const int custom_lines = std::clamp(config.custom_text_lines, 0, kCustomTextLines);
    for (int line = 0; line < custom_lines; ++line) {
        custom_sizes[line] = ImVec2(0.0f, 0.0f);
        if (config.custom_text[line][0] == '\0') {
            continue;
        }
        custom_sizes[line] =
            font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, config.custom_text[line]);
        block_height += custom_sizes[line].y + line_gap;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float anchor_x = display.x * static_cast<float>(config.horizontal_percent) / 100.0f;
    const float rise = (1.0f - eased) * 16.0f;
    float line_y = display.y - static_cast<float>(config.bottom_margin) - block_height + rise;
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(
        config.text_color[0],
        config.text_color[1],
        config.text_color[2],
        config.text_color[3] * eased));
    for (int line = 0; line < custom_lines; ++line) {
        if (custom_sizes[line].y <= 0.0f) {
            continue;
        }
        draw_list->AddText(
            font,
            font_size,
            ImVec2(std::round(anchor_x - custom_sizes[line].x * 0.5f), std::round(line_y)),
            color,
            config.custom_text[line]);
        line_y += custom_sizes[line].y + line_gap;
    }
    draw_list->AddText(
        font,
        font_size,
        ImVec2(std::round(anchor_x - time_size.x * 0.5f), std::round(line_y)),
        color,
        time_text);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

// Requires g_imgui_mutex.
bool QueryBackBufferSize(ID3D11RenderTargetView* target, float& width, float& height) {
    if (target == g_cached_target) {
        width = g_cached_target_width;
        height = g_cached_target_height;
        return width > 0.0f && height > 0.0f;
    }
    ID3D11Resource* resource = nullptr;
    target->GetResource(&resource);
    if (resource == nullptr) {
        return false;
    }
    bool ok = false;
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture))) && texture != nullptr) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        width = static_cast<float>(description.Width);
        height = static_cast<float>(description.Height);
        ok = width > 0.0f && height > 0.0f;
        texture->Release();
    }
    resource->Release();
    if (ok) {
        g_cached_target = target;
        g_cached_target_width = width;
        g_cached_target_height = height;
    }
    return ok;
}

// Requires g_imgui_mutex.
void UpdateFrameDelta() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (!g_frame_timer_ready) {
        QueryPerformanceFrequency(&g_frame_frequency);
        g_frame_last = now;
        g_frame_timer_ready = true;
    }
    LONGLONG elapsed = now.QuadPart - g_frame_last.QuadPart;
    g_frame_last = now;
    if (elapsed <= 0 || g_frame_frequency.QuadPart <= 0) {
        elapsed = 1;
    }
    const float delta = static_cast<float>(
        static_cast<double>(elapsed) / static_cast<double>(g_frame_frequency.QuadPart));
    // Clamp so a stalled frame (alt-tab, loading screen) cannot blow up animations.
    ImGui::GetIO().DeltaTime = std::clamp(delta, 1.0f / 1000.0f, 0.25f);
}

// Runs inside Render().OnPostRender: the client has finished its own drawing and
// the device is in a known state. ImGui restores the pipeline state it changes,
// and we re-bind the very render target we found.
void RenderFrame() {
    if (g_shutting_down.load() || g_session == nullptr) {
        return;
    }
    RefreshRenderDevice();
    if (!g_render_device_ready || g_cached_renderer != RendererID::DirectX11 ||
        g_cached_device == nullptr) {
        return;
    }
    if (!g_imgui_ready.load() && !InitializeImGui(g_cached_device)) {
        return;
    }

    const bool in_game = IsInGame();

    // Fast path: the clock is off screen and nothing is fading out. Skip the
    // whole frame - no ImGui work and no D3D11 state churn.
    {
        std::lock_guard<std::mutex> config_lock(g_config_mutex);
        if (g_time_animation <= 0.0f && !(in_game && g_config.time_visible)) {
            // Restart cleanly when the overlay comes back: the frame timer would
            // otherwise report the whole idle period as one frame delta.
            g_frame_timer_ready = false;
            return;
        }
    }

    std::unique_lock<std::mutex> imgui_lock(g_imgui_mutex);
    if (g_shutting_down.load() || !g_imgui_ready.load()) {
        return;
    }

    ID3D11DeviceContext* context = g_context;
    ID3D11RenderTargetView* target = nullptr;
    ID3D11DepthStencilView* depth = nullptr;
    context->OMGetRenderTargets(1, &target, &depth);
    if (target == nullptr) {
        if (depth != nullptr) {
            depth->Release();
        }
        return;
    }

    float width = 0.0f;
    float height = 0.0f;
    if (!QueryBackBufferSize(target, width, height)) {
        target->Release();
        if (depth != nullptr) {
            depth->Release();
        }
        return;
    }

    ImGui_ImplDX11_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    UpdateFrameDelta();
    ImGui::NewFrame();

    {
        std::lock_guard<std::mutex> config_lock(g_config_mutex);
        // Short fade/slide so the clock does not pop in or out.
        const bool time_should_show = in_game && g_config.time_visible;
        const float animation_step = io.DeltaTime * 5.0f;
        if (!in_game) {
            g_time_animation = 0.0f;
        } else if (time_should_show) {
            g_time_animation = std::min(1.0f, g_time_animation + animation_step);
        } else {
            g_time_animation = std::max(0.0f, g_time_animation - animation_step);
        }
        if (in_game && g_time_animation > 0.0f) {
            DrawTime(g_config, g_time_animation);
        }
    }

    ImGui::Render();
    // Everything above is CPU-only. The D3D11 calls below run with no lock held,
    // so the game thread can never end up waiting on the renderer.
    imgui_lock.unlock();

    context->OMSetRenderTargets(1, &target, depth);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    target->Release();
    if (depth != nullptr) {
        depth->Release();
    }
}

// ---------------------------------------------------------------------------
// Input events
// ---------------------------------------------------------------------------

// The only hotkey: F8 shows or hides the clock. Nothing is forwarded to ImGui,
// so this is the entire input surface of the plugin.
void OnKeyEvent(TruckersMP::InputKeyEvent& event) {
    if (!event.GetDown() || event.GetKey() != VK_F8) {
        return;
    }
    std::lock_guard<std::mutex> config_lock(g_config_mutex);
    ToggleClockVisibilityLocked();
}

// ---------------------------------------------------------------------------
// SCS telemetry (optional companion)
// ---------------------------------------------------------------------------

void __stdcall HandleScsEvent(ScsEvent event, const void*, ScsContext) {
    if (event == kScsTelemetryEventStarted) {
        g_scs_in_game = true;
    } else if (event == kScsTelemetryEventPaused) {
        g_scs_in_game = false;
    }
}

void EnsureConfigLoaded() {
    const std::wstring path = ConfigPath();
    const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::lock_guard<std::mutex> config_lock(g_config_mutex);
    g_config = LoadConfig(path);
    if (!exists) {
        // First run: drop the defaults next to the DLL so every key is discoverable.
        SaveConfig(g_config, path);
    }
}

void UnregisterScsEvents() {
    if (g_scs_unregister != nullptr) {
        g_scs_unregister(kScsTelemetryEventPaused);
        g_scs_unregister(kScsTelemetryEventStarted);
        g_scs_unregister = nullptr;
    }
    g_scs_ready = false;
    g_scs_in_game = false;
}

}  // namespace

// ---------------------------------------------------------------------------
// TruckersMP Client SDK entry points
// ---------------------------------------------------------------------------

TMP_EXPORT bool TMP_API truckersmp_init(
    const TruckersMP_Host* host, TruckersMP_PluginDesc* desc) {
    TruckersMP::PluginInfo info;
    info.m_name = kPluginName;
    info.m_author = kPluginAuthor;
    info.m_version = kPluginVersion;
    info.m_description = kPluginDescription;
    TruckersMP::FillPluginDesc(desc, info);

    if (g_session != nullptr) {
        return true;
    }

    g_session = TruckersMP::Session::Create(host);
    if (g_session == nullptr) {
        // The client refused the session: the load is refused and the client
        // unloads the DLL without calling truckersmp_shutdown.
        return false;
    }

    EnsureConfigLoaded();
    g_shutting_down = false;

    TruckersMP::Session& session = *g_session;
    LogFormat(
        LogLevel::Info,
        "%s %s initialised (client %s, game %s, TruckersMP SDK 1.1.0)",
        kPluginName,
        kPluginVersion,
        session.Core().ClientVersion().value_or("unknown").c_str(),
        session.Core().GameID().value_or("unknown").c_str());

    if (!session.Render().IsAvailable()) {
        Log(
            LogLevel::Error,
            "Render module unavailable - the overlay is disabled. Please update the "
            "TruckersMP client.");
        Notify(
            NotificationType::Error,
            "TMP Time Overlay: 渲染模块不可用，请更新 TruckersMP 客户端");
        return true;
    }
    RefreshRenderDevice();
    if (g_render_device_ready && g_cached_renderer != RendererID::DirectX11) {
        Log(LogLevel::Error, "The client is not running on DirectX 11 - the overlay is disabled.");
        Notify(NotificationType::Error, "TMP Time Overlay: 仅支持 DirectX 11 渲染器");
        return true;
    }

    session.Render().OnPostRender.Register([] { RenderFrame(); });

    if (session.Input().IsAvailable()) {
        session.Input().OnKey.Register([](TruckersMP::InputKeyEvent& event) {
            OnKeyEvent(event);
        });
    } else {
        Log(LogLevel::Warning, "Input module unavailable - F8 cannot toggle the clock.");
    }

    Log(LogLevel::Info, "Ready. F8 shows or hides the clock. Configure via tmp_time_overlay.ini.");
    return true;
}

TMP_EXPORT void TMP_API truckersmp_shutdown(void) {
    if (g_session == nullptr) {
        return;
    }
    g_shutting_down = true;
    {
        std::lock_guard<std::mutex> init_lock(g_init_mutex);
        std::lock_guard<std::mutex> imgui_lock(g_imgui_mutex);
        ShutdownImGuiLocked();
    }
    // Destroying the session unregisters every listener.
    g_session.reset();
    g_render_device_ready = false;
    g_cached_device = nullptr;
    g_render_device_countdown = 0;
    g_network_cache = NetworkCache{};
    g_shutting_down = false;
}

// ---------------------------------------------------------------------------
// SCS Telemetry SDK entry points
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) int __cdecl scs_telemetry_init(
    unsigned int version, const void* params) {
    if (version != kScsTelemetryVersion100 && version != kScsTelemetryVersion101) {
        return kScsResultUnsupported;
    }
    const auto* telemetry_params = static_cast<const ScsTelemetryInitParamsV101*>(params);
    if (telemetry_params == nullptr || telemetry_params->register_for_event == nullptr ||
        telemetry_params->unregister_from_event == nullptr) {
        return kScsResultInvalidParameter;
    }

    bool expected = false;
    if (!g_scs_ready.compare_exchange_strong(expected, true)) {
        return kScsResultOk;
    }
    g_scs_in_game = false;

    const ScsResult paused_result =
        telemetry_params->register_for_event(kScsTelemetryEventPaused, HandleScsEvent, nullptr);
    const ScsResult started_result =
        telemetry_params->register_for_event(kScsTelemetryEventStarted, HandleScsEvent, nullptr);
    if (paused_result == kScsResultOk && started_result == kScsResultOk) {
        g_scs_unregister = telemetry_params->unregister_from_event;
    } else {
        // The overlay keeps working without these events; it then falls back to the
        // TruckersMP connection state to decide whether the player is in the world.
        if (paused_result == kScsResultOk) {
            telemetry_params->unregister_from_event(kScsTelemetryEventPaused);
        }
        if (started_result == kScsResultOk) {
            telemetry_params->unregister_from_event(kScsTelemetryEventStarted);
        }
        g_scs_ready = false;
    }
    return kScsResultOk;
}

extern "C" __declspec(dllexport) void __cdecl scs_telemetry_shutdown() {
    UnregisterScsEvents();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
