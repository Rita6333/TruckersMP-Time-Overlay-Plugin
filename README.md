# 🚚 TruckersMP 时间显示插件 (Time Overlay)

一款专为《Euro Truck Simulator 2》（欧洲卡车模拟2）及 TruckersMP 打造的游戏内实时时间显示插件 🕒。通过 DirectX 11 `Present` Hook 技术，直接在游戏画面中无缝渲染时间与自定义设置面板。

```text
Current Time: 2026-Jul-29 17:57:44 UTC

```

---

## ✨ 核心特性与设置

进入游戏后，按下快捷键 **`Ctrl + F9`** 即可开启或关闭中文设置面板（基于 ImGui）。

设置面板支持丰富的自定义选项：

* 🎨 **外观调节**：自定义字体大小、文字颜色
* 📍 **位置摆放**：自由调整水平位置、距画面底部的边缘距离
* ✏️ **文本定制**：支持修改或隐藏 `Current Time:` 文本前缀

---

## 🛠️ 安装步骤

1. 🛑 **退出游戏**：确保彻底关闭《Euro Truck Simulator 2》及 TruckersMP 客户端。
2. 📁 **放置文件**：将 `tmp_time_overlay.dll` 复制到游戏的插件目录中：
```text
<Euro Truck Simulator 2 安装目录>\bin\win_x64\plugins\

```


> 💡 **小贴士**：如果 `win_x64` 目录下没有 `plugins` 文件夹，请手动新建一个。


3. 🚀 **启动游戏**：正常启动游戏。若弹窗提示“检测到高级 SDK (Advanced SDK Detected)”，点击确认继续即可。
4. ⚙️ **开启面板**：进入游戏后按下 **`Ctrl + F9`**，调出设置面板开始个性化配置！

---

## 📦 第三方开源组件说明

本项目源码内集成了以下优秀的开源库及许可证：

* 🖥️ [Dear ImGui](https://github.com/ocornut/imgui) (v1.92.9) — 遵循 **MIT** 许可证
* 🪝 [MinHook](https://github.com/TsudaKageyu/minhook) (v1.3.4) — 遵循 **BSD-2-Clause** 许可证