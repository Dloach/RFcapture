# Cardputer ADV + CAP.CC1101 · 315/433/868 MHz 遥控信号采集与回放

在 M5Stack Cardputer ADV 上，用 CAP.CC1101 模块采集、永久保存并重发铁门 / 卷帘门 /
道闸等 OOK 遥控器的信号。

> **2026-11-12 更新（先读）**：主线已是 **launcher 三功能（RF TOOLS / NFC TOOLS / BACKUP）
> + 九宫格快捷区**。RF TOOLS 内部的首页只有 **CAPTURE / SEND / SCAN RF** 三项；
> TF 备份/恢复已升为顶层 **BACKUP**，并同时备份 NFC 卡库和九宫格绑定。
> 本文 3.x 里仍把 BACKUP 当作 RF 首页项的描述属于旧版；以根目录 `README.md` 和
> `docs/DEV-STATE.md` §0 为准。

- **采集**：CC1101 异步 OOK 模式，GDO0 输出解调信号 → GPIO 中断 + `micros()` 时间戳记录脉宽
- **解码**：EV1527（24 bit）/ PT2262（12 位三态）
- **永久保存**：采集页选中后按 `Enter` 弹窗确认，存进 NVS（掉电不丢，可改名）
- **回放**：发送页选中后按 `S` 即按原始波形重发（软件生成基带，无需额外发射模块）
- **备份**：永久信号可一键写入 TF 卡的文本文件，换机 / 刷机后一键恢复
- **频段**：315 / 433 / 868 MHz 可切换，发送时自动切到该记录所属频段
- **界面**：横向 240×135；顶层 launcher 三功能 + 右侧九宫格，RF TOOLS 首页三项：
  采集 / 发送 / SCAN RF；支持键盘改名

> 已在实物上验证：EV1527 遥控器（315 MHz，T≈500 µs）采集、保存、回放均可让铁门动作。
> **本文所有码值（`123456` / `654321` 等）都是占位示例，不是任何真实设备的码值。**

---

## 1. 硬件

### 1.1 CAP.CC1101 与 Cardputer ADV 的连接

模块插在 Cardputer ADV 的 EXT 2.54-14P 排针上（模块自带 2×7 母座）。

| 信号 | Cardputer ADV | 说明 |
| --- | --- | --- |
| CC1101_CS | G5 | 片选 |
| CC1101_G0 (GDO0) | G15 | 接收解调输出 / 发送基带输入 |
| CC1101_RF_SW0 | G13 | 天线开关控制（普通 GPIO） |
| POWER_EN | G3 | 模块 3.3 V DCDC 使能 |
| SPI_SCLK | G40 | 与 microSD 共用 SPI |
| SPI_MOSI | G14 | |
| SPI_MISO | G39 | |
| NFC_CS | G6 | 同板上的 ST25R3916，使用时拉高 |

射频开关真值表（来自 Cap CC1101 原理图 V0.3）：

| RF_SW0 | RF_SW1 | 频段 |
| --- | --- | --- |
| 0 | 1 | 315 MHz |
| 1 | 0 | 433 MHz |
| 1 | 1 | 868 MHz |

RF_SW0 由 G13 直接驱动；RF_SW1 接在 CC1101 的 GDO2 上，通过写 `IOCFG2` 寄存器控制。

### 1.2 注意事项

- 模块 3.3 V 由 G3 控制的 DCDC 产生，G3 不拉高时芯片会"半死不活"（能读寄存器、射频不工作）。
- CC1101 的 SPI 用 250 kHz；模块走线较长，1 MHz 时读 ID 会失败。
- 读状态寄存器（PARTNUM / VERSION / MARCSTATE / RSSI）必须用 burst 读头 `0xC0 | addr`，
  普通读头 `0x80` 读不到。
- **GDO 静态电平编码**：本模块实测 `0x2F` = 拉低、`0x6F`（`0x2F|INV`）= 拉高，而手册 /
  RadioLib 用的 `0x2E` / `0x6E` 在这里是**高阻**。RF_SW1 悬空会导致完全收不到信号。
- 天线接在模块的 SMA 座上；没有天线几乎收不到信号。

---

## 2. 编译与烧录

### 2.1 Windows

```powershell
# 项目目录（NAS 映射盘符）：Y:\2026\cardputer
pio run -t upload --upload-port COM10
```

⚠️ 这块板子上传结束后会停在 ROM 下载模式，需要复位一次才会运行程序。
`tools/run-device.ps1` 已封装「编译 → 上传 → 复位 → 读串口」：

```powershell
.\tools\run-device.ps1              # 编译 + 烧录 + 看 6 秒日志
.\tools\run-device.ps1 -Seconds 10
.\tools\run-device.ps1 -NoBuild
```

⚠️ **如果串口没输出、esptool 连不上，九成不是固件挂了，而是板子还停在上一次的下载模式。**
手动进/出下载模式：**按住 `G0` 键再插 USB**。

### 2.2 macOS / Linux

```bash
# 项目目录（NAS 卷）：/Volumes/Project/2026/cardputer
pio run -t upload --upload-port /dev/cu.usbmodem101
```

同样的「编译 → 上传 → 复位 → 读串口」封装在 `tools/run-device.sh`：

```bash
./tools/run-device.sh               # 编译 + 烧录 + 看 6 秒日志
./tools/run-device.sh -s 10         # 看 10 秒
./tools/run-device.sh -n            # 只烧录 + 复位，不编译
./tools/run-device.sh -m            # 不编译也不烧录，只复位 + 看日志
./tools/run-device.sh -e example    # 烧官方 RadioLib 示例
./tools/run-device.sh -p /dev/cu.usbmodem1101
```

不写 `-p` 时会自动挑选第一个 `/dev/cu.usbmodem*` / `wchusbserial*` / `usbserial*`
等串口；串口在烧录后会重新枚举，脚本会等它回来再打开。串口波特率 115200。

启动日志：

```text
[cc1101] partnum=0x00 version=0x14 ok
[store] 1 capture(s) restored (magic=0x4331 count=1 bytes=340)
[saved] 1 permanent signal(s) restored
[app] band=315MHz captured=1 saved=1 rx=1 cc1101=1
```

`RF_DIAG=1`（默认）时还会在开机把 NVS 里所有记录的完整脉宽 dump 出来
（`[saved] #0 '...' band=433 code=234567 ... n=160: 350 1050 ...`），
`docs/signals-*.log` 就是这么来的。

> ⚠️ **那份 dump 里有你自己遥控器的真实码值和完整波形，等于一把能开门的钥匙。**
> 所以 `docs/signals-*.log` 已经在 `.gitignore` 里，不会进仓库——只留在采集它的那台机器上。
> 要重新生成一份：`RF_DIAG=1` 编译烧录，开机后把串口输出存成 `docs/signals-<日期>.log`。

抓到信号时：

```text
[cap] 315 EV1527 123456 bits=24 T=501 hits=1 pulses=160
```

要一份**没有串口诊断、也没有每秒强制重绘**的固件（排查键盘卡死用，见 `docs/DEV-STATE.md` §2 / §4）：

```bash
pio run -e diag-off -t upload
```

官方示例（RadioLib）也在同一工程里，用于排查射频硬件：

```powershell
pio run -e example -t upload --upload-port COM10
```

macOS / Linux 上直接：

```bash
./tools/run-device.sh -e example
```

---

## 3. 使用

> **按键布局（无需 Fn，单手可操作）**，`; , . /` 就是键帽上橙色箭头那四个：
>
> | 键 | 作用 |
> | --- | --- |
> | `;`（↑）/ `.`（↓），或 `k` / `j` | 上下选择 |
> | `,`（←）/ `/`（→） | **左右选择**，用来在弹窗的 YES / NO 之间移动 |
> | `` ` ``（左上角标 **esc** 那个键）或 `Backspace` | 返回 |
> | `Enter` | 进入 / 确认 |
>
> 左右键**不是**返回/确认：YES/NO 是横向排列的，用右箭头去选 `NO` 结果触发了确认，
> 删除时特别容易出事，所以 2026-11-08 改成了现在这样。
> 改名输入框里 `; , . /` 和 `` ` `` 都不当字符收，`Backspace` 删字符、`Enter` 确认。

开机进入 **launcher**（RF TOOLS / NFC TOOLS / BACKUP，↑/↓ 选择，`Enter` 进入）；
进 RF TOOLS 后是三项：

```text
CAPTURE
SEND
SCAN RF
```

右侧九宫格 `8 9 0 / I O P / J K L` 是快捷动作，在 launcher 上直接按。

CC1101 没接或没识别到时，标题栏会显示 `NO RADIO`（橙色）而不是页面标题，
此时 `CAPTURE` / `SEND` / `SCAN RF` 三项按 `Enter` 只会提示 `NO RADIO`，
TF 备份 / 恢复不受影响（只碰 NVS 和 SD）。

### 3.1 采集页

进入后自动监听；对准遥控器按一下，列表出现新条目（蜂鸣 + 底部提示）。

```text
CAPTURE  n=3        315MHz ●
01 EV1527 123456 q97 x3    501us
02 EV1527 654321 q88 x1    498us
03 RAW    160p     q39 x1  271us
ENT save  S send  F band  esc back
```

`q` 是**波形质量分 0–99**，和能不能解出码值无关——解出码值只说明形状像，
不代表回放得动。分数由三项算出（详见 `docs/DEV-STATE.md` §8.5）：

- 落在 1T / 3T 窗口的脉冲占比（60%）
- 最长低电平离理想 31T 有多近（40%）
- **一票否决**：帧内的长同步脉冲横跨两种下标奇偶 → 封顶 39。
  这种情况说明采集时电平标签被整体反转过，回放发出去的是补码，闸不会动。

所以：**同一个码值的多条记录，选 `q` 高的那条发。** `q40` 以下基本可以认为发不动，删掉重采。

| 按键 | 作用 |
| --- | --- |
| `;` / `.`（↑/↓） | 上下选择 |
| `Enter` | **永久保存**光标所选信号（弹窗选 YES / NO） |
| `S` | 直接发送这条（临时信号也能发） |
| `F` | 切换频段 315 → 433 → 868 |
| `D` | 删除选中条目（弹确认框，默认 NO，用 `,` / `/` 选） |
| `C` | 连按两次清空当前频段的采集列表 |
| `` ` ``（esc）/ `Backspace` | 回首页 |

保存弹窗：

```text
┌──────────────────────────┐
│ Save this signal?        │
│ [YES]         NO         │
│ < > choose   ENTER ok    │
└──────────────────────────┘
```

删除弹窗（采集页 `D` / 发送页 `D`）**默认停在 NO**，用 `,` / `/` 移到 YES 再按 `Enter`，
按 `` ` `` 直接取消。

### 3.2 发送页

只显示**永久保存**过的信号，按 `S` 即发。

```text
SEND  n=2           315MHz ●
01 Gate1        315 123456 q97
02 Gate2        315 654321 q88
S send  N rename  del del  esc back
```

| 按键 | 作用 |
| --- | --- |
| `;` / `.`（↑/↓） | 上下选择 |
| `S` / `Enter` | 发送选中信号 |
| `N` | **改名**：弹出输入框，直接敲键盘输入；`ENTER` 确认、`Backspace` 删字符 |
| `D` | 删除该条永久记录（弹确认框，默认 NO） |
| `` ` ``（esc）/ `Backspace` | 回首页 |

右边那个 `q` 是波形质量分，含义同 §3.1。
> 已知小瑕疵：`RAW`（解不出码值）的记录在发送页也按码值格式显示，会变成 `315 000000 qNN`——
> `SavedSignal` 里没有 `valid` 字段。回放不受影响，只是这一行看不出来是 RAW。

发送的是**采集到的原始波形**，不是用码值重建的帧。发送前先把它**裁尾到最后一个同步脉冲**
（`wholeFrameRange()`，头部故意不动：老记录开头那几个脉冲接收端能容忍，裁了反而会改变目前能用的信号），
这样每次重复都是整数帧、多次重复能连成一条不间断的流；
然后**零间隙背靠背**重复到 `RF_TX_TARGET_MS` = 700 ms——接近按住遥控器一下的长度。

这一步很关键：接收端只认「连续多帧完全相同」的信号，中间插入任何死区都会让它重新数帧，
结果就是一帧都发不动。重复次数由**实测的整帧长度**反推（`planRepeats()`），
所以不管记录里是一帧还是多帧，在空中的时长都一样；`RF_TX_REPEATS`(3) 是下限，
`RF_TX_MAX_REPEATS`(40) 是上限（防止一条极短的噪声记录把发送卡住）。
详细分析见 `docs/DEBUG-2026-11-08.md`。

改名弹窗：

```text
┌───────────────────┐
│ Rename signal     │
│ Gate1_            │
│ ENTER ok  DEL del  │
└───────────────────┘
```

### 3.3 SCAN RF：先找遥控器在哪个频点

不知道手上的遥控器是 315 还是 433（433 还分 433.07 / 433.42 / 433.92 / 434.42）时用这个。
首页选 `SCAN RF` 按 `Enter`，设备会轮流调到 7 个频点各测一次 RSSI，一直循环扫：

```text
SCAN RF  sweep 3
315.00  ▓▓▓░░░░░░░  -88
433.07  ▓░░░░░░░░░ -100
433.92  ▓▓▓▓▓▓▓░░░  -71
...
best 433.92 -71dBm  ENT use
```

- 条形按 −105…−40 dBm 映射；≥ `RF_SCAN_ACTIVE_DBM`(−72 dBm) 的频点标绿色，表示"这里有发射机"。
- **扫的时候按住遥控器不放**。一轮扫完约 0.12 秒（每个频点 15 ms），标题栏的 `sweep N` 是已经扫了几轮，
  多扫几轮读数才稳——每个频点只在切过去 1 ms 后就读 RSSI，单轮结果抖动很大。
- 按 `Enter` 采用当前最佳频点：切到它所属的频段（315 / 433 / 868 三选一，不是精确频率），
  然后**直接跳进采集页**，可以马上开始采。
- `` ` ``（esc）/ `Backspace` 退出，频段恢复成进来之前那个。

> 扫描期间接收中断被摘掉（`rx_.suspend()`，否则每次跳频都会往环形缓冲里灌一堆噪声），
> 所以这时候采集页不会收东西；退出扫描会自动恢复。

### 3.4 TF 卡备份 / 恢复（顶层 BACKUP）

launcher 进 **BACKUP**，选 `BACKUP TO TF` 按 `Enter`：把当前最多 8 条永久信号、
NFC 卡库、九宫格绑定写到 TF 卡。`RESTORE FROM TF` 先显示确认页，
再按一次 `Enter` 才真正恢复（防止误触覆盖）。

- 备份成功：状态行 `RF n ok|NFC n ok|Q ok`；没有记录时提示 `NOTHING TO BACKUP`
- 恢复成功：状态行同上；个别记录损坏时 RF 会提示 `RESTORED n, SKIPPED`
- 失败提示：`NO TF CARD` / `NO BACKUP FILE` / `BAD BACKUP FILE` / `BACKUP EMPTY`

文件路径与格式（UTF-8 纯文本，可以直接在电脑上查看 / 编辑名字）：

```text
/cardputer/signals.txt    RF 永久信号
/nfc/cards.bak            NFC 卡库
/quick.txt                九宫格绑定
```

`/cardputer/signals.txt` 的内容格式：

```text
CardputerSig 1
S 315 24 1 123456 501 160 Gate1
P 350 1050 350 1050 350 350 ...（共 160 个脉宽，单位 µs）
S 433 24 1 654321 498 160 Gate2
P 350 1050 ...
```

`S` 行字段：频段(MHz) 位数 协议序号 码值(十六进制) 基准脉宽(µs) 脉宽个数 名字。
`P` 行紧跟其后，是发送时实际重放的脉宽序列。恢复时整个永久列表会被文件内容**替换**
（不是追加），所以备份前可以先改好文件再恢复。

> TF 卡与 CC1101 共用 SPI（G40/G14/G39，卡座 CS = G12）。固件只在备份 / 恢复的瞬间
> 挂载卡，操作完立刻 `SD.end()` 卸载，平时射频接收不受影响。卡必须是 FAT32。

### 3.5 存储说明

| 数据 | 位置 | 说明 |
| --- | --- | --- |
| 采集列表（自动） | NVS `rfcap` | 最多 16 条，掉电不丢；`C` 两次可清空 |
| 永久保存（手动） | NVS `rfsave` | 最多 8 条，带名字，只有按 `Enter` 保存才进入 |
| NFC 卡库 | SPIFFS `/nfc/retry.txt` | 最多 16 张；`CARD LIST` 里删除 / 改名 |
| 九宫格绑定 | SPIFFS `/quick.txt` | 6 字段：slot/kind/index/icon/color/key |
| TF 备份文件 | `/cardputer/signals.txt`、`/nfc/cards.bak`、`/quick.txt` | BACKUP 一次写三份 |
| 重刷固件 | — | 两份 NVS 数据都不受影响；只有 `erase_flash` 或改分区表才会清掉 |

发送时会自动切到该记录所属的频段。

### 3.6 NFC TOOLS

launcher 进 **NFC TOOLS**，两个入口：

- `READ CARDS`：`S` 开始/停止扫描，卡贴到模块背面，读到后自动入库（UID / ATQA / SAK / 类型）；
- `CARD LIST`：`;` / `.` 选卡，`ENTER` 重启进入模拟，`B` 绑定九宫格，
  `D` 删除，`N` 改名，`` ` `` 返回。

模拟必须先选 `cfg.emulation` 再 `Units.begin()`，所以每次进入模拟都是一次重启。
会话期间不能打印、不能整屏重绘、尽量不碰 I2C；配方和踩坑见
`docs/DEV-STATE.md` §14.2 / §7.1。NFC 卡库在 SPIFFS `/nfc/retry.txt`，
BACKUP 会写 `/nfc/cards.bak`。

---

## 4. 解码原理

EV1527 / PT2262 都是脉宽编码，基准时间 T 一般 250–500 µs：

| 元素 | 高电平 | 低电平 |
| --- | --- | --- |
| 同步头 | 1T | 31T |
| 数据 `0` | 1T | 3T |
| 数据 `1` | 3T | 1T |
| PT2262 悬空位 | 1T | 1T |

EV1527 一帧 24 位（20 位地址 + 4 位按键），连续发 8–15 遍；固件把重复帧合并成一条记录并累加命中次数。

解码流程见 `src/ook_decoder.cpp`：找同步头 → 反推 T → 按"高+低"成对解析，**最多只解析 24 对**
（再往下就会把后面的帧折进码值，同一按键每次采到的码都会变）；末尾与帧间空隙粘连的那一位只按
高电平宽度判断。解析不足 22 对的不认，避免噪声波形被误判成 EV1527。

滚动码（HCS301 等）无法回放：每次按键码值都不同，抓到的码已经作废。

---

## 5. 代码结构

```text
src/
├── main.cpp          入口，分发 RF / NFC / BACKUP / launcher
├── launcher.*        launcher 三功能 + 九宫格
├── app.*             RF 首页 / 采集页 / 发送页 / SCAN RF
├── nfc_app.*         NFC 读卡 / 列表 / 模拟
├── nfc_cards.*       NFC 卡库（SPIFFS + TF 备份）
├── backup_app.*      BACKUP / RESTORE
├── bind_picker.*     九宫格绑定选择器
├── quick_bind.*      九宫格绑定表（/quick.txt）
├── cc1101.*          CC1101 驱动（SPI、频段、收发模式、PATABLE、RSSI）
├── ook_rx.*          GPIO 中断采集脉冲 + 成帧
├── ook_decoder.*     EV1527 / PT2262 解码 + 波形质量分（estimateBaseUs / scoreWaveform）
├── ook_tx.*          基带发送：RMT 硬件时序，位翻转作为 fallback
├── capture_store.*   采集列表（NVS rfcap，自动保存）
├── saved_store.*     永久保存列表（NVS rfsave，带名字）
├── tf_backup.*       RF 信号的 TF 备份 / 恢复（纯文本，共享 SPI）
├── sd_mount.h        共享 SD 挂载
├── polling_keyboard.h TCA8418 轮询键盘（替换库自带的中断读取）
├── trace.*           最近 8 帧存 RTC 内存，panic 重启后打印
├── rf_frame.h        脉冲/帧结构
├── rf_config.h       引脚、频段、时序参数、调试开关
└── example.cpp       官方 RadioLib 示例（排查硬件用）
```

参数调整都在 `src/rf_config.h`：

| 宏 | 默认 | 含义 |
| --- | --- | --- |
| `RF_FRAME_GAP_US` | 18000 | 超过此低电平判定一帧结束（须大于 31T） |
| `RF_NOISE_US` | 150 | 噪声门限（低于此宽度的脉冲会被合并） |
| `RF_MAX_FRAME_PULSES` | 300 | 单次突发最多保留的脉冲数（一帧 50 脉冲） |
| `RF_STORED_PULSES` | 160 | 每条记录的脉冲数组容量；一个 EV1527 帧 50 脉冲，连按出来的多帧突发会截到这里 |
| `RF_MAX_CAPTURES` | 16 | 采集列表条数上限 |
| `RF_MAX_SAVED` | 8 | 永久保存条数上限 |
| `RF_STORE_FLUSH_MS` | 30000 | `hits` 计数批量写回 NVS 的最小间隔（按住遥控时每秒会 bump 几十次） |
| `RF_TX_REPEATS` | 3 | 重复次数的**下限** |
| `RF_TX_TARGET_MS` | 700 | 目标在空时长；实际重复次数由实测整帧长度反推 |
| `RF_TX_MAX_REPEATS` | 40 | 重复次数的**上限**（防止极短的噪声记录卡住发送） |
| `RF_TX_USE_RMT` | 1 | 1 = RMT 硬件时序，0 = `delayMicroseconds()` 位翻转（fallback / A-B 对照） |
| `RF_SCREEN_ROTATION` | 0 | 屏幕方向（0 = 240×135 横向） |
| `RF_KBD_INT` | 11 | TCA8418 的 `/INT`（Cardputer 的脚，不是 CC1101 的）；开机时必须 `detachInterrupt`，见 §5.1 |
| `RF_DIAG` | **1** | 串口诊断 + 1 Hz 强制重绘 + 屏幕"还活着"指示。`pio run -e diag-off` 整体编译掉 |
| `RF_SCAN_SELFTEST` / `RF_GDO_TEST` / `RF_RX_SELFTEST` | 0 | 三个开机自测，用 `-D` 打开，见 `docs/DEV-STATE.md` §8.4 |

### 5.1 调试过程中踩过的坑

| 现象 | 原因 | 解决 |
| --- | --- | --- |
| 完全读不到芯片（0xFF） | 读状态寄存器用了 `0x80` 读头 | 必须用 burst 读头 `0xC0` |
| 芯片识别正常但收不到任何信号 | 天线开关 RF_SW1 没被驱动 | 本模块 `0x2F`=拉低、`0x6F`=拉高；`0x2E`/`0x6E` 是高阻 |
| 有波形但解不出协议 | 自选的 "OOK 嗅探" 寄存器组让解调器不出数据 | 采用官方 RadioLib（SmartRF 推荐值）的寄存器组 |
| 脉宽被拉宽、解码不稳 | 解调速率 2.4 kbps 太慢 | 改成 9.6 kbps（`MDMCFG4=0x88, MDMCFG3=0x83`） |
| 能收不能发、闸不开 | 从未写 PATABLE，PA 输出极低 | 进 TX 前写 PATABLE `{0x00, 0xC0}`（+10 dBm） |
| 开机瞬间串口刷屏 | CC1101 未配置时 GDO0 默认输出时钟，被接收端当成无数个边沿 | 先配置芯片再启动接收。历史上持续刷的 `RX buffer too small` 是 RMT 驱动 ISR 里打的日志，`esp_log_level_set` 按 tag 压不住；接收改成 GPIO 中断后已不再出现 |
| 可收可发，但键盘用几秒 / 按几下就失灵（心跳日志还在打，主循环活着） | M5Cardputer ADV 的键盘读取靠 TCA8418 `/INT` 边沿：漏一次边沿或未用矩阵引脚触发 GPIO 中断后 INT 被拉低锁死，10 级按键 FIFO 填满，主循环仍在跑但再也收不到键 | 用 `src/polling_keyboard.h` 的 `PollingKeyboardReader` 替换库自带的中断读取：每轮主循环直接读 TCA8418 FIFO，并关掉 GPIO 中断（`CFG` 只留 `KE_IEN`）。替换前先 `detachInterrupt(RF_KBD_INT)`，否则销毁默认读取器会在 GPIO 11 上留下悬空的 ISR |
| 同一个按钮两次采集，码值一样，一次能开闸一次不能 | store 只存脉宽、电平靠下标奇偶推；`onEdge()` 原来盲翻 `level_`，漏一个边沿之后整段电平标签反过来，回放发出去的是补码 | `onEdge()` 改成 `readPin()` 直接读 GPIO 寄存器拿真实电平（`IRAM_ATTR`，NVS 提交关 flash cache 期间也安全）。这类坏记录现在会显示成低 `q` 分，见 §3.1 |
| 换机 / 重刷后永久信号没了 | 永久信号只存在 NVS `rfsave` 里 | launcher 进 `BACKUP` 生成 `/cardputer/signals.txt`，新机器上 `RESTORE FROM TF` 恢复 |
| 插 TF 卡后射频偶发丢帧 | TF 卡座与 CC1101 共用 SCK/MOSI/MISO，长时间挂载会抢 SPI | 只在备份 / 恢复瞬间 `SD.begin()`，结束后立刻 `SD.end()`；操作期间暂停接收 |
| 换了台电脑编译报 `rmt_set_pin was not declared` | `platform = espressif32` 没锁版本，新机器拉到 8.0.0（Arduino core 3.x / IDF 5.x），legacy RMT 已移除 `rmt_set_pin` | 改用 `rmt_set_gpio(..., false)`（IDF 4.4 也提供该 API） |
| 烧录后黑屏、串口无限重启：`rmt(legacy): CONFLICT! driver_ng is not allowed to be used with the legacy driver` + `abort()` | core 3.x / IDF 5.x 下 M5Unified 的 `LedBus_RMT`（板载 RGB LED）先占用新 RMT 驱动，与 `ook_rx.cpp` 的 legacy RMT 冲突，开机约 200ms 即 abort | `platformio.ini` 锁 `espressif32@6.13.0`（IDF 4.4 只有 legacy RMT；与官方推荐的 6.7.0 同线）。**不要升 8.x**。崩溃固件会让 esptool 抓不到下载窗口，需侧面开关 OFF→按住 G0→上电→松 G0 手动进下载模式救砖 |

---

## 6. 排障清单

1. 串口没有 `[cc1101] partnum=... ok`，或屏幕标题栏是橙色 `NO RADIO`：
   检查模块是否插紧、G3 是否被拉高、SPI 是否 250 kHz、读状态寄存器是否用了 `0xC0`。
   `NO RADIO` 状态下 `CAPTURE` / `SEND` / `SCAN RF` 按 `Enter` 只会提示 `NO RADIO`，TF 备份/恢复仍可用。
2. 芯片识别正常但 `captured` 不增加：GDO0 上没有脉冲。先确认天线已接、频段正确
   （按 `F` 切换，或先用 `SCAN RF` 找频点）。串口心跳里的 `isr` 不涨就是引脚根本没边沿，
   `isr` 涨而 `rp` 不涨是边沿都短于噪声门限——详见 `docs/DEV-STATE.md` §9.4。
3. 发送无效：
   - 先确认是固定码遥控器（EV1527 / PT2262），滚动码无法回放。
   - 看这条记录的 `q` 分：`q40` 以下基本发不动，删掉重采（原因见 §3.1）。
   - 同一个码值采了好几条的话，挑 `q` 最高的发。
4. 采到的码值和别人说的对不上、差一位：先按 `q` 分判断这条波形本身好不好；
   码值偏移不影响回放（回放发的是原始波形），只影响显示和去重。
5. 屏幕方向不对：改 `RF_SCREEN_ROTATION`（0 或 2 为横向，1 或 3 为竖向）。
6. 按键失灵但屏幕还在刷新：`docs/DEV-STATE.md` §3 那个未定案问题，判据表在 §2。

---

## 7. 合规提醒

- 只采集你拥有或已获授权测试的设备（自家铁门、自家车位道闸等）。
- 很多小区 / 物业设备属于公共设施，未经授权录制、复制、回放可能违反法律或物业规定。
- 本项目不实现滚动码破解，也无法绕过双向认证。
