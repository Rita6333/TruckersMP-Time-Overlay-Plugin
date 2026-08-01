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

## 📝 更新日志

### 2026-08-01

* 新增 **`F8`** 快捷键，无需打开控制面板即可隐藏或恢复时间显示。
* 时间显示状态会自动保存，重新启动游戏后继续使用上次的设置。
* **`Ctrl + F9`** 继续用于打开或关闭中文设置面板。

### 2026-07-30

* 新增字体大小、底部距离、水平位置、文字颜色和前缀设置的自动保存。
* 使用高精度 UTC 时间源，降低时间显示延迟。
* 修复设置面板打开后鼠标无法移动的问题。
* 时间文字改为无描边渲染，并按照实际文字宽度对齐。

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
