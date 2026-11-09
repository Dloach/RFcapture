# Cardputer ADV + CAP.CC1101 · 315/433/868 MHz 遥控信号采集与回放

M5Stack Cardputer ADV 上，用 **CAP.CC1101** 模块采集、永久保存并重发铁门 / 卷帘门 /
道闸等 OOK 遥控器的信号（EV1527 / PT2262 固定码）。

- **采集**：CC1101 异步 OOK，GDO0 → GPIO 中断 + `micros()`，1 µs 分辨率记录脉宽
- **解码**：EV1527（24 bit）、PT2262（12 位三态）
- **永久保存**：采集页 `Enter` 弹窗确认，写入 NVS，掉电不丢，可改名
- **回放**：发送页选中后 `S` 发送；原始波形**零间隙背靠背**重发（接收端只认连续多帧）
- **备份**：永久信号可写入 TF 卡文本文件，换机 / 刷机后一键恢复
- **界面**：横向 240×135，首页 5 项：采集 / 发送 / SCAN RF / BACKUP TF / RESTORE TF

> 实测：315 MHz EV1527 遥控器（T≈500 µs）采集 → 保存 → 回放，铁门正常动作。
>
> **码值一样不代表波形一样好。** 采集页和发送页每行都带 `q00`–`q99` 波形质量分，
> 用来区分"解得出码值"和"回放得动"：`q40` 以下基本发不动，删掉重采。
> 分数怎么算、以及它抓到的那个真实故障（电平标签被整体反转、回放发的是补码），
> 见 [docs/DEV-STATE.md](docs/DEV-STATE.md) §8.3 / §8.5。

```text
   遥控器 ──315MHz──▶ CAP.CC1101 ──GDO0──▶ 中断采样 ──▶ 解码 ──▶ 采集列表
                                                                  │ Enter
                                                          永久保存（NVS）
                                                                  │
                                           发送页  S ──▶ 零间隙连发 ──▶ 天线
```

## 快速开始

Windows：

```powershell
pio run -t upload --upload-port COM10
.\tools\run-device.ps1          # 上传后自动复位并看日志（必须，否则停在下载模式）
```

macOS / Linux：

```bash
pio run -t upload --upload-port /dev/cu.usbmodem101
./tools/run-device.sh           # 自动找串口，编译 → 烧录 → 复位 → 看日志
```

按键（无需 Fn，`; , . /` 就是键帽上橙色箭头那四个）：`;`/`.` 或 `k`/`j` 上下选，
`,`/`/` 左右选（弹窗里在 YES / NO 之间移动），`` ` ``（左上角标 esc 那个键）或 `BKSP` 返回，`ENTER` 进入 / 确认。
采集页 `ENTER` 永久保存、`S` 直接发、`F` 换频段、`D` 删除、`C` 连按两次清空；
发送页 `S` 发送、`N` 改名、`D` 删除；首页第 3 项 `SCAN RF` 扫 7 个频点找遥控器。

详细接线、按键、解码原理、踩坑记录见 **[docs/README.md](docs/README.md)**。

## 目录结构

```text
cardputer/
├── platformio.ini              env: cardputer-adv（应用）/ diag-off（关诊断）/ example（官方示例）
├── boards/cardputer_adv.json   板卡定义（8 MB Flash，无 PSRAM）
├── src/
│   ├── app.*                   首页 / 采集页 / 发送页、保存确认、改名
│   ├── cc1101.*                CC1101 驱动（SPI、频段、收发、PATABLE）
│   ├── ook_rx.*                GPIO 中断采集脉冲 + 成帧
│   ├── ook_decoder.*           协议解析 + 波形质量分（`estimateBaseUs` / `scoreWaveform`）
│   ├── ook_tx.*                基带发送（原始波形零间隙回放）
│   ├── capture_store.*         采集列表（NVS）
│   ├── saved_store.*           永久保存列表（NVS，带名字）
│   ├── tf_backup.*             TF 卡备份 / 恢复
│   ├── polling_keyboard.h      TCA8418 轮询键盘（替换库自带中断读取）
│   ├── trace.*                 最近 8 帧存 RTC 内存，重启后打印
│   ├── rf_frame.h / rf_config.h
│   └── example.cpp             官方 RadioLib 示例（排查硬件用）
├── tests/host/                 解码器 PC 端单元测试（run.ps1 / run.sh）
├── tools/run-device.ps1        编译 + 烧录 + 复位 + 串口日志（Windows）
├── tools/run-device.sh         同上（macOS / Linux）
├── tools/cap-serial.py         长时间抓串口日志，断线自动重连
└── docs/
    ├── README.md               中文使用文档（接线、按键、解码原理）
    ├── DEV-STATE.md            当前状态、已锁定约束、待验证项
    └── DEBUG-2026-11-08.md     排查记录
```

## 测试

解码器有脱离硬件的单元测试，覆盖 EV1527 24 bit、PT2262 三态、抖动、缺同步头、
基准时间估算（`estimateBaseUs`）和波形质量分（`scoreWaveform`）：

```bash
./tests/host/run.sh             # macOS / Linux
.\tests\host\run.ps1            # Windows
```

需要一个本机 C++ 编译器（g++ / clang++ / MSVC）。PlatformIO 自带的 Xtensa
交叉编译器不能用来跑这些测试。

## 合规

仅采集自己拥有或获授权的设备；不包含滚动码破解或双向认证绕过。
