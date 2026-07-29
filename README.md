# TruckersMP UTC 时间显示 DLL

这是一个供 Euro Truck Simulator 2 / TruckersMP 使用的 x64 Telemetry 插件。它在游戏窗口底部居中显示 UTC 实时时钟，格式与参考图一致：

```text
Current Time: 2026-Jul-29 17:57:44 UTC
```

插件只使用 SCS 官方 Telemetry 加载入口和 Windows 透明窗口，不注入或 Hook DirectX。文字层会跟随游戏窗口、允许鼠标穿透，并在切换到其他程序时自动隐藏。

## 安装

1. 将 `tmp_time_overlay.dll` 和 `tmp_time_overlay.ini` 放到：

   ```text
   <Euro Truck Simulator 2 安装目录>\bin\win_x64\plugins\
   ```

2. 没有 `plugins` 文件夹时自行新建。
3. 使用“无边框窗口”或“窗口”显示模式启动 TruckersMP。独占全屏模式无法保证 Windows 透明层可见。
4. 游戏启动时如果出现“检测到高级 SDK”的提示，确认继续即可。

## 配置

编辑 DLL 旁边的 `tmp_time_overlay.ini`，重启游戏后生效：

- `font_size`：字号，范围 10 到 72。
- `font_name`：Windows 字体名称。
- `bottom_margin`：文字与游戏窗口底边的距离。
- `text_color` / `shadow_color`：六位 RGB 十六进制颜色。
- `hide_when_unfocused`：设为 `1` 时，切出游戏后隐藏。
- `show_prefix`：设为 `1` 时显示 `Current Time:` 前缀。

## 构建

需要 Visual Studio 2022 Build Tools，并安装“使用 C++ 的桌面开发”工作负载。在 PowerShell 中执行：

```powershell
.\build.ps1
```

成品会生成到 `dist` 文件夹。

## 注意

此项目不读取、修改或自动化任何游戏数据，仅显示系统 UTC 时间。TruckersMP 的第三方插件政策可能变化，使用前应确认当前规则。若 DLL 没有加载，请检查 `Documents\Euro Truck Simulator 2\game.log.txt` 中的插件错误。
