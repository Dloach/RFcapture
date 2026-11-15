# IR TOOLS：红外收发

> **状态（2026-11-16）：已从出货固件移除，源码与文档全部保留。**
>
> 发射链在真机上验证过（手机摄像头能看到 LED 亮），但格力空调本体始终没被触发，
> 在没有 38 kHz 接收模块的情况下无法确认机型变体，所以先把功能移出固件，不占空间。
>
> - 编回来：`pio run -e cardputer-ir -t upload`（launcher 会重新出现 IR TOOLS）
> - 开关在 `src/ir_config.h` 的 `ENABLE_IR_TOOLS` + `platformio.ini` 的 `build_src_filter`
> - host 测试不受影响，`.\tests\host\run.ps1` 照跑 251 项红外用例
>
> 出货固件：Flash 71.4% / RAM 54.2%；带 IR 的 `cardputer-ir`：Flash 72.3% / RAM 58.5%。

Cardputer ADV 的 **G44** 是板载红外发射 LED（串 22 Ω，从原理图 `IR1` + `R14`
确认），**没有载波产生硬件，也没有红外接收头**。所以：

- 发送：开箱即用，38 kHz 载波由 ESP32-S3 的 RMT 外设硬件调制；
- 接收/学习：必须在 Grove 口外接 38 kHz 解调模块，没有第二种办法。

## 接线（学习功能）

```text
模块 VCC  -> Grove 5V (红)
模块 GND  -> Grove GND (黑)
模块 OUT  -> Grove G1 (黄)     <-- 默认
```

G1/G2 在本固件里没有别的用途（Grove 上没接射频）。模块丝印是反的也没关系：
`SETUP -> RX PIN` 可以在 G1/G2 之间切换。可选模块：VS1838B、HX1838、
TSOP38238，或 M5Stack Unit-IR（U002）。

## 功能

| 页面 | 作用 |
| --- | --- |
| MY CODES | NVS 里的解析码库（最多 48 条），可直接发、删、导出 |
| AIR CON | 格力空调遥控：电源 / 模式 / 温度 / 风速 / 摆风 |
| SD REMOTES | 浏览 TF 卡上的 Flipper `.ir` 文件，逐键回放 |
| LEARN | 用外接模块抓一帧，解码后保存 |
| UNIVERSAL | 内置 LG / 三星 / 索尼 / 飞利浦 常见电视键 |
| SETUP | 接收脚 G1/G2、重发次数、LED 测试、清空码库 |

## 空调（格力）

空调不是"一堆按键"，而是一帧整机状态：温度就在帧里，所以每次按键都发
**完整状态**，不是"温度+1"这种相对指令。格力用 64 位自有协议（不是 NEC）。

`AIR CON` 页五行，`,` `/` 改值，`ENT` 发：

```text
POWER   ON / OFF
MODE    COOL / HEAT / DRY / FAN / AUTO
TEMP    16 - 30 C
FAN     AUTO / LOW / MID / HIGH
SWING   ON / OFF
```

- 改值不发红外，`ENT` 才发一帧 —— 连按六下温度也只是最后一帧上空气；
- 状态写 NVS（`ir/ac`），重启后还是你上次设的那台；
- `S` 把当前状态存进 MY CODES，之后在码库里一键重发；
- 一帧就走，不重复（`gree_send` 只发 1 份），和官方库 `kGreeDefaultRepeat`
  一致。

### 为什么这套帧是真机对得上的

- 帧结构、校验和、`0x50`/`0x20` 两个固定半字节，全部照 IRremoteESP8266 的
  `IRGreeAC` 实现，并用它单元测试里那份**真机抓包**（issue #386）逐字节校验：
  解出来必须是 `19 0A 60 50 02 23 00 F0`；
- Flipper-IRDB `ACs/GREE/` 下的 4 个文件、共 21 帧抓包，校验和 21/21 全过；
- 上机：`env:ir-selftest` 固件每轮都发一帧格力 `COOL 26C`，
  日志 `gree_send=1 gree=140`（140 个跳变 = 2 引导 + 32 位 + 3 位标记 +
  1 段间隔 + 32 位 + 1 段间隔）。

### 两套机型（MODEL 行）

真机抓包分成两族，只有两个字节不同：

| 字节 | YAW1F | YAPOF |
| --- | --- | --- |
| `byte2` | 开机时 ModelA=1 | 无 ModelA |
| `byte5` | `0x20` | `0x40`（WiFi 位） |

`AIR CON → MODEL` 可切 `BOTH / YAPOF / YAW1F`，默认 **BOTH**：按一次 `ENT`
两族各发一帧，空调只认它懂的那一帧。

### 实测状态（2026-11-16）

- **发射链已闭环**：手机摄像头对顶部，`SETUP → LED TEST`（闪 8 次）能看到淡紫色光。
  人眼看不见 940 nm，所以"肉眼没亮"从来不是故障证据。
- **空调本体尚未被触发**：`AIR CON → ENT` 在 BOTH / YAPOF / YAW1F 下都试过，无反应。
  帧格式与校验和都对，所以剩下的是机型变体问题。
- 确定解：接一个 38 kHz 接收模块，用 `LEARN` 抓一次原遥控器的「开机」帧，
  把那 8 个字节和本固件生成的逐字节对比。

### 局限

- 只做了格力。美的、海尔、大金协议各不相同，不能共用这套状态机；
- 没有定时、睡眠、强力、自清洁这些位，虽然协议里有字段；
- 摄氏度 16–30，不支持华氏（`UseFahrenheit` 位没碰）。

## 按键

- `;` `.` 上下，`ENT` 发送/进入，`` ` `` 或 `BKSP` 返回；
- AIR CON：`;` `.` 选行、`,` `/` 改值、`ENT` 发送、`S` 存码库；
- MY CODES：`ENT` 发送、`D` 连按两次删除、`S` 导出到 `/ir/MYCODES.ir`；
- SD REMOTES：`ENT` 进目录/打开文件，再 `ENT` 发送，`S` 存进 MY CODES；
- LEARN：抓到一帧后 `ENT` 回放、`S` 保存、`D` 丢弃重听；
- UNIVERSAL：`ENT` 发送，`S` 存进 MY CODES；
- SETUP：`ENT` 切换当前项；`LED TEST` 会连续发 1 秒载波。

## 支持的协议

发送（Flipper `.ir` 的 `parsed` 协议名，大小写不敏感）：
`NEC`、`NECext`、`Samsung32`、`RC5`、`RC5X`、`RC6`、`SIRC`、`SIRC15`、
`SIRC20`、`Kaseikyo`、`Gree`（空调），以及任意 `raw` 波形。

接收解码：同上（`raw` 永远可用，解不出来就存成 raw）。

`RCA`、`NEC42`、`NEC42ext` 目前只认名字、不能产生波形，遇到会提示
`UNSUPPORTED PROTOCOL`。

## 实现

- **协议引擎**（`src/ir_protocol.*`）：纯 C++，不依赖 Arduino，`tests/host/test_ir.cpp`
  在 PC 上跑 193 项编解码断言。时间参数与位序取自 Flipper 固件的
  `lib/infrared/encoder_decoder`，与 Flipper-IRDB 里两万个遥控文件同源。
- **发送**（`src/ir_tx.*`）：legacy `driver/rmt.h`（固件钉在 IDF 4.4），
  RMT 通道 0，1 tick = 1 µs，`rmt_set_tx_carrier()` 用 80 MHz 源时钟算
  38 kHz 的高低电平；mark 占空 33%，帧间静默 NEC ~107 ms、索尼 ~45 ms。
- **接收**（`src/ir_rx.*`）：RMT 通道 4，1 µs 分辨率，20 ms 空闲分帧；
  模块输出低有效，低电平段就是 mark。
- **码库**（`src/ir_store.*`）：NVS 一个 blob，带 magic/版本/FNV 校验，
  只存解析码；raw 帧太大，走 TF 卡。
- **文件**（`src/ir_file.*`）：Flipper `.ir` 解析/生成，支持
  `Filetype/Version/#/name/type/protocol/address/command/frequency/duty_cycle/data`，
  地址与命令按小端 4 字节解释（`EE 87 00 00` → `0x87EE`）。

## 限制

- **不能学习，除非接了接收模块**。这是硬件事实，不是配置问题。
- 空调（AC）遥控只有 `raw` 文件能发：`ACs/` 目录下的 `.ir` 都是整帧波形，
  本固件按帧原样回放，不做状态机。
- 帧太长的文件（> 48 kB）不打开；一次最多 512 个跳变。
- 一次按键发 1–5 份，默认 3 份；按住不放不会连发。
- `RC6` 是照 Flipper 编码器实现的实验性支持，没在真实飞利浦设备上验证过。

## 调试

- 裸眼看不见红外。`SETUP -> LED TEST` 时用手机摄像头对着顶部：会看到一个
  紫色亮点，说明 LED 和驱动正常。
- 串口（115200，USB CDC）会打印 `[ir] app ready …`、每次发送的
  `[ir] send …`、以及学习到的跳变数。
- **USB 日志只有在主机打开端口并拉高 DTR 之后才发得出去**：`usbLogf` 走的是
  TinyUSB CDC，TinyUSB 用 DTR 判断"主机在听"，而 .NET/PowerShell 打开串口时
  DTR 默认是关的。抓日志要显式 `DtrEnable = $true`，否则一个字都收不到
  （本功能第一次上机就被这个坑住过）。
- 更新固件后芯片可能停在 ROM 下载模式，应用不会启动；这时
  `esptool --before no_reset --after hard_reset chip_id` 打一次硬复位即可。
- 没有接收模块时学习页会直接说 `NO RECEIVER`，不会假装在听。

### 不带按键的 bring-up

`env:ir-selftest`（`pio run -e ir-selftest -t upload`）把固件编译成
`-DIR_BOOT_SELFTEST=1`：每次复位直接进 IR TOOLS，并在启动后 1.2 s 起每 1.5 s
发一串载波 + 一帧 NEC（LG 电源码），共 6 轮，每轮把结果写进 USB 日志：

```text
[ir] selftest 6/6 tx=1 burst=1 nec_send=1 timings=67
```

`tx=1` 是 RMT 通道建好，`burst=1`/`nec_send=1` 是 `rmt_write_items` 接受了两段
数据。真机验证就是这么做的，不需要按任何键。出货固件不含这个开关。


## 合规

只对自己拥有或获授权的设备发射；本功能不含任何滚动码或认证绕过。
