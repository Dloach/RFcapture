# Cardputer ADV + CAP.CC1101：RF 采集回放 + NFC 读卡/模拟

M5Stack Cardputer ADV 上，用 **CAP.CC1101**（CC1101 + ST25R3916）做两件事：

- **RF**：采集、保存并重发铁门 / 卷帘门 / 道闸等 OOK 遥控器（EV1527 / PT2262 固定码）；
- **NFC**：读取 NFC-A 卡、保存 UID/ATQA/SAK，并把其中一张放回空中（模拟开门）。

系统是 launcher 三功能 + 右侧 3×3 快捷九宫格；每个功能一次重启，避免 CC1101
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
- **BACKUP**
  - 一次把 RF 永久信号、NFC 卡库、九宫格绑定写到 TF 卡；
  - `RESTORE FROM TF` 恢复三份数据（会先确认）。

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
pio run -e cardputer-adv -t upload --upload-port COM10
.\tools\run-device.ps1          # 上传后自动复位并看日志
```

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
- NFC 扫描：`S` 开始/停止。

## 目录

```text
cardputer/
├── platformio.ini              env: cardputer-adv / diag-off / example
├── boards/cardputer_adv.json   板卡定义（8 MB Flash，无 PSRAM）
├── src/
│   ├── main.cpp                启动分发
│   ├── launcher.*              launcher 三功能 + 九宫格
│   ├── app.*                   RF 首页 / 采集 / 发送 / SCAN
│   ├── nfc_app.*               NFC 读卡 / 列表 / 模拟
│   ├── nfc_cards.*             NFC 卡库（SPIFFS + TF 备份）
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
    └── RESEARCH-keyboard.md    键盘调研
```

## 测试

host 测试覆盖 EV1527 / PT2262 解码、基准时间估算、波形质量分、九宫格 key、
同码波形替换策略、永久列表撕裂恢复：

```bash
./tests/host/run.sh             # macOS / Linux
.\tests\host\run.ps1            # Windows
```

需要一个本机 C++17 编译器（g++ / clang++ / MSVC）。脚本也认 `tools/zig/zig.exe`
和 PATH 上的 `zig`。当前为 **43 项全过**；PlatformIO 的 Xtensa 交叉编译器不能跑这些测试。

## 合规

仅采集自己拥有或获授权的设备；不包含滚动码破解或双向认证绕过。
