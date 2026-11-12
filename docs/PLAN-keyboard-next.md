# 下一步执行计划：有线 / 蓝牙键盘（交接版，2026-11-12）

> 这份文档是给**下一个接手执行的 AI 或人**看的：照做即可，不需要重新调研。
> 调研论证在 `docs/RESEARCH-keyboard.md`（为什么这么做），本文档只写**怎么做**。
> 当前代码状态：tag `v1.0.0` 之后，工作区干净。

---

## 0. 环境与硬约束（先读，违反会烧时间）

| 项 | 约束 |
|---|---|
| 工程目录 | `Y:\2026\cardputer`（唯一权威目录；`G:\Projects\cardputer` 是旧拷贝，别动） |
| 平台 | `espressif32@6.13.0` **锁死**（Arduino core 2.0.17 / IDF 4.4），升 8.x 会黑屏 |
| 语言 | `src/` 按 **C++11** 编译 |
| 构建/烧录 | `pio run -e cardputer-adv -t upload --upload-port COM10`（Windows 用 `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`） |
| 烧录后 | 板子可能停在 ROM 下载模式：`.\tools\run-device.ps1 -Env cardputer-adv -NoBuild` 会做 watchdog 复位；或拔插 USB |
| 进下载模式兜底 | **按住键盘左上角 G0 键再插 USB** |
| NVS | **禁止 erase_flash / 擦 NVS / 改分区表**（里面有不可再生的 RF 采集记录） |
| 串口抓取 | `tools/cap-serial.py`（pyserial，用 pio 自带 python 跑）；COM 口可能变化，先 `python -c "from serial.tools import list_ports; print([p.device for p in list_ports.comports()])"` |

### 硬件事实（CAP.CC1101 + 主板）

- SPI 共享总线：SCK=G40 / MOSI=G14 / MISO=G39；CC1101 CS=G5、ST25R3916(NFC) CS=G6、TF 卡 CS=G12、GDO0=G15、模块电源 POWER_EN=G3、ST25R3916 IRQ=G4
- **ST25R3916 的 13.56MHz 载波跨 ESP32 重启不丢**：挂 SD 前必须对它发 `CMD_SET_DEFAULT`（`sd_mount.h::ensure()` 已做）；离开 NFC 功能必须灭场（`nfc_app.cpp` 已做）。**新功能若碰 SPI/SD，先看这两个文件**
- 键盘控制器 TCA8418 怕高频 I2C（每帧读电量会打死它，只能断电恢复）；键盘读取用 `PollingKeyboardReader`（`src/polling_keyboard.h`），**不要用库自带的中断读取器**，替换前必须 `detachInterrupt(digitalPinToInterrupt(RF_KBD_INT))`
- 模态循环必须喂任务看门狗：`esp_task_wdt_reset()`（RF app 装了 6s TWDT）

### 架构（必须遵守）

- **每个功能一次重启**：launcher 写 NVS("appboot") + `ESP.restart()` 进对应功能；冷启动回菜单（`launcher.cpp::currentApp()`）。新功能 = 新的菜单项 + 新的 `xxx_app.cpp` + main.cpp 里一个 case
- 页面直绘（LovyanGFX），footer 提示用 `src/ui_hint.h` 的 `drawMoveTriangles()` 画 ▲▼；按键约定：`;`=上、`.`=下、`,`=左、`/`=右、ENTER 确认、`` ` ``/BKSP 返回
- 首页右侧已有快捷九宫格（`src/quick_bind.*`），新功能别破坏它

---

## 1. 功能定义

launcher 加第 4 项 **KEYBOARD**：

- **USB 模式**：USB-C 插电脑，Cardputer 键盘直接当 USB HID 键盘打字
- **BLE 模式**：电脑/手机蓝牙配对 `Cardputer-KB`，打字无线输入
- 进入时二选一（模式切换 = 重启本功能），页面显示状态 + 最近按键回显，esc 回菜单

数据流（两条路共用，映射已免费）：

```
TCA8418 → M5Cardputer.Keyboard.keysState()
   → .hid_keys（已是标准 USB HID 键码）+ .modifiers（位掩码）
   → USB: USBHIDKeyboard.sendReport()  /  BLE: inputReport(1)->setValue(8字节)+notify()
```

HID 语义：报"当前按下的键集合"，**repeat 由 host 做**；按下组一次 8 字节报文（modifier + 保留 + ≤6 键码），松开发全零。

---

## 2. P0 · USB 模式切换（先做，纯配置，决定 USB 键盘生死）

改 `platformio.ini` 的 `[env:cardputer-adv]`：

```ini
build_unflags = -DARDUINO_USB_MODE=1
build_flags =
    -DCORE_DEBUG_LEVEL=0
    -Wno-unused-variable
    -DRF_DIAG=0
    -DARDUINO_USB_MODE=0
    -DARDUINO_USB_CDC_ON_BOOT=1
board_upload.use_1200bps_touch = yes
board_upload.wait_for_upload_port = yes
```

验证（全部通过才进 P1）：

1. RF TOOLS / NFC TOOLS / BACKUP 行为不变（**NFC 模拟贴一次门、BACKUP 挂一次 TF**）
2. 串口日志还能看到（Serial 变成 TinyUSB CDC；主机不开端口时日志丢弃但不阻塞，属正常）
3. `pio run -t upload` 的 1200bps touch 自动复位可用
4. 按住 G0 插 USB 的 ROM 兜底可用，能刷回 mode 1 的旧固件（git 恢复 ini 重刷）

记录：新 VID:PID（默认 `303a:0002`）、新 COM 号。**若 TinyUSB 在你主力机器上枚举不稳，放弃 USB 键盘只做 BLE（P2 不受影响），在文档里记一笔。**

---

## 3. P1 · kbd_app 骨架 + USB HID

1. `src/kbd_app.h/.cpp`，照 `nfc_app.cpp` 的结构：`kbdapp::setup()/loop()`；launcher 菜单加第 4 项（`launcher.h` 的 `App` 枚举加 `kAppKbd=4`，`launcher.cpp` 菜单数组、`rebootInto` 名称表、`main.cpp` 分发各加一行）。
2. `setup()`：
   ```cpp
   #include "USB.h"
   #include "USBHIDKeyboard.h"
   USBHIDKeyboard Keyboard;
   Keyboard.begin();
   USB.begin();
   ```
3. `loop()`：缓存上一帧 `KeysState`，变了就发：
   ```cpp
   auto ks = M5Cardputer.Keyboard.keysState();
   if (ks != prev) {  // 自行 diff hid_keys/modifiers
       KeyReport r{}; r.modifiers = ks.modifiers;
       for (i<6) r.keys[i] = i < ks.hid_keys.size() ? ks.hid_keys[i] : 0;
       Keyboard.sendReport(&r);
   }
   ```
4. 页面：状态行（`USB KEYBOARD` / 枚举失败提示）、最近按键回显、footer 提示。
5. 测试：Windows 记事本、锁屏密码框、BIOS 界面（最挑剔）。

注意：`M5.Power.getBatteryLevel()` 别每帧调（3 秒缓存，TCA8418 会死，见 §0）。

---

## 4. P2 · BLE HID

同一 `kbd_app` 加模式选择（USB / BLE），**用 core 自带 Bluedroid，不引 NimBLE、不引 T-vK**：

```cpp
BLEDevice::init("Cardputer-KB");
auto* pServer = BLEDevice::createServer();
pServer->setCallbacks(new ServerCB());   // onDisconnect 里 adv->start() 重来
auto* hid = new BLEHIDDevice(pServer);
auto* in  = hid->inputReport(1);         // ID 与 report map 里的 0x85 一致
hid->manufacturer()->setValue("M5Stack");
hid->pnp(0x02, 0x03C1, 0x8200, 0x0110);
hid->hidInfo(0x00, 0x01);
hid->reportMap((uint8_t*)kbdMap, sizeof(kbdMap));
hid->startServices();
auto* adv = pServer->getAdvertising();
adv->setAppearance(HID_KEYBOARD);        // 0x03C1
adv->addServiceUUID(hid->hidService()->getUUID());
adv->start();
auto* sec = new BLESecurity();
sec->setAuthenticationMode(ESP_LE_AUTH_BOND);   // JustWorks
sec->setCapability(ESP_IO_CAP_NONE);
```

- report map 用标准 8 字节 boot keyboard 描述符（HID 规范附录的公版常量；esp-idf 示例里那份是 Apache-2.0）
- 发键：`in->setValue(report, 8); in->notify();`；松开发全零；键间留 7–25ms
- **绝不调 `BLEDevice::deinit()`**（IDF 4.4 上破坏 coex）；退出功能靠重启
- 连接回调里把 input report 的 2902 置 `setNotifications(true)`（部分 host 需要）
- 测试矩阵：Windows / Android /（有的话）iOS、macOS；改 report map 或设备名后**必须 host 端删设备重配**（bond 缓存坑）

---

## 5. P3 · 收尾（可选）

- Caps Lock LED 回读：`hid->outputReport(1)` 挂 onWrite，屏上显示 CAPS
- 文档更新：`docs/README.md`、`tools/run-device.ps1` 注释（新 COM/1200bps）
- 一键发送文本 macro（九宫格扩展）——**新需求，另立项**

## 6. 验收标准

1. USB 插电脑：记事本能打出大小写、数字、符号、回车、退格，10 分钟不断
2. BLE：Windows 和 Android 都能配对、打字、断开后自动回 advertising 可重连
3. 键盘功能进出（esc 回菜单）5 次无卡死、无冻屏
4. 回归：RF 采集回放、NFC 开门、BACKUP 挂卡全部正常
5. `git tag v1.1.0`

---

## 7. 已知坑速查（踩过了就别再踩）

| 坑 | 事实 |
|---|---|
| 模态循环不喂狗 | RF app 有 6s TWDT，模态循环必须 `esp_task_wdt_reset()` |
| 键盘 I2C 高频 | 电量 3 秒缓存；键盘更新走 `M5Cardputer.update()` 常规频率即可 |
| ST25R3916 载波 | 挂 SD 前 `CMD_SET_DEFAULT`；离开 NFC 灭场（已内置，别绕过） |
| BLE deinit | IDF 4.4 上禁调，退出靠重启 |
| host bond 缓存 | 换固件/改名后配对失败 → host 删设备重配 |
| TinyUSB 枚举 | IDF 4.4 内嵌版本旧，主力机器实测不过就只做 BLE |
| 串口抓不到开机日志 | USB-CDC 每次复位重枚举，用 `tools/cap-serial.py` 自动重连 |
