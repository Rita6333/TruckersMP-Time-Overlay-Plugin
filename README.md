# 🚚 TruckersMP Time Overlay Plugin

An in-game real-time time display plugin designed for Euro Truck Simulator 2.
🕒 Seamlessly renders the time and a customizable settings panel directly onto the game screen.

## 📸 Preview

![In-game preview](https://i.imgur.com/5SkX1jc.png)

---

## ✨ Key Features & Settings

Once in-game, press the hotkey **`Ctrl + F9`** to toggle the settings panel.

The settings panel offers extensive customization options:
* 🎨 **Appearance**: Customize font size and text color.
* 📍 **Positioning**: Freely adjust horizontal placement and the distance from the bottom of the screen.
* ✏️ **Text Customization**: Modify or hide the `Current Time:` text prefix.

---

## 📝 Changelog

### 2026-08-01

* Added the **`F8`** hotkey to toggle the time display on or off without opening the control panel.
* The time display state is automatically saved, preserving your settings after restarting the game.
* **`Ctrl + F9`** remains the hotkey for opening or closing the settings panel.

### 2026-07-30

* Added automatic saving for settings regarding font size, bottom offset, horizontal position, text color, and prefix.
* Switched to a high-precision UTC time source to reduce display latency.
* Fixed an issue where the mouse could not move after opening the settings panel.
* Changed time text rendering to remove the outline and aligned the text based on its actual width.

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
Launch the game as usual. If a pop-up appears stating "Advanced SDK Detected," simply click Confirm to proceed.

**Step 4: ⚙️ Open the Panel**
Once in the game, press **`Ctrl + F9`** to bring up the settings panel and begin customization.

---

## 📦 Third-Party Open-Source Components

The source code for this project integrates the following excellent open-source libraries and licenses:

* 🖥️ [Dear ImGui](https://github.com/ocornut/imgui) (v1.92.9) — Licensed under **MIT**
* 🪝 [MinHook](https://github.com/TsudaKageyu/minhook) (v1.3.4) — Licensed under **BSD-2-Clause**

```

```
