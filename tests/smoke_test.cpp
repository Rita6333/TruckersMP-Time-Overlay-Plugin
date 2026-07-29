#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <cstdio>

using TelemetryInit = int(__cdecl*)(unsigned int, const void*);
using TelemetryShutdown = void(__cdecl*)();

int wmain() {
    HWND game_window = CreateWindowExW(
        0,
        L"STATIC",
        L"Euro Truck Simulator 2 - overlay smoke test",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100,
        100,
        960,
        540,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (game_window == nullptr) {
        fwprintf(stderr, L"Could not create the test window.\n");
        return 1;
    }
    SetForegroundWindow(game_window);

    HMODULE plugin = LoadLibraryW(L"tmp_time_overlay.dll");
    if (plugin == nullptr) {
        fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 2;
    }

    const auto init = reinterpret_cast<TelemetryInit>(GetProcAddress(plugin, "scs_telemetry_init"));
    const auto shutdown = reinterpret_cast<TelemetryShutdown>(GetProcAddress(plugin, "scs_telemetry_shutdown"));
    if (init == nullptr || shutdown == nullptr) {
        fwprintf(stderr, L"Required telemetry exports were not found.\n");
        FreeLibrary(plugin);
        return 3;
    }

    if (init(0x00010001, nullptr) != 0) {
        fwprintf(stderr, L"scs_telemetry_init returned an error.\n");
        FreeLibrary(plugin);
        return 4;
    }

    Sleep(500);
    HWND overlay = FindWindowW(L"TmpTimeOverlayWindow", L"TMP UTC Time Overlay");
    if (overlay == nullptr) {
        fwprintf(stderr, L"The overlay window was not created.\n");
        shutdown();
        FreeLibrary(plugin);
        return 5;
    }

    RECT rect{};
    GetWindowRect(overlay, &rect);
    wprintf(
        L"Overlay created: %ldx%ld at (%ld,%ld)\n",
        rect.right - rect.left,
        rect.bottom - rect.top,
        rect.left,
        rect.top);

    HWND editor = FindWindowW(L"TmpTimeOverlayEditorWindow", L"TMP Time Overlay Settings");
    if (editor == nullptr || IsWindowVisible(editor)) {
        fwprintf(stderr, L"The hidden editor window was not created correctly.\n");
        shutdown();
        FreeLibrary(plugin);
        return 6;
    }
    PostMessageW(editor, WM_HOTKEY, 1, 0);
    Sleep(250);
    if (!IsWindowVisible(editor)) {
        fwprintf(stderr, L"The editor did not open.\n");
        shutdown();
        FreeLibrary(plugin);
        return 7;
    }
    PostMessageW(editor, WM_HOTKEY, 1, 0);
    Sleep(250);
    if (IsWindowVisible(editor)) {
        fwprintf(stderr, L"The editor did not close.\n");
        shutdown();
        FreeLibrary(plugin);
        return 8;
    }
    wprintf(L"Editor open and close passed.\n");

    shutdown();
    const bool closed = FindWindowW(L"TmpTimeOverlayWindow", L"TMP UTC Time Overlay") == nullptr;
    FreeLibrary(plugin);
    DestroyWindow(game_window);
    if (!closed) {
        fwprintf(stderr, L"The overlay window did not close.\n");
        return 9;
    }

    wprintf(L"Telemetry exports and shutdown passed.\n");
    return 0;
}
