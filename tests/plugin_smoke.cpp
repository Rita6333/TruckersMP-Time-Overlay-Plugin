// Integration smoke test for tmp_time_overlay.dll.
//
// Stands in for the TruckersMP client: it hands the plugin a fake
// TruckersMP_Host with minimal Core, Render, Input and UserInterface tables and
// checks that truckersmp_init drives the documented sequence - declare the SDK
// version, fetch the API tables, register the post-render listener and the F8
// hotkey - and that truckersmp_shutdown tears everything down and can be
// followed by a reload.
//
// The plugin used to grow a settings panel that took the client's input capture
// (a mouse reference plus the game mouse/keyboard locks) while it was open. The
// client implements those as reference-counted counters, and getting the pairing
// wrong leaked a lock on every open/close until the game stopped accepting mouse
// input altogether. The panel is gone, so this test now also asserts that the
// plugin never touches the input capture at all.
//
// Build:  zig c++ -target x86_64-windows-gnu -O2 -std=c++17 plugin_smoke.cpp
//              -I external/GameClientSDK/include -L build -ltmp_time_overlay -o ...

#include <TruckersMP/TruckersMP.hxx>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

int g_declare_version_calls = 0;
int g_get_api_calls = 0;
int g_add_listener_calls = 0;
int g_remove_listener_calls = 0;
int g_log_calls = 0;
int g_log_errors = 0;
uint32_t g_declared_version = 0;

constexpr int kMaxRegisteredEvents = 32;
uint64_t g_registered_events[kMaxRegisteredEvents] = {};
int g_registered_event_count = 0;

bool RegisteredEvent(uint64_t eventId) {
    for (int i = 0; i < g_registered_event_count; ++i) {
        if (g_registered_events[i] == eventId) {
            return true;
        }
    }
    return false;
}

constexpr int kMaxNotifications = 16;
constexpr int kNotificationCapacity = 160;
char g_notifications[kMaxNotifications][kNotificationCapacity] = {};
int g_notification_count = 0;

bool g_use_directx11 = true;
uint64_t g_device_handle = 0;

// --- Core -----------------------------------------------------------------

// Captured so the test can push input events exactly the way the client does.
TruckersMP_EventFn g_input_key_dispatch = nullptr;
void* g_input_key_user_ctx = nullptr;

TruckersMP_EResult TMP_API StubAddListener(
    TruckersMP_PluginContext*, uint64_t eventId, TruckersMP_EventFn fn, void* userCtx) {
    ++g_add_listener_calls;
    if (g_registered_event_count < kMaxRegisteredEvents) {
        g_registered_events[g_registered_event_count++] = eventId;
    }
    if (eventId == TRUCKERSMP_EVT_INPUT_KEY) {
        g_input_key_dispatch = fn;
        g_input_key_user_ctx = userCtx;
    }
    return TruckersMP_Result_Ok;
}

TruckersMP_EResult TMP_API StubRemoveListener(
    TruckersMP_PluginContext*, uint64_t eventId, TruckersMP_EventFn, void*) {
    ++g_remove_listener_calls;
    if (eventId == TRUCKERSMP_EVT_INPUT_KEY) {
        g_input_key_dispatch = nullptr;
        g_input_key_user_ctx = nullptr;
    }
    return TruckersMP_Result_Ok;
}

TruckersMP_EResult TMP_API StubUnlockIntent(TruckersMP_PluginContext*, TruckersMP_String) {
    return TruckersMP_Result_Ok;
}

TruckersMP_EResult TMP_API StubLogMessage(
    TruckersMP_PluginContext*, TruckersMP_LogLevel level, TruckersMP_String text) {
    ++g_log_calls;
    if (level == TruckersMP_LogLevel_Error) {
        ++g_log_errors;
    }
    std::printf("  [client log] %.*s\n", static_cast<int>(text.m_size), text.m_ptr);
    return TruckersMP_Result_Ok;
}

bool TMP_API StubGameID(TruckersMP_PluginContext*, TruckersMP_String* out) {
    static const char kGame[] = "eut2";
    out->m_ptr = kGame;
    out->m_size = sizeof(kGame) - 1;
    return true;
}

bool TMP_API StubClientVersion(TruckersMP_PluginContext*, TruckersMP_String* out) {
    static const char kVersion[] = "0.7.4.0";
    out->m_ptr = kVersion;
    out->m_size = sizeof(kVersion) - 1;
    return true;
}

// --- UserInterface --------------------------------------------------------

TruckersMP_EResult TMP_API StubShowNotification(
    TruckersMP_PluginContext*, TruckersMP_NotificationType, TruckersMP_String message) {
    if (g_notification_count < kMaxNotifications) {
        const int length =
            std::min<int>(static_cast<int>(message.m_size), kNotificationCapacity - 1);
        if (message.m_ptr != nullptr && length > 0) {
            std::memcpy(g_notifications[g_notification_count], message.m_ptr, length);
            g_notifications[g_notification_count][length] = '\0';
        } else {
            g_notifications[g_notification_count][0] = '\0';
        }
        ++g_notification_count;
    }
    return TruckersMP_Result_Ok;
}

// --- Render ---------------------------------------------------------------

bool TMP_API StubGetRendererID(TruckersMP_PluginContext*, TruckersMP_RendererID* out) {
    *out = g_use_directx11 ? TruckersMP_RendererID_DirectX11 : TruckersMP_RendererID_OpenGL;
    return true;
}

bool TMP_API StubGetDeviceHandle(TruckersMP_PluginContext*, uint64_t* out) {
    *out = g_device_handle;
    return true;
}

// --- Input ----------------------------------------------------------------
//
// Models the real client as implemented in core_ets2mp.dll: the two "the game
// must not see this input" flags are *counters*, and IncreaseMouseRef() bumps the
// mouse lock counter as well when it takes the cursor reference count from zero
// to one. The plugin must never call any of these now that the panel is gone.

int g_mouse_locks = 0;
int g_keyboard_locks = 0;
int g_mouse_refs = 0;
int g_increase_mouse_ref_calls = 0;
int g_decrease_mouse_ref_calls = 0;
int g_set_mouse_locked_calls = 0;
int g_set_keyboard_locked_calls = 0;
int g_read_lock_state_calls = 0;

bool TMP_API StubIsMouseVisible(TruckersMP_PluginContext*, bool* out) {
    *out = g_mouse_refs > 0;
    return true;
}

TruckersMP_EResult TMP_API StubIncreaseMouseRef(TruckersMP_PluginContext*) {
    ++g_increase_mouse_ref_calls;
    if (++g_mouse_refs == 1) {
        ++g_mouse_locks;
    }
    return TruckersMP_Result_Ok;
}

TruckersMP_EResult TMP_API StubDecreaseMouseRef(TruckersMP_PluginContext*) {
    ++g_decrease_mouse_ref_calls;
    if (g_mouse_refs > 0 && --g_mouse_refs == 0 && g_mouse_locks > 0) {
        --g_mouse_locks;
    }
    return TruckersMP_Result_Ok;
}

bool TMP_API StubIsGameMouseLocked(TruckersMP_PluginContext*, bool* out) {
    ++g_read_lock_state_calls;
    *out = g_mouse_locks > 0;
    return true;
}

TruckersMP_EResult TMP_API StubSetGameMouseLocked(TruckersMP_PluginContext*, bool locked) {
    ++g_set_mouse_locked_calls;
    if (locked) {
        ++g_mouse_locks;
    } else if (g_mouse_locks > 0) {
        --g_mouse_locks;
    }
    return TruckersMP_Result_Ok;
}

bool TMP_API StubIsGameKeyboardLocked(TruckersMP_PluginContext*, bool* out) {
    ++g_read_lock_state_calls;
    *out = g_keyboard_locks > 0;
    return true;
}

TruckersMP_EResult TMP_API StubSetGameKeyboardLocked(TruckersMP_PluginContext*, bool locked) {
    ++g_set_keyboard_locked_calls;
    if (locked) {
        ++g_keyboard_locks;
    } else if (g_keyboard_locks > 0) {
        --g_keyboard_locks;
    }
    return TruckersMP_Result_Ok;
}

// --- Host -----------------------------------------------------------------

const TruckersMP_Core kCoreTable{
    &StubAddListener,
    &StubRemoveListener,
    &StubUnlockIntent,
    &StubLogMessage,
    &StubGameID,
    &StubClientVersion};

const TruckersMP_Render kRenderTable{&StubGetRendererID, &StubGetDeviceHandle};

const TruckersMP_Input kInputTable{
    &StubIsMouseVisible,
    &StubIncreaseMouseRef,
    &StubDecreaseMouseRef,
    &StubIsGameMouseLocked,
    &StubSetGameMouseLocked,
    &StubIsGameKeyboardLocked,
    &StubSetGameKeyboardLocked};

const TruckersMP_UserInterface kUserInterfaceTable{&StubShowNotification};

TruckersMP_EResult TMP_API StubDeclareVersion(
    TruckersMP_PluginContext*, uint32_t sdkVersion) {
    ++g_declare_version_calls;
    g_declared_version = sdkVersion;
    return TruckersMP_Result_Ok;
}

const void* TMP_API StubGetApi(TruckersMP_PluginContext*, uint64_t id) {
    ++g_get_api_calls;
    if (id == TRUCKERSMP_API_CORE) {
        return &kCoreTable;
    }
    if (id == TRUCKERSMP_API_RENDER) {
        return &kRenderTable;
    }
    if (id == TRUCKERSMP_API_INPUT) {
        return &kInputTable;
    }
    if (id == TRUCKERSMP_API_USER_INTERFACE) {
        return &kUserInterfaceTable;
    }
    return nullptr;
}

void TMP_API StubFree(TruckersMP_PluginContext*, void*) {}

int g_failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "  PASS" : "  FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

// Drives the same entry point the client uses for input events.
void DispatchKey(unsigned char virtual_key) {
    if (g_input_key_dispatch == nullptr) {
        return;
    }
    TruckersMP_Evt_InputKey event{};
    event.m_key = virtual_key;
    event.m_flags = 0;
    event.m_down = true;
    g_input_key_dispatch(
        g_input_key_user_ctx, TRUCKERSMP_EVT_INPUT_KEY, &event, sizeof(event));
}

bool NotificationContains(int index, const char* needle) {
    return index >= 0 && index < g_notification_count &&
           std::strstr(g_notifications[index], needle) != nullptr;
}

}  // namespace

int main() {
    TruckersMP_Host host{};
    host.m_size = sizeof(host);
    host.m_ctx = nullptr;
    host.DeclareVersion = &StubDeclareVersion;
    host.GetApi = &StubGetApi;
    host.Free = &StubFree;

    std::printf("--- first load ---\n");
    TruckersMP_PluginDesc desc{};
    const bool loaded = truckersmp_init(&host, &desc);
    Check(loaded, "truckersmp_init returned true");
    Check(std::strcmp(desc.m_name, "TMP Time Overlay") == 0, "plugin name filled");
    Check(desc.m_version[0] != '\0', "plugin version filled");
    Check(g_declare_version_calls == 1, "Session::Create declared the SDK version once");
    Check(g_declared_version == TRUCKERSMP_SDK_VERSION, "declared the SDK version it built against");
    Check(g_get_api_calls >= 10, "every module table was requested");
    Check(RegisteredEvent(TRUCKERSMP_EVT_RENDER_POST_RENDER), "post-render listener registered");
    Check(RegisteredEvent(TRUCKERSMP_EVT_INPUT_KEY), "input key listener registered");
    Check(
        !RegisteredEvent(TRUCKERSMP_EVT_INPUT_MOUSE_BUTTON) &&
            !RegisteredEvent(TRUCKERSMP_EVT_INPUT_MOUSE_MOVE) &&
            !RegisteredEvent(TRUCKERSMP_EVT_INPUT_MOUSE_WHEEL) &&
            !RegisteredEvent(TRUCKERSMP_EVT_INPUT_CHAR),
        "no mouse or text listeners are registered (there is no panel to drive)");
    Check(g_log_calls >= 2, "the plugin logged through Core()");
    Check(g_log_errors == 0, "no errors logged on a healthy client");

    std::printf("--- F8 hotkey ---\n");
    const int notifications_before = g_notification_count;
    DispatchKey(VK_F8);
    Check(
        g_notification_count == notifications_before + 1,
        "F8 is handled and reports the new state");
    Check(
        NotificationContains(notifications_before, "时间显示已"),
        "the notification describes the clock state");
    DispatchKey(VK_F8);
    Check(
        g_notification_count == notifications_before + 2 &&
            std::strcmp(
                g_notifications[notifications_before],
                g_notifications[notifications_before + 1]) != 0,
        "F8 toggles back and forth");
    DispatchKey(VK_F9);
    Check(
        g_notification_count == notifications_before + 2,
        "F9 alone is not a hotkey any more");

    std::printf("--- shutdown ---\n");
    truckersmp_shutdown();
    Check(g_remove_listener_calls >= 1, "listeners removed on shutdown");
    // The client's input locks are reference counted, so touching one without a
    // matching release silently unlocks input the game itself is holding.
    Check(
        g_increase_mouse_ref_calls == 0 && g_decrease_mouse_ref_calls == 0 &&
            g_set_mouse_locked_calls == 0 && g_set_keyboard_locked_calls == 0 &&
            g_read_lock_state_calls == 0,
        "the plugin never touches the client's input capture");
    Check(
        g_mouse_locks == 0 && g_keyboard_locks == 0 && g_mouse_refs == 0,
        "the client input counters are untouched");

    std::printf("--- reload (dev command) ---\n");
    const int add_calls_before = g_add_listener_calls;
    Check(truckersmp_init(&host, &desc), "second truckersmp_init succeeded");
    Check(
        g_add_listener_calls > add_calls_before,
        "post-render listener registered again");
    truckersmp_shutdown();

    std::printf("--- non DirectX 11 client ---\n");
    g_use_directx11 = false;
    Check(truckersmp_init(&host, &desc), "load still succeeds on OpenGL");
    Check(g_log_errors >= 1, "an error is logged when the renderer is unsupported");
    truckersmp_shutdown();

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "SMOKE TEST PASSED" : "SMOKE TEST FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
