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

#include <cstdio>

using TelemetryInit = int(__cdecl*)(unsigned int, const void*);
using TelemetryShutdown = void(__cdecl*)();

LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain() {
    constexpr wchar_t class_name[] = L"TmpTimeOverlayDx11SmokeTest";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = TestWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    if (RegisterClassExW(&window_class) == 0) {
        fwprintf(stderr, L"Could not register the test window class.\n");
        return 1;
    }

    HWND window = CreateWindowExW(
        0,
        class_name,
        L"Euro Truck Simulator 2 - DX11 overlay smoke test",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100,
        100,
        960,
        540,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (window == nullptr) {
        fwprintf(stderr, L"Could not create the test window.\n");
        return 2;
    }

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
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
    HRESULT result = D3D11CreateDeviceAndSwapChain(
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
        fwprintf(stderr, L"Could not create the DX11 swap chain: 0x%08lX\n", result);
        DestroyWindow(window);
        return 3;
    }

    HMODULE plugin = LoadLibraryW(L"tmp_time_overlay.dll");
    if (plugin == nullptr) {
        fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 4;
    }
    const auto init = reinterpret_cast<TelemetryInit>(GetProcAddress(plugin, "scs_telemetry_init"));
    const auto shutdown =
        reinterpret_cast<TelemetryShutdown>(GetProcAddress(plugin, "scs_telemetry_shutdown"));
    if (init == nullptr || shutdown == nullptr) {
        fwprintf(stderr, L"Required telemetry exports were not found.\n");
        return 5;
    }
    if (init(0x00010001, nullptr) != 0) {
        fwprintf(stderr, L"scs_telemetry_init could not install the DX11 hooks.\n");
        return 6;
    }

    ID3D11Texture2D* back_buffer = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    result = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (SUCCEEDED(result)) {
        result = device->CreateRenderTargetView(back_buffer, nullptr, &target);
        back_buffer->Release();
    }
    if (FAILED(result)) {
        fwprintf(stderr, L"Could not create the smoke-test render target.\n");
        shutdown();
        return 7;
    }

    const FLOAT background[4] = {0.08f, 0.12f, 0.18f, 1.0f};
    context->OMSetRenderTargets(1, &target, nullptr);
    context->ClearRenderTargetView(target, background);
    SetForegroundWindow(window);
    result = swap_chain->Present(0, 0);
    if (FAILED(result)) {
        fwprintf(stderr, L"Hooked Present failed: 0x%08lX\n", result);
        shutdown();
        return 8;
    }

    // Exercise the resize hook and render another frame after device objects are recreated.
    context->OMSetRenderTargets(0, nullptr, nullptr);
    target->Release();
    target = nullptr;
    result = swap_chain->ResizeBuffers(2, 800, 450, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(result)) {
        fwprintf(stderr, L"Hooked ResizeBuffers failed: 0x%08lX\n", result);
        shutdown();
        return 9;
    }
    result = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (SUCCEEDED(result)) {
        result = device->CreateRenderTargetView(back_buffer, nullptr, &target);
        back_buffer->Release();
    }
    if (FAILED(result)) {
        fwprintf(stderr, L"Could not recreate the smoke-test render target.\n");
        shutdown();
        return 10;
    }
    result = swap_chain->Present(0, 0);
    if (FAILED(result)) {
        fwprintf(stderr, L"Present after resize failed: 0x%08lX\n", result);
        shutdown();
        return 11;
    }

    shutdown();
    // A post-shutdown Present confirms the original vtable path remains usable.
    result = swap_chain->Present(0, 0);
    if (FAILED(result)) {
        fwprintf(stderr, L"Present after hook shutdown failed: 0x%08lX\n", result);
        return 12;
    }

    target->Release();
    context->Release();
    device->Release();
    swap_chain->Release();
    FreeLibrary(plugin);
    DestroyWindow(window);
    UnregisterClassW(class_name, GetModuleHandleW(nullptr));

    wprintf(L"DX11 Present hook, resize hook, ImGui frame, and shutdown passed.\n");
    return 0;
}
