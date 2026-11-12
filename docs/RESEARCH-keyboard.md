# 有线键盘 + 蓝牙键盘：调研结论与实操方案（2026-11-12）

> 目标：Cardputer ADV 插到电脑上能当 USB 键盘用（有线），也能被电脑/手机蓝牙配对当键盘用（BLE）。
> 本文档 = 调研结论（§1–§5）+ 实操方案（§6）。所有结论都有本地源码或引用来源支撑。

---

## 0. 结论速览

| 问题 | 结论 |
|---|---|
| 可行性 | **两条路都可行**，不需要换 core、不需要重编 IDF、不需要引第三方库 |
| USB 键盘 | core 2.0.17 自带 `USBHIDKeyboard`（Apache-2.0）；代价是全固件要从 `ARDUINO_USB_MODE=1` 切到 `0`，烧录/串口习惯要改变（§3） |
| BLE 键盘 | core 2.0.17 自带 Bluedroid `BLEHIDDevice`（Apache-2.0），照 Evil-M5Project（MIT）的流程写；代价是 ~100 KB heap 常驻，放在独立功能启动里无所谓（§4） |
| 键盘映射 | **零工作量**：M5Cardputer 的 `KeysState.hid_keys` / `modifiers` 直接就是标准 USB HID 键码（本地核实 `Keyboard.cpp:171-197`、`_kb_asciimap`） |
| 架构契合 | 完美契合现有「每个功能一次重启」架构：KEYBOARD 成为 launcher 第 4 项，独立启动，Bluedroid 常驻也只在这次启动里 |
| 被否决项 | T-vK/ESP32-BLE-Keyboard（无 LICENSE）、wakwak-koba fork（无 LICENSE）、Bruce（AGPL-3.0 + 要 core 3.x）、NimBLE 路线（版本匹配风险，见 §4.2） |

---

## 1. 已核实的本地事实（不依赖网络）

| 事实 | 证据 |
|---|---|
| core 2.0.17 自带 `USBHIDKeyboard` / `USBHIDMouse` 等全套 HID 类，Apache-2.0 | `~/.platformio/packages/framework-arduinoespressif32/libraries/USB/src/` |
| 官方示例确认调用顺序：`Keyboard.begin()` 在前、`USB.begin()` 在后 | `libraries/USB/examples/Keyboard/KeyboardSerial/KeyboardSerial.ino` |
| core 自带 Bluedroid `BLEHIDDevice`：`reportMap()` + `inputReport(id)` + `startServices()`，构造函数自动建好 0x180A/0x1812/0x180F 三个服务 | `libraries/BLE/src/BLEHIDDevice.{h,cpp}` |
| `inputReport()` 自动附加加密读写权限 → host 一读就触发配对，iOS 可直接工作 | `BLEHIDDevice.cpp:118-120` |
| 预编译 sdkconfig：`CONFIG_TINYUSB_ENABLED=y`、`CONFIG_TINYUSB_HID_ENABLED=y`、`CONFIG_TINYUSB_CDC_ENABLED=y`、`CONFIG_BT_ENABLED=y`、`CONFIG_BLUEDROID_ENABLED=y` | `tools/sdk/esp32s3/sdkconfig` → **不用重编 IDF** |
| 板卡定义现状：`-DARDUINO_USB_MODE=1`（HW CDC+JTAG）+ `-DARDUINO_USB_CDC_ON_BOOT=1` | `boards/cardputer_adv.json` |
| M5Cardputer 键盘库 `KeysState` 直接产出标准 HID usage code 和 modifier 位掩码（内部 `_kb_asciimap`，`KEY_ENTER=0x28` 等） | `M5Cardputer/src/utility/Keyboard/Keyboard.cpp:171-197`、`Keyboard_def.h` |
| 我们自研的 `PollingKeyboardReader` 不影响 `hid_keys` 生成（`updateKeyList()` 与读取器无关） | 同上 |
| 当前固件体量：Flash 778 KB / 23.3%，RAM 33.8 KB / 10.3% | 最近一次 `pio run` |

---

## 2. 键盘数据流（两条路共用）

```
TCA8418 → PollingKeyboardReader → M5Cardputer.Keyboard.keysState()
                                        │  hid_keys[]（已是 USB HID 键码）
                                        │  modifiers（位掩码）
                    ┌───────────────────┴────────────────────┐
            USB：USBHIDKeyboard.sendReport()      BLE：inputReport(1)->setValue(8B)+notify()
```

HID 语义是"报告当前按下的键集合"，**key repeat 由 host 完成**：按下时发一次 8 字节报文（modifier + 保留字节 + 最多 6 个键码），松开时发一次全零。设备端不要定时重发。

---

## 3. USB 有线键盘：切换 USB 模式是唯一的大事

### 3.1 必须做的切换

`USBHIDKeyboard` 只在 `ARDUINO_USB_MODE=0`（USB-OTG/TinyUSB）下可用；当前固件是 mode 1（HW CDC+JTAG）。ESP32-S3 只有一个 USB PHY，二者编译期二选一。PlatformIO 干净切法（不改全局包，社区验证）：

```ini
; platformio.ini, env:cardputer-adv
build_unflags = -DARDUINO_USB_MODE=1
build_flags =
    ${env:cardputer-adv.build_flags}  ; 保留原有
    -DARDUINO_USB_MODE=0
    -DARDUINO_USB_CDC_ON_BOOT=1
board_upload.use_1200bps_touch = yes
board_upload.wait_for_upload_port = yes
```

### 3.2 切换的实际影响（逐条评估过）

| 影响面 | 结论 |
|---|---|
| 串口日志 | **保留**。mode 0 + `CDC_ON_BOOT=1` 时 `Serial` 就是 TinyUSB CDC；主机没打开端口时日志**丢弃但不阻塞**（`USBCDC.cpp:377`），固件不会卡死。RF/NFC/BACKUP 所有功能不受影响 |
| 烧录 | 自动复位改为 **1200bps touch**（host 以 1200 波特开闭 CDC 口触发 `usb_persist_restart`）；上面两行 upload 配置必须加。**兜底永远成立**：按住 G0 插 USB 进 ROM 下载模式（`303a:1001`），与 app 用什么 USB 模式无关 |
| COM 口/VID:PID | 运行口从 `303a:1001` 变为 TinyUSB 的 `303a:0002`（默认）→ **Windows 会分配新 COM 号**；烧录口（ROM 下载）仍是 `303a:1001`，两者可能不同号 |
| Bootloader 日志 | 基本丢失（ROM 日志走 USB-Serial-JTAG，app 起 TinyUSB 后该设备消失，窗口太短）。影响：`cap-serial.py` 只能看到 app 起来之后的日志——现在抓日志的绝大多数场景就是这个，可接受 |
| HID + 日志共存 | TinyUSB 复合设备天然支持 HID + CDC 双接口（官方 KeyboardSerial 示例就是这么跑的） |
| RF 采集 | 无影响：USB 模式是 app 侧的事，GDO0 中断、RMT、CC1101 都不碰 |

### 3.3 已知风险

- **TinyUSB 枚举稳定性**：IDF 4.4 内嵌的 TinyUSB 较旧，Windows 不同机器上枚举表现有差异（直插不认、过 hub 就认的案例存在）。P0 阶段必须实机验证；若不行，退路是回到 mode 1 放弃 USB 键盘（BLE 不受影响）。
- **1200bps touch 依赖当前固件的 CDC 活着**：固件崩了 touch 失效，手动 G0 兜底（写进用户文档）。

---

## 4. BLE 键盘

### 4.1 选型：Bluedroid（core 自带），不引 NimBLE

| 方案 | 结论 | 理由 |
|---|---|---|
| **core 自带 Bluedroid `BLEHIDDevice`** | ✅ **采用** | Apache-2.0；API 齐（本地核实）；Evil-M5Project 在同一块板子（Cardputer ADV）、同代 core（2.x）上就是这么做的，且 MIT 可抄 |
| NimBLE-Arduino + 自写胶水 | 备选 | 省 ~70 KB heap、重连快（200ms vs 600-700ms），但 1.4.x 才配 IDF 4.4（老），2.x 要 core 3.x；现有键盘封装要么无 LICENSE 要么要 core 3.x。heap 不够时再回来 |
| T-vK / wakwak-koba | ❌ | 无 LICENSE 文件，只能读流程不能拷 |
| Bruce | ❌ | AGPL-3.0 + 要 pioarduino/core 3.x |

### 4.2 实现流程（与 Evil-M5Project `initBluetoothKeyboard()` 一致）

```cpp
BLEDevice::init("Cardputer-KB");
BLEServer* pServer = BLEDevice::createServer();
pServer->setCallbacks(new ServerCB());        // 断开后重启 advertising
BLEHIDDevice* hid = new BLEHIDDevice(pServer);
BLECharacteristic* in = hid->inputReport(1);  // ID 与 report map 一致
hid->manufacturer()->setValue("M5Stack");
hid->pnp(0x02, vid, pid, 0x0110);
hid->hidInfo(0x00, 0x01);
hid->reportMap((uint8_t*)kbdMap, sizeof(kbdMap));  // 标准 8B boot keyboard + LED
hid->startServices();
auto* adv = pServer->getAdvertising();
adv->setAppearance(HID_KEYBOARD);             // 0x03C1
adv->addServiceUUID(hid->hidService()->getUUID());  // 0x1812，必须有否则不认成键盘
adv->start();
// 安全：JustWorks + Bonding
auto* sec = new BLESecurity();
sec->setAuthenticationMode(ESP_LE_AUTH_BOND);
sec->setCapability(ESP_IO_CAP_NONE);
// 发键：in->setValue(report, 8); in->notify();   松开：全零
```

要点：

- **report map**：标准 8 字节 boot keyboard 报文（1 modifier + 1 保留 + 6 键码）+ LED output 段，HID 规范附录的标准键盘描述符（公版常量，可用 esp-idf 示例里那份，Apache-2.0）。
- **配对**：JustWorks（`ESP_LE_AUTH_BOND` + `ESP_IO_CAP_NONE`），Windows/Android/macOS 都静默成功。passkey 留作编译开关（Windows 对 passkey 兼容性差）。
- **heap 开销**：Bluedroid HID 起来后约吃掉 ~100 KB（T-vK 实测剩 ~140 KB）。我们键盘功能是独立启动（不挂 SD、不开 CC1101/NFC），预算 260 KB+ 够用。
- **不要 `BLEDevice::deinit()`**：IDF 4.4 上 deinit 破坏 WiFi/BT 共存（Evil 代码里的血泪注释）。初始化一次、常驻，退出功能靠重启。
- **键间节拍 7-25 ms**，密集 notify 部分 host 处理不过来。
- **断连重连**：`onDisconnect` 里重启 advertising；换固件/换名后 host 端 bond 缓存会害人——测试期记得"删除设备重配"。

### 4.3 各 host 已知坑

- Windows 缓存 HID 描述符：改 report map 后必须删设备重配。
- iOS 缓存设备名；连了物理键盘会隐藏屏幕键盘。
- Bluedroid 在 S3 上的 HID 断连/重连慢是 IDF 4.4 固有代差（v5.1.2 才修），发作就多删配几次。

---

## 5. 参考项目清单

| 项目 | License | 用处 |
|---|---|---|
| **Evil-M5Project** | **MIT** | **主参考**：同板同 core 代的 USB + BLE 键盘双实现；BLE 用 core 自带 Bluedroid，无第三方库；按键直接吃 `keysState().hid_keys` |
| arduino-esp32 官方 USB 示例 | Apache-2.0 | USBHIDKeyboard 用法、复合设备 |
| core 自带 BLEHIDDevice | Apache-2.0 | BLE 实现本体 |
| esp-idf HID 示例 | Apache-2.0 | 标准键盘 report descriptor 字节表 |
| T-vK/ESP32-BLE-Keyboard | **无 LICENSE** | 只读流程（begin 序列、2902 置位、节拍参数），不拷代码 |
| Bruce | AGPL-3.0 | 只读思路（NimBLE + core 3.x，不可复用） |
| HijelHID_BLEKeyboard | Apache-2.0 | 功能最全但要求 core 3.x + NimBLE 2.3.8，只读思路 |
| xyb/aikb | MIT | Cardputer BLE 键盘，但依赖无 LICENSE 的 T-vK 库，不用 |

---

## 6. 实操方案（分阶段，每阶段都可回退）

> 原则：每阶段一个可烧录验证的里程碑；任何阶段翻车都能 `git checkout` 回退 + G0 刷回。

### P0 · USB 模式切换 spike（纯配置，不写键盘代码）

1. `platformio.ini` 按 §3.1 改 `env:cardputer-adv`（unflags + flags + upload 两行）。
2. 编译烧录，验证四件事：
   - RF TOOLS / NFC TOOLS / BACKUP 行为不变（尤其 NFC 模拟还能开门、TF 还能挂）；
   - Windows 给新的 COM 号，串口日志能看到（TinyUSB CDC）；
   - `pio run -t upload` 的 1200bps touch 自动复位可用；
   - 按住 G0 插 USB 的 ROM 兜底可用（烧回原固件验证一次）。
3. 记录新 VID:PID 和 COM 号，更新 `tools/run-device.ps1` 的注释。

**回退**：git 恢复 platformio.ini 重刷即可。**若 TinyUSB 枚举在你主力机器上不稳，整个 USB 键盘功能降级为"仅 BLE"，后续 P2 不受影响。**

### P1 · KEYBOARD 功能骨架 + USB HID

1. launcher 加第 4 项 `KEYBOARD`（左侧菜单，不动九宫格）；新建 `src/kbd_app.h/.cpp`，照 nfc_app 的模式（独立启动、esc 回菜单）。
2. 页面：大字状态（`USB KEYBOARD` / `waiting host`）、最近按键回显、底栏提示。
3. `setup()`：`Keyboard.begin(); USB.begin();`（VID/PID 用默认或自定义 `Cardputer-KB`）；`loop()`：diff 上一帧 `keysState()`，变了就组 8 字节报文 `Keyboard.sendReport()`。
4. 测试：插 Windows 记事本打字、锁屏密码框、BIOS 界面（BIOS 是对 HID 最挑剔的场景）。

### P2 · BLE HID

1. 同一 `kbd_app` 里加模式选择页：`USB` / `BLE`（进入时选定，或 ENTER 切换——Bluedroid 起来就不 deinit，切换靠重启本功能）。
2. 按 §4.2 写 `ble_hid.cpp`：初始化、advertising、连接回调、8 字节报文转发；JustWorks。
3. 页面状态：`ADVERTISING` / `CONNECTED <host>` / 信号回显。
4. 测试：Windows、Android、iOS（如有）、macOS（如有）各配对打字一遍；断开后自动回 advertising。

### P3 · 收尾

- Caps Lock LED 回读（可选，`outputReport` onWrite，屏上显示 CAPS 状态）。
- 首页九宫格要不要支持"绑定一段文本一键发送"（macro）——**新需求，单独立项**，不在本期。
- 文档：`docs/README.md` 用户文档加键盘章节；`tools/run-device.ps1` 适配新 COM；DEV-STATE 更新。
- 清理：P0 若确认稳定，把 `boards/cardputer_adv.json` 里 mode 1 的历史注释改掉。

### 工作量预估

| 阶段 | 内容 | 预估 |
|---|---|---|
| P0 | ini 改动 + 四项回归 + 烧录验证 | 小（半天，主要是实机验证） |
| P1 | kbd_app 骨架 + USB HID | 小（一天内，示例路径清晰） |
| P2 | BLE HID | 中（1-2 天，坑在配对体验和断连） |
| P3 | 打磨 | 按需 |

### 硬约束复核（不违反既有规矩）

- 不动 `espressif32@6.13.0` 平台锁 ✅（全部在 core 2.0.17 内解决）
- 不动分区表、不擦 NVS ✅（键盘功能无持久化需求；将来 macro 存 SPIFFS）
- C++11 ✅
- 射频互斥 ✅（KEYBOARD 启动不碰 CC1101/ST25R3916/SD；BLE 射频与 sub-GHz 无共享资源，Evil 在 ADV 上已实证 TCA8418+Bluedroid 共存）

---

## 7. 开放问题（实机才能答）

1. TinyUSB 在主力 Windows 机器上的枚举稳定性（P0 验证，决定 USB 键盘生死）。
2. 1200bps touch 在我们这套板卡定义上的实际成功率。
3. Bluedroid 驻留后 heap 余量实测（预期 >120 KB，够）。
4. iOS/macOS 配对体验（手头有设备才测得了）。
