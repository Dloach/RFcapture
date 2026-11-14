# Cardputer ADV + CAP.CC1101：RF / NFC / 键盘 / WiFi / SSH / 文件 / BadUSB / U 盘

M5Stack Cardputer ADV 上，用 **CAP.CC1101**（CC1101 + ST25R3916）和板载键盘做：

- **RF**：采集、保存并重发铁门 / 卷帘门 / 道闸等 OOK 遥控器（EV1527 / PT2262 固定码）；
- **NFC**：读取 NFC-A 卡、保存 UID/ATQA/SAK，并把其中一张放回空中（模拟开门）；
- **KEYBOARD**：USB 有线 HID 键盘，或 BLE 无线 HID 键盘（`Cardputer-KB`）；
- **SETTINGS**：WiFi 扫描/保存、OTA 无线烧录、TF 备份/恢复、亮度/超时/电池；
- **SSH**：在 WiFi 上连 SSH 服务器，带 40×13 终端；
- **FILES**：TF / SPIFFS 文件浏览、文本查看、删除；
- **BADUSB**：从 TF 读取 DuckyScript 子集，作为 USB HID 键盘执行；
- **USB DISK**：把整张 TF 卡作为 USB MSC 盘暴露给电脑。

系统是 launcher 八功能（可滚动）+ 右侧 3×3 快捷九宫格；每个功能一次重启，避免 CC1101
（SPI 模式 0）、ST25R3916（模式 1）和 TF 卡（模式 0）在同一总线上互相锁死。

## 功能

- **RF TOOLS**
  - 采集：CC1101 异步 OOK，GDO0 → GPIO 中断 + `micros()`，1 µs 分辨率；
  - 解码：EV1527（24 bit）、PT2262（12 位三态）；
  - 每条采集带 `q00`–`q99` 波形质量分；同码重采质量明显更好时会自动升级波形；
  - 永久保存：采集页 `ENTER` 确认后写入 NVS，掉电不丢，可改名；
  - 回放：发送页选中后 `S`，原始波形零间隙背靠背重发；
  - `SCAN RF`：7 个频点扫 RSSI，找遥控器。
- **NFC TOOLS**
  - `READ CARDS`：扫描并入库（UID / ATQA / SAK / 类型）；
  - `CARD LIST`：选中 `ENTER` 模拟、`B` 绑定九宫格、`D` 删除、`N` 改名；
  - 模拟配方见 `docs/DEV-STATE.md` §14.2。
- **SETTINGS → BACKUP / RESTORE**
  - 一次把 RF 永久信号、NFC 卡库、九宫格绑定写到 TF 卡；
  - `RESTORE FROM TF` 恢复三份数据（会先确认）。
- **KEYBOARD**
  - `USB / WIRED`：TinyUSB 复合设备（HID + CDC），插 PC 直接当键盘；
  - `BLUETOOTH`：BLE HID，主机名 `Cardputer-KB`，支持 bonded 重连；
  - 工作页显示 4×14 键盘映射，按键/修饰键实时高亮；`Opt` = Win/Command；
  - `Fn+1..0` = F1..F10，`Fn+-`/`Fn+=` = F11/F12，`Fn+; . , /` = 方向键，
    `Fn+`` = Esc，`Fn+BKSP` 返回 KEYBOARD 菜单。
- **SETTINGS → WIFI**
  - 异步扫描 2.4 GHz AP，密码输入，连接成功后写入 NVS；
  - 保存最多 5 个网络，启动时自动连上次成功的网络；
  - `TEST CONNECTION` 用 `example.com:80` 验证 DNS/外网 TCP。
- **SETTINGS → OTA UPDATE**
  - 基于 ArduinoOTA 的无线烧录，`cardputer.local:3232`，密码 `cardputer-ota`；
  - 第一次仍需 USB，之后 `pio run -e cardputer-ota -t upload`；
  - 详细步骤见 [docs/OTA.md](docs/OTA.md)。
- **SETTINGS → DISPLAY / POWER**
  - 背光 10–100%（NVS `ui/bright`）；
  - 屏幕超时 OFF/15/30/60/120 秒（`ui/timeout`），按键唤醒；
  - 显示电池百分比、电压、是否充电。
- **FILES**
  - TF / SPIFFS 切换，目录浏览，文本查看；
  - `D` 连按两次删除文件，不支持目录递归删除；
  - 详细说明见 [docs/LOCAL.md](docs/LOCAL.md)。
- **BADUSB**
  - 从 TF `/badusb/*.txt` 读取 DuckyScript 子集；
  - USB HID 键盘输出，仅限授权设备；
  - 支持 `STRING / DELAY / DEFAULTDELAY / ENTER / TAB / ESC / 方向键 / F1–F12 / 组合键`。
- **USB DISK**
  - 独立启动模式，把 TF 卡作为 USB MSC 盘；
  - 电脑直接挂载、复制、删除文件；
  - 退出前必须在电脑上安全弹出；详细见 [docs/LOCAL.md](docs/LOCAL.md)。
- **SSH**
  - 自动使用 SETTINGS 里保存的 WiFi；
  - 最多 4 个 profile（NAME / HOST / PORT / USER / AUTH / PASS|KEY），`+ NEW PROFILE` 新建；
  - 密码认证；Ed25519 key 代码保留在仓库中，但默认固件关闭；
  - 首次 host key 显示 SHA256 指纹确认；
  - 编辑页 `Opt+, /` 移光标、光标处插入、闪烁 `_`；
  - 40×13 终端，ANSI 16 色、UTF-8 回退、64 行历史，`Opt+; .` 翻页；
  - 15 s 超时、30 s keepalive；`Fn+BKSP` 退出会话，`Fn+Q` 关闭会话。
  - 使用说明见 [docs/SSH.md](docs/SSH.md)。

> 实测：315 MHz EV1527 遥控器（T≈500 µs）采集 → 保存 → 回放，铁门正常动作；
> 门卡 `3C02B611` 的模拟已验证能开门。**码值一样不代表波形一样好**：`q40` 以下
> 基本发不动，优先重采，而不是只改名字。

```text
   遥控器 ──315/433/868MHz──▶ CAP.CC1101 ──GDO0──▶ 中断采样 ──▶ 解码 ──▶ 采集列表
                                                                        │ ENTER
                                                                  永久保存（NVS）
                                                                        │
                                                      发送页 S ──▶ 零间隙连发 ──▶ 天线

   NFC 卡 ──13.56MHz──▶ ST25R3916 ──▶ UID/ATQA/SAK ──▶ 卡库 ──▶ CARD LIST
                                                                  │ ENTER
                                                            重启进入模拟（EMULATE）
```

## 快速开始

Windows：

```powershell
pio run -e cardputer-adv -t upload --upload-port COM13
.\tools\run-device.ps1 -Port COM13
```

> **USB mode 0（键盘功能）**：从这一版起，USB 是 TinyUSB 复合设备（HID + CDC），
> 不再是硬件 CDC/JTAG。运行口/VID:PID 会变，烧录使用 1200bps touch；如果 Windows
> 找不到新口，按住键盘左上角 **G0** 再插 USB 进 ROM 下载模式，用 `esptool` 直接写。
> 具体 COM 号以 `pio device list` 为准。

macOS / Linux：

```bash
pio run -e cardputer-adv -t upload --upload-port /dev/cu.usbmodem101
./tools/run-device.sh           # 自动找串口，编译 → 烧录 → 复位 → 看日志
```

## 按键

无需 Fn；`; , . /` 就是键帽上橙色箭头那四个：

- `;` / `.` 上下，`,` / `/` 左右，`` ` ``（左上角 esc）或 `BKSP` 返回，`ENTER` 确认；
- 九宫格键：`8 9 0 / I O P / J K L`，在 launcher 上直接触发绑定；
- RF 采集页：`ENTER` 永久保存、`S` 直接发、`F` 换频段、`D` 删除、`C` 连按两次清空；
- RF 发送页：`S` 发送、`N` 改名、`D` 删除、`B` 绑定九宫格；
- NFC 列表：`ENTER` 模拟、`B` 绑定、`D` 删除、`N` 改名；
- NFC 扫描：`S` 开始/停止；
- KEYBOARD 模式页：`ENT` 进入选中模式、`esc` 回 launcher；
- KEYBOARD 工作页：`Fn+BKSP` 回 KEYBOARD 菜单；`Fn+`` = Esc；`Opt` = Win。
- launcher：`; .` 上下滚动八项；`ENT` 进入；右侧九宫格照旧。
- SETTINGS / WIFI：`S` 扫描，`ENT` 选择，密码页 `ENT` 连接、`BKSP` 删除、
  `Fn+BKSP` 取消；`TEST CONNECTION` 验证外网 TCP。
- SSH：`; .` 选字段，`ENT` 编辑/连接；终端里 `Fn+; . , /` = 方向键，
  `Fn+BKSP` 退出，`Fn+Q` 关闭会话。

## 目录

```text
cardputer/
├── platformio.ini              env: cardputer-adv / diag-off / example
├── boards/cardputer_adv.json   板卡定义（8 MB Flash，无 PSRAM）
├── src/
│   ├── main.cpp                启动分发
│   ├── launcher.*              launcher 八功能（滚动）+ 九宫格
│   ├── app.*                   RF 首页 / 采集 / 发送 / SCAN
│   ├── nfc_app.*               NFC 读卡 / 列表 / 模拟
│   ├── nfc_cards.*             NFC 卡库（SPIFFS + TF 备份）
│   ├── kbd_app.*               USB / BLE 键盘 + 映射页
│   ├── settings_app.*          SETTINGS / WiFi 扫描、连接、测试
│   ├── wifi_store.*            WiFi 凭据（NVS，最多 5 个）
│   ├── ssh_app.*               SSH profile / 认证 / 会话
│   ├── ssh_terminal.*          40×13 VT100 终端缓冲
│   ├── backup_app.*            BACKUP / RESTORE
│   ├── bind_picker.*           九宫格绑定的模态选择器
│   ├── quick_bind.*            九宫格绑定表（SPIFFS /quick.txt）
│   ├── cc1101.*                CC1101 驱动（SPI、频段、收发、PATABLE）
│   ├── ook_rx.*                GPIO 中断采集脉冲 + 成帧
│   ├── ook_decoder.*           协议解析 + 波形质量分
│   ├── ook_tx.*                RMT 基带发送
│   ├── capture_store.*         采集列表（NVS）
│   ├── saved_store.*           永久保存列表（NVS，带名字）
│   ├── tf_backup.*             RF 信号的 TF 备份 / 恢复
│   ├── sd_mount.h              共享 SD 挂载
│   ├── polling_keyboard.h      TCA8418 轮询键盘
│   ├── trace.*                 最近 8 帧存 RTC 内存
│   ├── ui_hint.h               footer 三角
│   └── rf_frame.h / rf_config.h
├── retry/                      独立 NFC 参考工程（已验证配方，非出货固件）
├── tests/host/                 解码器 / 绑定 key / 存储恢复的 PC 端测试
├── tools/                      烧录、串口抓取
└── docs/
    ├── README.md               详细使用文档
    ├── DEV-STATE.md            开发状态与踩坑记录（§0 是当前真相）
    ├── STATE-2026-11-12.md     某一晚的状态快照
    ├── PLAN-fix-2026-11-12.md  修复计划与执行记录
    ├── PLAN-keyboard-next.md   键盘功能的执行计划
    ├── PLAN-wifi-ssh.md        WiFi Settings + SSH 客户端开发计划
    ├── SSH.md                  SSH profile / host key / 终端说明
    ├── OTA.md                  无线烧录步骤与故障排查
    ├── LOCAL.md                FILES / BADUSB / 显示与电源
    ├── THIRD-PARTY.md          第三方组件与 LGPL 注意事项
    └── RESEARCH-keyboard.md    键盘调研
```

## 路线图

WiFi Settings 和 SSH MVP 已落地：扫描/保存/开机自动连接、外网 TCP 测试、
单 profile 密码 SSH、40×13 终端。后续是 SSH 多 profile、Ed25519 key、
host key 变更提示、终端 ANSI 补全和滚动历史；详见
[docs/PLAN-wifi-ssh.md](docs/PLAN-wifi-ssh.md)。

## 测试

host 测试覆盖 EV1527 / PT2262 解码、基准时间估算、波形质量分、九宫格 key、
同码波形替换策略、永久列表撕裂恢复：

```bash
./tests/host/run.sh             # macOS / Linux
.\tests\host\run.ps1            # Windows
```

需要一个本机 C++17 编译器（g++ / clang++ / MSVC）。脚本也认 `tools/zig/zig.exe`
和 PATH 上的 `zig`。当前为 **57 项全过**（43 解码/绑定/存储 + 14 SSH 终端）；
PlatformIO 的 Xtensa 交叉编译器不能跑这些测试。

## 合规

仅采集自己拥有或获授权的设备；不包含滚动码破解或双向认证绕过。
