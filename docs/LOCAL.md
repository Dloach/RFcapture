# FILES / BADUSB / DISPLAY-POWER

这三个功能都不碰 CC1101 / ST25R3916：FILES 和 BADUSB 只初始化 TF 卡，
DISPLAY / POWER 是 SETTINGS 内部页。

## FILES

入口：launcher → **FILES**。

- `;` / `.` 选择，`ENT` 进入目录 / 打开文本；
- 文本查看页 `;` / `.` 滚动，`esc` 返回；
- `D` 连按两次删除**文件**；目录不递归删除；
- `S` 在 **TF** 和 **SPIFFS** 之间切换；
- 根目录按 `esc` 回 launcher；
- 文件大小以字节显示；最多列出 64 项。

限制：

- 只读文本前 80 行、每行 40 字符；
- 二进制文件显示为空/不可读；
- 不支持改名、复制、移动、新建目录；
- SPIFFS 没有真正的目录层级，切换后按根目录处理。

## BADUSB

入口：launcher → **BADUSB**。

首次进入是授权警告，`ENT` 接受，`esc` 退出。脚本放在 TF 卡：

```text
/badusb/*.txt
```

选中脚本 `ENT` 执行；执行中 `Fn+BKSP` / `` ` `` 中止。

### 支持的 DuckyScript 子集

```text
REM comment
DELAY 500
DEFAULTDELAY 30
STRING Hello from Cardputer
ENTER
TAB
ESC
BACKSPACE
SPACE
UP / DOWN / LEFT / RIGHT
HOME / END / PAGEUP / PAGEDOWN
DELETE
CAPSLOCK
F1 .. F12
CTRL / SHIFT / ALT / GUI + key
```

示例：

```text
REM open Run dialog and type a command
DELAY 500
GUI r
DELAY 500
STRING notepad
ENTER
DELAY 800
STRING hello
```

注意：

- **仅用于你拥有或明确授权的设备**；
- 当前只支持 US 键盘布局；
- 没有变量、条件、循环、文件读取等完整 DuckyScript 语义；
- 执行时 Cardputer 作为 USB HID 键盘，目标 PC 若弹出信任/驱动提示需先处理；
- 不要在不可信主机上运行脚本。

## SETTINGS → DISPLAY / POWER

- **BRIGHTNESS**：10–100%，`,` / `/`（或左右键）调整；
- **SCREEN TIMEOUT**：OFF / 15 / 30 / 60 / 120 秒；
- 超时后关闭背光，任意按键恢复；
- 页面同时显示电池百分比、电压、充电状态；
- 配置存 NVS `ui`：`bright`、`timeout`。

当前是**关背光**，不是 deep sleep；WiFi/SSH 连接在超时期间保持。

## 参考实现

这些项目用于对照交互和 DuckyScript 子集，不是直接复制代码：

- [MeowUSB-BLE](https://github.com/01Mayo01/MeowUSB-BLE)：Cardputer USB/BLE DuckyScript executor；
- [WUD-Ducky](https://github.com/tobozu/WUD-Ducky)：ESP32 DuckyScript parser；
- [CardPDA](https://github.com/RockSneeze081/CardPDA)：Cardputer SD 文件浏览器/模块化 OS；
- [M5Launcher](https://github.com/smoochiee/M5Stick-Launcher)：Cardputer SD 文件管理与 OTA 交互参考；
- M5Unified `Power_Class`：`getBatteryLevel()` / `getBatteryVoltage()` / `isCharging()`。
