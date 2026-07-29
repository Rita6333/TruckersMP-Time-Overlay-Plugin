# TruckersMP UTC 时间显示 DLL

这是一个供 Euro Truck Simulator 2 / TruckersMP 使用的 x64 SCS Telemetry 插件。时间和设置面板通过 DirectX 11 `Present` Hook 直接绘制在游戏帧内。

```text
Current Time: 2026-Jul-29 17:57:44 UTC
```

时间文字没有描边，并使用实际文字宽度按锚点精确对齐。

## 游戏内设置面板

进入游戏后按 `Ctrl+F9` 打开或关闭中文 ImGui 设置面板。支持调整：

- 字体大小
- 距离画面底部的位置
- 水平位置
- 文字颜色
- `Current Time:` 前缀

修改后点击“应用并保存”，配置会写入 DLL 旁边的 `tmp_time_overlay.ini`。面板采用官方 Dear ImGui 和 MinHook 独立实现，没有复制 PrismTextureStreamer 的面板源码。

## 安装

1. 完全退出 Euro Truck Simulator 2 和 TruckersMP。
2. 将 `tmp_time_overlay.dll` 和 `tmp_time_overlay.ini` 放到：

   ```text
   <Euro Truck Simulator 2 安装目录>\bin\win_x64\plugins\
   ```

3. 没有 `plugins` 文件夹时自行新建。
4. 启动游戏；出现“检测到高级 SDK”提示时确认继续。
5. 按 `Ctrl+F9` 打开设置面板。

## 手动配置

- `font_size`：字号，范围 10 到 72。
- `bottom_margin`：文字与画面底边的距离。
- `horizontal_percent`：水平锚点，`0` 为最左、`50` 为严格居中、`100` 为最右。
- `text_color`：六位 RGB 十六进制文字颜色。
- `show_prefix`：设为 `1` 时显示 `Current Time:` 前缀。

## 构建

需要 Visual Studio 2022 和“使用 C++ 的桌面开发”工作负载。可以运行：

```powershell
.\build.ps1
```

也可以使用 CMake：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

项目内固定包含 Dear ImGui 1.92.9（MIT）与 MinHook 1.3.4（BSD-2-Clause）的构建所需源码及许可证。

## 注意

此版本会 Hook DirectX 11 的 `Present` 和 `ResizeBuffers`，兼容性会受游戏、显卡覆盖层及 TruckersMP 更新影响。使用前请确认当前 TruckersMP 第三方插件规则。插件不读取、修改或自动化游戏数据，仅显示系统 UTC 时间。
