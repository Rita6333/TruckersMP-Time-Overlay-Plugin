# 🚚 TruckersMP Time Overlay Plugin

An in-game real-time clock overlay for Euro Truck Simulator 2 and American Truck Simulator,
built on the **TruckersMP Client SDK**.
🕒 Renders the time and up to four lines of your own text directly onto the game screen.

## 📸 Preview

![In-game preview](https://i.imgur.com/5SkX1jc.png)

---

## ✨ Features

Press **`F8`** while driving to show or hide the clock. The choice is written straight back to
`tmp_time_overlay.ini`, and a short notification confirms the change.

That is the entire interface — **there is no settings panel.** Everything is configured by editing
`tmp_time_overlay.ini` next to the DLL, which is created with the defaults on first load.

The clock fades and slides in when it appears and fades out when it is hidden.

It is only drawn while driving, so it stays hidden on the main menu, pause screen and loading
screens. The plugin learns that from the game's own telemetry events, and falls back to the
TruckersMP connection state when those are unavailable.

The overlay uses your local Windows time and by default displays it as
`Current time: 18.06.2026 13:34:10 (+0800)`.

### Configuration — `tmp_time_overlay.ini`

The file lives next to `tmp_time_overlay.dll` and is read at load time. Edit it with the game
closed, then start the game again.

| Key | Default | Meaning |
| --- | --- | --- |
| `font_size` | `16` | Text height in pixels (10 – 72). |
| `bottom_margin` | `12` | Distance from the bottom of the screen, in pixels (0 – 300). |
| `horizontal_percent` | `50` | Horizontal anchor, 0 = left edge, 50 = centred, 100 = right edge. |
| `text_color` | `FFFFFF` | `RRGGBB`, optionally prefixed with `#`. |
| `time_visible` | `1` | Whether the clock starts visible. `F8` flips this for you. |
| `show_prefix` | `1` | Show the `Current time: ` prefix. |
| `show_timezone` | `1` | Show the `(+0800)` offset. |
| `time_format` | `%d.%m.%Y %H:%M:%S` | Any `strftime` pattern, e.g. `%H:%M` for hours and minutes only. |
| `custom_text_lines` | `1` | How many of the custom text rows below are drawn (1 – 4). |
| `custom_text` | *(empty)* | First custom text row, drawn above the clock. |
| `custom_text_2` … `custom_text_4` | *(empty)* | The remaining custom text rows. Empty rows are not drawn. |

A complete example:

```ini
[overlay]
font_size=20
bottom_margin=63
horizontal_percent=50
text_color=FFFFFF
time_visible=1
show_prefix=1
show_timezone=1
time_format=%d.%m.%Y %H:%M:%S
custom_text_lines=2
custom_text=[VTC] My Company
custom_text_2=Have a safe trip!
custom_text_3=
custom_text_4=
```

`time_format` accepts the usual `strftime` specifiers — `%d` day, `%m` month, `%Y` year, `%H`
hours, `%M` minutes, `%S` seconds, `%a` weekday, `%b` month name. A pattern the C runtime
rejects falls back to the default format rather than drawing nothing.

---

## 🛠️ Installation Steps

**Step 1: 🛑 Exit the Game**
Ensure Euro Truck Simulator 2 is completely closed.

**Step 2: 📁 Place the File**
Copy `tmp_time_overlay.dll` into the game's plugins directory:

```text
<Euro Truck Simulator 2 installation directory>\bin\win_x64\plugins\

```

> 💡 **Tip**: If there is no `plugins` folder inside the `win_x64` directory, please create one manually.

**Step 3: 🚀 Launch the Game**
Launch the game **through the TruckersMP launcher** — the TruckersMP client loads the plugin.
If a pop-up appears stating "Advanced SDK Detected", simply click Confirm to proceed.

**Step 4: ⏱️ Drive**
The clock appears once you are in the world. Press **`F8`** to hide or show it, and edit
`tmp_time_overlay.ini` (next to the DLL) to change how it looks.

### Requirements

* Windows x64.
* **TruckersMP client 0.7.4.0 or newer** (the version that ships the Client SDK).
* The client must be running on the **DirectX 11** renderer. On OpenGL the plugin loads but
  disables the overlay and says so in the client log.

---

## ⚡ Performance

The overlay runs inside the game's frame, so it is written to stay off the critical path:

* **Nothing on screen, nothing rendered.** When the clock is hidden and has finished fading out,
  the entire ImGui frame is skipped — no `NewFrame`/`Render`, no DirectX 11 pipeline state backup
  and restore. This is the common case while driving with the clock hidden.
* **The plugin has no input surface.** It listens for `F8` and nothing else, and never forwards an
  event to ImGui, so there is no per-event work and no input queue to grow.
* **The client's input state is never touched.** No mouse reference is taken and the game's mouse
  and keyboard locks are left exactly as the game left them.
* **No blocking behind the renderer.** ImGui's DirectX 11 calls run with no lock held, so the game
  thread can never end up waiting for the overlay to finish drawing.
* **Nothing heavy in a lock.** Loading the font and compiling the shaders happens outside every
  lock, so a one-off hitch cannot stall the game thread.
* **Cached instead of polled.** The renderer and device handle, the back buffer size, the
  TruckersMP connection state, and the UTC offset are cached rather than queried every
  frame. The time zone is refreshed at most once every 30 seconds.
* **No per-frame allocations.** The clock line is formatted into a stack buffer, so drawing the
  overlay does not touch the heap.

---

## 🧩 How It Works

The plugin implements the two entry points the TruckersMP client looks for and drives everything
through the SDK's modules:

| SDK module | Used for |
| --- | --- |
| `Render()` | `OnPostRender` frame boundary and the `ID3D11Device` used to draw the overlay |
| `Input()` | the `F8` hotkey, and nothing else |
| `Network()` | connection state, used as the fallback "am I driving?" signal |
| `UserInterface()` | the in-game notification shown when `F8` flips the clock |
| `Core()` | Client log messages, so problems are easy to diagnose |

The plugin also keeps the **SCS telemetry** entry points so the clock can still tell "driving"
from "menu / paused". The two SDKs are designed to co-exist in one DLL. If the game does not
provide the telemetry events, the plugin falls back to the TruckersMP connection state, and if
neither is available it simply shows the clock instead of hiding it forever.

There are **no function hooks and no patching**: no `Present` hook, no `ResizeBuffers` hook, no
window procedure subclassing. Everything goes through the SDK.

### ℹ️ Why there is no settings panel

Earlier builds had a `Ctrl+F9` panel. Drawing one meant asking the client for the mouse cursor and
blocking the game's mouse and keyboard for as long as it was open — a surprising amount of state to
manage, in exchange for something nobody needs mid-drive. The client keeps those locks as
**reference-counted counters**, and an unbalanced pair is easy to get wrong: v2.0 leaked one lock
per open/close cycle until the game stopped accepting mouse input altogether (fixed in 2.0.2).

Rather than keep babysitting that, the panel is gone as of **2.1.0** and the plugin no longer
touches the client's input state at all. Everything it used to configure lives in
`tmp_time_overlay.ini`.

### ⚠️ One client getter is deliberately avoided

`Network().GetLastPing()` is never called. TruckersMP client **0.7.7.6** implements it with a
missing null check: it dereferences its internal session object before that object exists, so
calling it in the first frames after the game starts crashes the whole process with an access
violation inside `core_ets2mp.dll`. The plugin therefore reads only `Network().IsConnected()`,
which is a plain state-field read and is safe at any time. This can be revisited once the client
is fixed.

---

## 🔨 Building from Source

The plugin targets Windows x64 and needs a C++17 compiler. The examples below use
[Zig](https://ziglang.org/) as a portable toolchain; Visual Studio works too via
`tmp_time_overlay.vcxproj`.

```bash
# place zig at tools/zig-x86_64-windows-0.15.2/zig.exe, then:
./build.sh
```

`build.sh` compiles `src/tmp_time_overlay.cpp` together with the vendored Dear ImGui sources and
writes `build/tmp_time_overlay.dll`.

The equivalent command by hand:

```bash
tools/zig-x86_64-windows-0.15.2/zig.exe c++ -target x86_64-windows-gnu -O2 -DNDEBUG -std=c++17 -shared \
  -DUNICODE -D_UNICODE \
  -I external/imgui -I external/imgui/backends -I external/GameClientSDK/include \
  src/tmp_time_overlay.cpp \
  external/imgui/imgui.cpp external/imgui/imgui_draw.cpp \
  external/imgui/imgui_tables.cpp external/imgui/imgui_widgets.cpp \
  external/imgui/backends/imgui_impl_dx11.cpp \
  -o build/tmp_time_overlay.dll \
  -L build -ld3dcompiler -luser32 -lgdi32
```

There is also an integration smoke test that stands in for the TruckersMP client. It verifies the
init/shutdown sequence, drives `F8` through the same event entry point the client uses, and asserts
that the plugin registers no mouse listeners and never touches the client's input capture:

```bash
./build.sh --test
```

---

## 📦 Third-Party Open-Source Components

The source code for this project integrates the following excellent open-source libraries and licenses:

* 🖥️ [Dear ImGui](https://github.com/ocornut/imgui) (v1.92.9) — Licensed under **MIT**
* 🚛 [TruckersMP Client SDK](https://github.com/TruckersMP/GameClientSDK) — Licensed under **MIT**

---

## 📝 Release History

See [CHANGELOG.md](CHANGELOG.md) for the version history.
