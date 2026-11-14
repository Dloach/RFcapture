# 开发状态交接（最后更新 2026-11-14）

换机器继续开发前记录。**代码在 NAS 上，不在任何 Qoder 会话目录里**：
macOS 是 `/Volumes/Project/2026/cardputer`，Windows 上映射成 `Y:\2026\cardputer`——同一个目录，别另存一份。
上一轮会话的串口日志留在原 Mac 的 `/tmp/cap.log`（不随项目走），需要时用 `tools/cap-serial.py` 重抓。

## 0. 当前真相（先读这一节）

| 主题 | 当前结论 |
|---|---|
| 功能结构 | launcher 六功能 **RF TOOLS / NFC TOOLS / KEYBOARD / BACKUP / SETTINGS / SSH**，左侧 4 行滚动 + 右侧 3×3 九宫格；每个功能一次重启 |
| RF 首页 | 只有 **CAPTURE / SEND / SCAN RF** 三项；BACKUP 已升为顶层功能 |
| KEYBOARD | USB mode **0**（TinyUSB CDC + HID）或 BLE HID `Cardputer-KB`；键位映射见 §0.1 |
| SETTINGS / WIFI | 异步扫描 2.4 GHz、密码连接、NVS 保存 5 个网络、启动自动连接；`TEST CONNECTION` 验证 `example.com:80` |
| SSH | LibSSH-ESP32 5.10.0（LGPL-2.1），最多 4 个 profile，密码认证、host key 指纹确认、40×13 终端；Ed25519 代码保留但默认关闭 |
| NFC × TF 共存 | **已解决**，见 §7.1：挂 SD 前对 ST25R3916 发 `CMD_SET_DEFAULT`，不要再打 G3 脉冲 |
| NFC 模拟配方 | **§14.2 是唯一权威**；§13.6 是首次成功记录，§13.5 是已被推翻的历史（当时判定“做不到”） |
| NFC 实验代际 | `nfc_spike/nfc_emu/nfc_example/nfc_store` 已在 **v1.1.0** 删除；历史见 tag `archive-nfc-spikes-2026-11-12` |
| 快捷绑定 | `quick.txt` 6 字段（slot/kind/index/icon/color/key）；按 UID/code 的稳定 key 解析，失效显式 `BINDING STALE` |
| 永久列表撕裂 | P0-2 已改为 blob→count 提交 + 按字节数自愈；`recoverSavedCount()` 有 host 测试 |
| 同码重采 | P0-3：新波形分数显著更高时自动替换，`BETTER WAVE`；再次 ENTER 可更新永久列表 |
| host 测试 | **52 项全过**（43 解码/绑定/存储 + 9 SSH 终端；g++ 15.2 / WSL 验证） |

### 0.1 KEYBOARD 速查

- `platformio.ini` 把 `ARDUINO_USB_MODE` 从 1 改成 **0**，全固件 TinyUSB CDC + HID；
  运行口会变，烧录走 1200bps touch，G0 + 上电是 ROM 兜底。
- KEYBOARD 是两层页面：模式选择页（USB / BLUETOOTH）→ 键盘工作页；工作页
  `Fn+BKSP` 软重启回模式选择页，模式选择页 `esc` 回 launcher。
- 工作页显示 4×14 映射并高亮按下键；`Opt` = Win/Command；
  `Fn+1..0`/`Fn+-`/`Fn+=` = F1..F12；`Fn+; . , /` = 方向键；`Fn+\`` = Esc。
- BLE 重连：连接时强制 CCCD 通知 + `ESP_BLE_SEC_ENCRYPT` + 每秒补发当前报文。

### 0.2 WiFi / SSH 速查

- WiFi 凭据在 NVS `wifi`（最多 5 个，`count/last/ssidN/passN`）；
  `settings_app` 扫描/密码/连接，`TEST CONNECTION` 连 `example.com:80`。
- launcher 启动会自动连 `last` 网络；从 SETTINGS 按 `esc` **不重启**回 launcher，
  WiFi 栈继续运行，header 右侧三格图标显示状态。
- SSH 依赖 `LibSSH-ESP32@5.10.0`（LGPL-2.1，`platformio.ini` 固定 zip tag）；
  `ssh_app` 使用 56 KB FreeRTOS task，不能塞进 Arduino loop 栈。
- SSH profile 在 NVS `ssh`（最多 4 个：name/host/port/user/auth/pass/keyPath）；
  `+ NEW PROFILE` 新建，`E` 编辑，`D` 连按两次删除，`ENT` 打开；
  known_hosts 在 SPIFFS `/spiffs/ssh/known_hosts`。
- Host key：首次连接显示 SHA256 指纹，`ENT` 信任/`esc` 拒绝；变更报错；
  错误页 `K` 清空 known_hosts，`R` 重试。
- Ed25519：默认固件**关闭**（`SSH_ENABLE_ED25519=0`），profile 只显示
  NAME/HOST/PORT/USER/PASS/CONNECT；代码保留在 `ssh_app.cpp`，把该宏设为 1
  可恢复 TF `/ssh/id_ed25519` 导入与 AUTH 切换。
- 编辑页支持 `,` / `/` 光标、光标处插入、`BKSP` 删除、闪烁 `_`。
- 终端 40×13：ANSI 16 色、UTF-8 框线回退、64 行历史（`Opt+; .`）；
  15 s 超时、30 s keepalive；SSH task 56 KB，串口每 30 s 打 heap/stack。
- 终端 40×13，`ssh_terminal` 做基本 VT100（光标/清屏/SGR 忽略/滚动）；
  `Fn+BKSP` 退出会话，`Fn+Q` 关闭。
- 已知限制：Ed25519 默认关闭、无加密私钥/RSA、无无限自动重连
  （错误页/列表 R 手动重试）、UTF-8 只做 ASCII 回退、ANSI 字符集/鼠标不完整；
  SSH/WiFi 密码均为 NVS 明文。

> 下面 §1–§14 是历史记录，按时间保留；与 §0 冲突时以 §0 和最新代码为准。
> `retry/` 是独立参考工程，源码仍在 git 中；不要在出货固件里引用它。

---

> 15:15 更新：另一轮开发把接收从 RMT 换成了 GPIO 中断、解码只取同步头后 24 位、加了 `[trace]` 噪声帧日志，
> 并把回放帧间空隙改成 12.5ms——这导致新采集的单帧记录发不动铁闸。已定案并修复：
> **回放必须零间隙背靠背**（原遥控就是连续发帧）。分析全文见 `docs/DEBUG-2026-11-08.md` §0/§7。
>
> 23:20 更新（本轮）：发送换成 **RMT 硬件时序**、加了 `SCAN RF` 扫频与实时 RSSI、
> 修掉接收链路 4 个致命 bug（其中一个是我自己引入的：`rmt_config()` 抢占 GDO0 导致采集全废）、
> 采集页保留 `RAW` 波形并加了质量门限、按键重新布局（左右键改为横向选择、`` ` `` 当 esc）。
> 详见 **§8** / **§9**；换机步骤见 **§10**。

> 16:08 更新：**已实测确认**——修复固件下重新采样得到单帧记录（n=50），从采集页直接 `S` 发送，
> 铁闸可反复正常操控（发送侧自动补到 6 个连续帧）。另外 `#3` 那条「下降」曾误命名 `rollUP`，
> 已改回 `rollDOWN`；`#1` 仍为 `rollUP`，两者码值不同（234567 / 23456F），别再混用。

> **2026-11-09 更新（本轮，全部是代码修复，没有新功能）**：`onEdge()` 改成读真实引脚电平（原来存的是**反的**电平，
> 这正是 §8.3「同码值但发不动」的根因，已用真实波形证实，见 §8.3）、`trace` 改回 `RTC_DATA_ATTR`、
> 采集/保存每条记录加 `q00`–`q99` 波形质量分（判据见 §8.5）、CC1101 没识别到时上屏降级、
> `SavedStore` 的 RAW 去重改成比波形、`hits` 计数改成批量落盘（原来根本没写进 NVS）、
> 诊断代码全部收进 `RF_DIAG` 开关 + `pio run -e diag-off`、清掉死代码、抽出 `App::transmit()`。
> **本轮改动全部只编译验证过，没有上机测过。**
> 详见 **§8.5** / **§8.6** / **§2**。

> **2026-11-09 更晚（入库 + 立项，没有改任何出货代码）**：仓库脱敏后推到 public（**§11**）；
> 决定把固件改造成多功能系统，架构定为「单体 + 运行时 app 注册表」，第一个功能选 NFC，
> 依赖验证 spike 当时编译通过（**§12**）。这些 spike 文件已在 v1.1.0 删除，见 §0。
> **§0 是当前真相；以下 §1–§14 是按时间保留的历史记录。**

---

## 1. 2026-11-09 固件快照（历史）

当时的 `pio run`：**Flash 635945 B / 19.0%，RAM 26264 B / 8.0%**（`diag-off` 环境：632961 B / 26256 B）。
`platform = espressif32@6.13.0` 已锁死（原因见 §5 硬约束）。下面是当时的单功能 RF 固件状态，
不是当前三功能固件；当前状态看 §0。

| 功能 | 状态 |
|---|---|
| 315 / 433 / 868 MHz OOK 采集 | ✅ 可用，但要注意波形质量（见 §8.3） |
| 采集页：选中 / 保存 / 直接发送 / 换频段 / 删除 / 清空 | ✅ |
| 采集页 `RAW` 行（解不出码但保留波形） | ✅ 需通过质量门限才入库（见 §9.1） |
| 每条记录的 `q00`–`q99` 波形质量分 | ✅ 新增，判据见 §8.5 |
| 永久保存（NVS，8 条） | ✅ 现有 4 条，**都是修 `onEdge()` 之前采的，建议重采**（见 §8.3） |
| 发送：**RMT 硬件时序 + 零间隙连发 + 700ms 定长** | ✅ rollUP/rollDOWN/maindoor 实测都能开 |
| CC1101 没接/没识别到时的降级 | ✅ 新增，标题栏显示 `NO RADIO`，RF 三项菜单拒绝进入（TF 备份/恢复仍可用） |
| 首页 5 项：CAPTURE / SEND / SCAN RF / BACKUP TF / RESTORE TF | 历史；当前是 launcher 三功能 + 九宫格，见 §0 |
| `SCAN RF` 扫频（7 频点 RSSI） | ✅ 实测 433 附近 -88、其他 -100 |
| 采集页实时 RSSI 条 + 峰值保持 | ✅ |
| TF 卡备份 / 恢复 | ✅ 之前已验证 |

实机确认过的发送信号：`maindoor`(315/123456)、`rollUP`(234567)、`rollSTOP`(23456D)、
`rollDOWN`(23456F)，都能开闸。**这些码值是占位符**，真值只在本机 NVS 里（脱敏原因见 §11）。

**NVS 数据不可再生，任何情况下不要 `erase_flash`。**

---

## 2. 诊断代码：已收进 `RF_DIAG` 开关

> 2026-11-09 更新：这一节原来叫「临时诊断代码清单（发布前要删）」，靠 `grep "Temporary diagnostic"` 找。
> 那些注释已经全部删掉，诊断改成由 `src/rf_config.h` 里的 `RF_DIAG` 统一控制，
> 默认 `1`（**行为和以前完全一样**，没有任何静默改动）。

这些都是 2026-11-08 为定位「按键卡死」加的，正常功能不受影响，但会刷串口、每秒强制重绘一次。

| 位置（函数名，不要用行号） | 内容 | 作用 |
|---|---|---|
| `App::begin()` | 任务看门狗 6 秒 + panic | 主循环卡住超过 6 秒自动重启并打印崩溃回溯 |
| `App::update()` | `esp_task_wdt_reset()` | 喂狗 |
| `App::update()` | `[slow] kbd/rx` | 键盘 I2C 或接收中断单次耗时 >100 ms 立即打印 |
| `App::handleAction()` | `[key]` / `[act]` | 每次动作带 screen/mode/sel |
| `App::update()` | 1 Hz `tick_` → `dirty_ = true` | **每秒强制重绘**，冻屏一眼可见 |
| `App::update()` | `[hb]` 心跳（含 `ev / evAge / loops`） | 区分「键盘不出事件」和「主循环死了」 |
| `App::renderHeader()` | 标题颜色每秒切换 accent/dim | 画面是否还活着 |
| `App::renderHeader()` | 右上角 8×8 方块每秒闪 | 同上 |
| `polling_keyboard.h` | `kbdEventCount() / kbdLastEventMs()`、`[ev] 0x..` | 统计 TCA8418 事件（`kbdEmptyPolls()` 只写不读，已删） |
| `App::pollReceiver()` | `[rej]` | 被质量门限挡掉的帧 |

**没有**收进 `RF_DIAG` 的：`trace::dumpAfterReset()`——它是 panic 之后唯一还能出来的输出，
`[app] band=... cc1101=...` 这行开机健康摘要，`cc1101.cpp` 的 `[cc1101] partnum=... version=...`
（排障清单第 1 条靠它），以及 `src/tf_backup.cpp` 的 `[tfs]` / `[tfm]`（只在备份/恢复时打印，不刷屏）。

关掉诊断（就是第 4 节第 2 步那个实验）：

```bash
pio run -e diag-off -t upload
```

`platformio.ini` 里已有常驻的 `[env:diag-off]`，只是 `-DRF_DIAG=0`，不用手改编译参数。
实测差异：**Flash 635945 → 632961 B（省 2984 B），RAM 26264 → 26256 B**。

**注意**：1 Hz 强制重绘同时也是目前「不卡死」的因素之一，关掉后要重新做长时间按键测试（见第 4 节）。
彻底删的话，`RF_DIAG` 各分支连同 `app.h` 里的 `lastTickMs_ / tick_ / loops_` 和 `polling_keyboard.h` 的两个计数一起去掉。

---

## 3. 未定案的「按键卡死」

时间线：

1. 库自带中断式键盘读取器 → 开机 5 秒 / 10 次按键内必卡。根因已确认（/INT 电平漏边沿、FIFO 填满），已用轮询读取器修复。
2. 加 TF 备份/恢复 + 4 项首页后，用户报告「3 次按键内卡死」，连续 3 次。
3. 加上面那批诊断（含 1 Hz 强制重绘）后，同一台机器、同一批操作（导航、备份、恢复、发送）连续 7 分钟无卡死，`loops` 稳定约 490 次/秒，无 `[slow]`、无 panic。

**当前主要假设（未证实）**：那次卡死是**画面停止刷新**（`dirty_` 渲染路径），主循环和键盘其实还活着——用户看到的是「冻屏」。1 Hz 强制重绘把它掩盖了。次假设是 TF 挂载/SD 超时阻塞主循环，但这次备份和恢复都跑通了，没有 `[tfs]` 卡顿记录。

**下一步判断依据**（诊断已就位，直接看日志）：
- 标题还在闪 + 按键无 `[ev]` → 键盘/芯片侧问题。
- 标题不闪 + 6 秒后设备重启 → 主循环阻塞，看 panic 回溯。
- 标题不闪 + 不重启 + 心跳继续 → 渲染/脏标记问题。

---

## 4. 换机器后的建议顺序

> 换机具体步骤见 **§10**，这里只保留当时的功能验证顺序。

1. **先跑长时间按键压力测试**：连续 50+ 次按键、来回切换页面、反复备份/恢复，观察是否复现。这是唯一能证伪「1 Hz 重绘治好了卡死」的办法。
2. 复现就按第 3 节的判据读日志；不复现则 `pio run -e diag-off -t upload`（诊断全部编译掉，含 1 Hz 重绘），再跑一轮同样的压力测试。
3. 补测：发送页改名 / 删除、采集页 `S` 直接发送、`C` 连按两次清空。
4. 最后同步 `docs/README.md`（用户文档已按当前功能更新过一轮，按键提示里已去掉方向说明）。

---

## 5. 环境与常用命令

```bash
# macOS / Linux（NAS 卷）
cd /Volumes/Project/2026/cardputer

pio run                                  # 编译
./tools/run-device.sh                    # 编译 + 烧录 + 复位 + 读 6 秒日志
./tools/run-device.sh -n -s 10           # 只烧录
./tools/run-device.sh -m -s 10           # 只复位 + 读日志（不改固件）

# 长时间抓日志（断线自动重连，过滤历史 rmt 噪声行）
python tools/cap-serial.py 600 /tmp/cap.log --port /dev/cu.usbmodem101
```

```powershell
# Windows：同一个 NAS 目录映射成 Y:\2026\cardputer
pio run -t upload --upload-port COM10
.\tools\run-device.ps1
```

Windows 的 `cap-serial.py` 需要 pyserial，没有系统包时用
`%USERPROFILE%\.platformio\penv\Scripts\python.exe` 跑。

⚠️ **这台 Windows 机器上没有任何本机 C++ 编译器（g++ / clang++ / MSVC），`tests/host` 跑不起来。**
PlatformIO 自带的 Xtensa 交叉编译器不能用来跑单元测试。要验证解码器/质量分的算术，只能：

```bash
# 语法检查（能查出编译错误，不能跑）
"$HOME/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-g++" \
    -std=c++11 -fsyntax-only -Itests/host -Isrc tests/host/test_decoder.cpp
```

或者把整数运算原样搬到一个临时 JS 文件里用 `node` 跑（可以喂 `docs/signals-2026-11-08.log` 的真实波形）。
§8.5 的判据就是这么验的。

硬约束：

- `platform = espressif32@6.13.0` 锁死，升 8.x 会黑屏无限重启。
- `src/` 按 **C++11** 编译，没有 `std::make_unique`。
- 禁止 `erase_flash` / 擦 NVS / 改分区表。
- 烧录后板子会停在 ROM downloader，必须让脚本做 watchdog reset 才会跑新固件。

---

## 6. 代码地图（本轮新增/改动）

| 文件 | 说明 |
|---|---|
| `src/polling_keyboard.h` | 轮询式 TCA8418 读取器，替换库自带中断版；含诊断计数（`RF_DIAG` 门控） |
| `src/tf_backup.h` / `.cpp` | TF 备份恢复。路径 `/cardputer/signals.txt`，文本格式 `S <MHz> <bits> <proto> <code> <baseUs> <count> <name>` + 下一行 `P p1 p2 ...`；按需挂载、写完立刻 `SD.end()` |
| `src/saved_store.*` | 新增 `replaceAll()`，恢复时整体替换并写回 NVS；`RAW` 条目改成按波形去重（见 §8.6） |
| `src/capture_store.*` | `similarTrain()` 提到头文件供两个 store 共用；新增 `quality()` / `flush()`（见 §8.5 / §8.6） |
| `src/ook_decoder.*` | 新增 `estimateBaseUs()` / `scoreWaveform()`，删掉没人调的 `Decoded::address()/button()` |
| `src/ook_tx.*` | 删掉没人调的 `sendCode()`（回放一律走原始波形） |
| `src/cc1101.*` | 删掉死代码 `idle()` / `paTable()` / `configured()`；`ready()` 现在用于上屏降级 |
| `src/app.*` | 5 项首页菜单、`backupSignals()` / `restoreSignals()`、`transmit()`（两个发送路径共用）、诊断代码 |
| `tools/cap-serial.py` | 新增：跨平台断线重连抓串口日志 |
| `tests/host/test_decoder.cpp` | 新增 `estimateBaseUs` / `scoreWaveform` 用例，含真实波形的奇偶性用例 |
| `docs/README.md` | 用户文档：新首页、去方向提示、TF 备份恢复章节、踩坑表 |

TF 与 CC1101 共用 SPI（SCK 40 / MOSI 14 / MISO 39，SD CS 12，CC1101 CS 5，NFC CS 6 拉高）。只在备份/恢复期间挂载 SD，且先 `rx_.suspend()` 摘掉 GDO0 的 GPIO 中断（14:13 之后接收已不走 RMT）。

---

## 7. 已知无害噪声（部分已过时，见 §9.4）

- ~~`E rmt: ---RX buffer too small: 4`~~：**已消失**。这是 RMT 驱动 ISR 层的日志，运行时 `esp_log_level_set` 关不掉；
  2026-11-08 14:13 接收改为 GPIO 中断后 RMT 驱动不再安装，这条不再产生。`cap-serial.py` 的过滤规则保留着，
  用于读旧日志。`app.cpp` 里那句 `esp_log_level_set("rmt", ESP_LOG_NONE)` 已是空操作，注释里已注明。
- 备份期间堆最低会从 264 KB 掉到 232 KB（SD 挂载缓冲），卸载后回收，不是泄漏。
- 看门狗用的是任务 WDT，已确认 sdkconfig 里 `CONFIG_ESP_TASK_WDT_PANIC=1`，panic 回溯会走 UART0 + USB Serial/JTAG。

---

## 8. 2026-11-08 晚间：RMT 发送 / RSSI 扫频 / 接收链路大修

### 8.1 已完成并上机验证

| 功能 | 状态 |
|---|---|
| 发送后端换 **RMT 硬件时序**（位翻转保留为 fallback，`RF_TX_USE_RMT`） | ✅ 三条记录均可开闸 |
| 发送时长按 `RF_TX_TARGET_MS`（700ms）反推重复次数 | ✅ |
| 发送前把波形**只裁尾**成整数帧（修 `rollDOWN` 要按多次） | ✅ |
| 首页第 3 项 `SCAN RF`：7 频点 RSSI 扫描，ENT 采用最佳频段 | ✅ |
| 采集页空态显示实时 RSSI + 峰值保持 | ✅ |
| `rfcap` 的 NVS 撕裂写入自愈（`count=0` 但 `items` 完好时按字节数反推） | ✅ 救回 3 条 |
| `rfcap` 写入顺序改为 `magic → items → count`（count 作提交位） | ✅ |

### 8.2 四个坑（这次踩得很深，务必记住）

1. **`rmt_config(gpio_num)` 会独占 GDO0。** 它把该引脚配成 RMT 输出并保持空闲电平低，
   等于 ESP32 自己把 CC1101 的解调输出按住 —— **采集从此一个边沿都收不到**，
   而芯片侧 `ms=0x0D`(RX)、`iocfg0=0x0D` 全部正常，极难察觉。
   修法：`configureRmt()` 之后立刻 `rmt_set_idle_level(ch,false,*)` + `pinMode(pin,INPUT)`，
   只在 `sendRmt()` 里用 `rmt_set_gpio()` + `GPIO.enable_w1ts` 临时接管。
   自查手段：`RF_GDO_TEST=1` 编译，开机把 IOCFG0 设成 0x2F/0x6F 静态电平再 `digitalRead`。

2. **`onEdge()` 存错电平。** 原代码先把 `level_` 翻转再存，导致每条记录的"时长"是上一段的、
   "电平"是下一段的 —— 因为相邻段电平必然相反，**每条记录电平都反了**，解码器永远找不到同步头。
   应存"刚结束那段"的电平。

   > **2026-11-09 补充：这个坑当时只修了一半。** 修法从"翻转"改成了盲翻 `level_ = level_ ? 0 : 1`，
   > 只要 ISR 漏掉一个边沿（`micros()` 抖动、噪声毛刺被门限吃掉、NVS 提交关 cache），
   > 之后**整段的电平标签就全部反过来**，而且不会自愈。现在 `onEdge()` 直接
   > `readPin()` 读 GPIO 寄存器拿真实电平（`IRAM_ATTR` + 直接读 `GPIO.in` / `GPIO.in1.data`，
   > 不调 `digitalRead()`，NVS 提交关 flash cache 期间也安全）。
   > 这个 bug 在真实波形上留下了可识别的指纹，见 **§8.3 第 1 条**。

3. **`push()` 的长低电平判断用错变量**：用的是 `last.us`（上上段时长），应该用当前的 `us`。

4. **松开遥控后不成帧**：原来判断 `last.level == 0`，但真正在进行的那个低电平还没被记录，
   最后一条记录的是"高"。应改看当前线路电平 `level_`：
   `if (level_ == 0 && micros() - lastUs_ >= frameGapUs) closeFrame(false);`

### 8.3 已知遗留（2026-11-09 更新）

- **~~同码值 ≠ 同波形质量~~ → 已定案**：同一个按钮两次采集都可能解出 `891A2B`，但第一次能开闸、第二次不行。
  原来以为是"脉宽被噪声拉歪"，**不是**。拿 `docs/signals-2026-11-08.log` 里的真实波形数了一下：

  | 记录 | 能开闸？ | 帧内 ~31T 长脉冲的下标 | 奇偶 |
  |---|---|---|---|
  | `rollUP`(234567) | ✅ | 7 / 57 / 107 / 157，**间距正好 50 = 一个 EV1527 帧** | 全是奇数 |
  | `maindoor`(123456) | ❌ | 10 个，间距不规则 | **5 个奇数、5 个偶数** |

  store 只存**脉宽**、不存电平，电平是靠下标奇偶推出来的（`pulses[0]` 归一化成高，所以每个同步低电平**必须**落在奇数下标）。
  `maindoor` 的长脉冲横跨两种奇偶 = **电平标签在突发中途被整体反转了**，回放发出去的是实际空中波形的**补码**。
  这正是 §8.2 第 2 条那个盲翻 `level_` 的指纹，等于**用真实硬件数据确认了 `onEdge()` 那个 bug 就是现场故障的根因**。

  > 上面引用的 log 和码值都是**本机留档 / 占位符**，不在仓库里，原因见 §11。结论本身不依赖具体数值，
  > 只依赖「长脉冲下标的奇偶是否一致」这个判据——这个判据已经固化进 §8.5 的质量分和 `tests/host`。

  ⚠️ **推论：NVS 里现存的 4 条记录都是修复前采的，波形可能已经反了，应该全部重采一遍再判断能不能开闸。**
  现在这类记录会在屏幕上显示成低 `q` 分（`maindoor` = `q39`，`rollUP` = `q97`），判据见 §8.5。

- **~~码值右移一位~~ → 大概率同一个 bug**：真实遥控采集常得到 `891A2B` 而不是 `123456`（= 123456 右移一位、最高位补 1）。
  §9.2 改成"最长低电平优先"后仍存在。电平反转会让解码器把某一段数据低电平误认成同步头，多解析一对 → 整体右移，
  和上面是同一个来源。**修了 `onEdge()` 之后需要重新采一次确认，本轮没有上机验证。**
- 环境底噪重：315 常驻 `rssi≈-85 dBm`、`isr≈2000 次/秒`。这是波形不稳的根源。
  没试过的一个方向：`MDMCFG4` 的 RX 滤波带宽现在是 203 kHz，OOK 9.6 kbps 其实用不到这么宽，
  收到 58–100 kHz 能显著压底噪——**这是改硬件行为，必须上台子测，不要盲改**。
- 偶尔仍把残帧误判成 `PT2262 00007F`。
- PT2262 三态没有真正打包：`classifyPair()` 把浮空(1T/1T)塌缩成逻辑 0，不同遥控可能撞成同一个码值。
  改了会影响显示码值和 NVS 去重语义，**没动**。

### 8.4 调试开关

| 宏 | 默认 | 作用 |
|---|---|---|
| `RF_DIAG` | **1**（开） | 串口诊断 + 1 Hz 强制重绘 + 屏幕"还活着"指示，见 §2。`pio run -e diag-off` 关掉 |
| `RF_RX_SELFTEST` | 0 | 开机自发自收：RMT 发一帧已知波形，用接收链路解回来 |
| `RF_GDO_TEST` | 0 | 静态验证 GDO0 是否跟随 IOCFG0（查上面的坑 1） |
| `RF_SCAN_SELFTEST` | 0 | 开机扫一遍 7 个频点并打印 RSSI |

心跳 `[hb]` 已带 `susp / rp(脉冲数) / rd / pend / ms(MARCSTATE) / iocfg0 / gdo / isr / mind`，
排查接收问题时先看这几个。

### 8.5 逐条波形质量分 `q`（2026-11-09 新增）

§10 第 8 条那个待办做完了。采集页和发送页每行都带 `q00`–`q99`，
**解码成功的帧也算分**——§9.1 的门限只管入库，管不了"入库之后哪条好"。

`scoreWaveform(pulses, count, baseUs)`（`src/ook_decoder.cpp`），满分 100：

| 权重 | 判据 |
|---|---|
| 60% | 落在 1T 或 3T 窗口（±~30%，和 §9.1 的 `looksLikeOok()` 用同一套窗口，两者不能对"干净脉冲"有不同看法）的脉冲占比。≥6T 的同步低电平按设计不计入 |
| 40% | 最长低电平离理想 31T 有多近（偏差 ≥50% 记 0 分） |
| 一票否决 | 帧内 ≥20T 且 ≤45T 的长脉冲**横跨两种下标奇偶** → 分数封顶 **39**，掉进和噪声同一档 |

第三项是 §8.3 那个电平反转的专用检测：反转之后形状分和同步分都还是接近满分，只有奇偶性能看出来。
`maindoor` 就是这样从"看起来没问题"变成 `q39` 的。

`baseUs` 不再取最小值：接收机噪声门限会把 `RF_NOISE_US`(150µs) 以下的毛刺并进邻居，
偶尔漏网一个尖峰就能把最小值拖到远低于真实 1T，所有窗口全落空。
改成 `estimateBaseUs()`——**取"至少有 4 个其它脉冲和它相差在 ±20% 以内"的最小脉宽**
（真 EV1527 一帧有 24 个脉冲落在 1T 上，4 是很松的门槛；噪声宽度一个支持者都没有）。
`looksLikeOok()` 也换成用它了。

分数只存 RAM（`quality_[]`），`begin()` / `replaceAll()` 时重算。**故意不进 NVS blob**——
`sizeof(Capture)` / `sizeof(SavedSignal)` 一变，现存不可再生的信号数据就读不回来了。

### 8.6 顺手修掉的三个持久化 bug（2026-11-09）

1. **`hits` 计数从来没写进 NVS。** `CaptureStore::hit()` 直接改内存就返回，掉电全丢。
   现在改成打脏标记 + `flush()` 批量落盘（`RF_STORE_FLUSH_MS` = 30 s）：
   按住遥控每秒会 bump 几十次 `hits`，每次都重写 5 kB blob 是不行的。
   主循环里 `store_.flush()` 紧跟在输入处理之后。
   ⚠️ **这个批量写之所以安全，全靠接收 ISR 用 `readPin()` 直接读 GPIO 寄存器而不是 `digitalRead()`**——
   NVS 提交会关 flash cache，那时调 `digitalRead()` 会取指失败。改 ISR 的时候别把它换回去。

2. **`SavedStore` 的 RAW 去重只看脉冲数。** RAW 条目没有码值可比，原来的判据是
   `!valid && items_[i].count == capture.count` —— **存过一条 160 脉冲的 RAW 之后，
   以后每一条 160 脉冲的 RAW 采集都被当成重复，永远存不进去，而且没有任何提示。**
   现在加上 `similarTrain()`（逐脉冲 ±25% 比对，吸收廉价超外差接收机的抖动）。
   `similarTrain()` 从 `capture_store.cpp` 的匿名 namespace 提到了 `capture_store.h`，两个 store 共用。

3. **列表满时 `remove(0)` 会立刻重写一次 NVS，紧接着插入又写一次。**
   拆成私有的 `remove(index, persist)`，满员淘汰走 `persist=false`，由后面的插入统一落盘。

### 8.7 一个**没有**改的不一致（改之前先想清楚）

`rfcap`（采集列表）和 `rfsave`（永久保存）的撕裂写入自愈强度是**反的**：

| | 写入顺序 | 恢复条件 | 撕裂后果 |
|---|---|---|---|
| `rfcap` | magic → items → **count**（count 当提交位） | magic 对 + `bytes % sizeof(Capture) == 0` | 按字节数反推条数，**能救回** |
| `rfsave` | magic → count → items | magic 对 + `bytes == sizeof(SavedSignal) * count`（必须**精确相等**） | 条数对不上就**整个列表作废** |

也就是说：**不可再生的那一份（永久信号）恢复条件反而更严**。写入中途掉电，
`rfcap` 大概率救回来，`rfsave` 8 条全没。

没直接改的原因：把 `rfsave` 放宽成"按字节数反推"会改变不可再生数据的恢复语义，
而这个改动**只能靠拔电测**——必须真的在 `putBytes` 中途断电才能验证，
本轮没有硬件在手，不敢盲改。**上台子之后的做法**：先 `BACKUP TF`（TF 卡是第三份拷贝，
`RESTORE TF` 走的是 `replaceAll()`，和 NVS 撕裂无关），再改恢复逻辑，再反复拔电验证。

---

## 9. 2026-11-08 夜间（第二轮）：采集质量过滤 + UI/按键改造

### 9.1 RAW 保留 + 波形质量门限

解码失败的帧不再直接丢弃，波形照样入库、显示成 `RAW 160p` 这类行（回放走原始波形，不依赖码值）。

但必须过 `looksLikeOok()` 两道门限，否则噪声会在 20 秒内把 16 条列表塞满（实测过）：

1. **必须有 ≥10T 的长低电平（同步头）**——没有同步头的残帧发不出去，留着只会误导；
2. **≥80% 的脉冲宽度落在 1T 或 3T 附近**（±~30%）——真遥控只有这两种宽度，噪声是随机的。

采集页同时改成**列表优先**布局：顶部一条 5px 高的 RSSI 条（`-86dBm pk-80dBm` + 填充条），
下面 6 行记录；`RAW` 行整体偏暗。

> 2026-11-09：这两道门限只决定"能不能入库"，管不了入库之后哪条好——每行现在带 `q00`–`q99`，见 **§8.5**。
> `looksLikeOok()` 找 1T 的方式也从"取最小脉宽"换成了 `estimateBaseUs()`（原因同上）。

### 9.2 解码：同步头改成"最长低电平优先"

`decodeFrame()` 原来按顺序取第一个 `≥6T` 的低电平当同步头。3T 的数据低电平被毛刺拉长后也能过
这个门限，而且它出现得比真正的同步头早 → 多解析一对 → 整个码值右移一位。
现在把所有候选**按低电平从长到短排序**再试：一帧里同步是 31T、数据最多 3T，差 10 倍，最长的那段必然是同步头。

> 码值偏移不影响回放，只影响显示与去重。

### 9.3 按键与界面（用户明确要求，已实现）

| 键 | 作用 |
|---|---|
| `ENTER` | 进入菜单 / 确认 |
| `` ` ``（左上角标 **esc** 的键） | 返回 |
| `del`（backspace） | 返回 |
| `,` / `/` | **左右移动**，在 YES/NO 之间选择 |
| `;` / `.`（另支持 `j` / `k`） | 上下移动 |

改动原因：YES/NO 是**横向排列**，用户直觉会用左右键去选，而 `,`/`/` 原来绑的是"返回/确认"，
一按就误触发（尤其删除时）。

其他界面改动：

- 删除（采集页/发送页按 `D`）改为**弹确认框、默认 NO**，左右键选择（`ConfirmKind` 区分保存/删除采集/删除保存）。
- 底部提示改为**分色**：按键名绿色（`kColOk`），说明灰色。结构体 `FooterHint{key, text}[]`。
- 发送页底栏 `D del` → `del del`（前绿后灰），`DEL back` → `esc back`。
- 首页标题 `433 CAPTURE` → **`RF CAPTURE`**。

### 9.4 心跳诊断字段（排查接收问题必看）

`[hb]` 现在带：`susp`(中断是否被摘) / `rp`(已收脉冲数) / `rd`(丢弃边沿) / `pend`(待成帧脉冲数) /
`ms`(MARCSTATE，0x0D=RX) / `iocfg0` / `gdo`(GDO0 当前电平) / `isr`(ISR 进入次数) / `mind`(最小边沿间隔)。

判读：

- `isr` 不涨 → 引脚根本没有边沿 → 先查 GDO0 是否被别的外设占住（§8.2 第 1 条就是这个坑）
- `isr` 涨但 `rp` 不涨 → 边沿都短于噪声门限（解调器抖动）
- `rp` 涨、`rejected` 涨而 `captured` 不涨 → 成帧成功但解码失败

> 整条 `[hb]` 由 `RF_DIAG` 控制（§2）。`diag-off` 环境下心跳不打印，别以为是死了。

## 10. 换到新电脑继续开发的清单

1. 项目就在 NAS：macOS `/Volumes/Project/2026/cardputer`，Windows 映射成 `Y:\2026\cardputer`——同一目录。
   **不要另存一份到会话目录**，两边同时改会冲突。
2. 装 PlatformIO Core，首次 `pio run` 自动下载 `espressif32@6.13.0`（几百 MB，耐心等）。
3. 烧录：macOS/Linux `./tools/run-device.sh`；Windows `.\tools\run-device.ps1`。
4. ⚠️ **烧录后板子停在 ROM 下载模式；再烧一次前要手动进下载模式：按住 `G0` 键再插 USB。**
   串口没输出 / esptool 连不上，绝大多数是这个原因，不是固件挂了（今晚为这个白折腾过两次）。
5. 抓长日志用 `tools/cap-serial.py`（需要 pyserial）：
   macOS 用 pio 自带的 `/opt/homebrew/opt/platformio/libexec/bin/python3`；
   Windows 用 `%USERPROFILE%\.platformio\penv\Scripts\python.exe`。
6. 调试开关都在 `src/rf_config.h`（清单见 §8.4）。三个自测开关默认 `0`，用编译参数打开：
   ```bash
   PLATFORMIO_BUILD_FLAGS="-DRF_GDO_TEST=1" pio run -e cardputer-adv -t upload --upload-port <端口>
   ```
   - `RF_GDO_TEST`：开机静态验证 GDO0 是否跟随 IOCFG0（查 §8.2 第 1 条那个坑）
   - `RF_RX_SELFTEST`：开机自发自收，用接收链路解一帧已知波形（期望 `123456`）
   - `RF_SCAN_SELFTEST`：开机扫一遍 7 个频点并打印 RSSI
   `RF_DIAG` 默认 `1`，关掉用现成的环境：`pio run -e diag-off -t upload`。
7. 硬约束（§5）：`platform` 锁 `6.13.0`、`src/` 按 **C++11** 编译、禁止 `erase_flash`/擦 NVS/改分区表。
8. ~~下一步的待办：给采集加逐条波形质量判据~~ → **2026-11-09 已完成，见 §8.5。**
   当时设想的是"完整 50 脉冲 / 同步头 31T±20% / 脉宽离散度"，实际没有照做：
   硬卡 50 脉冲会误杀所有跨帧采集（真实记录常常是多帧连在一起，`rollUP` 就是 160 脉冲装 3 个整帧加一个残帧），
   换成"1T/3T 占比 + 同步偏差 + 下标奇偶一致性"三项加权，其中第三项才是真正抓住现场故障的那一条。
9. **下一步真正该做的（按顺序）**：
   1. 上机跑第 4 节的按键压力测试，然后 `diag-off` 再跑一轮 —— 这是唯一能给 §3 定案的办法。
   2. **把 NVS 里现存 4 条记录全部重采**（都是修 `onEdge()` 之前采的，见 §8.3），重采后确认：
      `q` 分是否都上到 90+、码值是否还是右移一位（`891A2B` 而不是 `123456`）。
   3. 如果 2 里码值仍右移，再回头查 §9.2 的采集窗口跨帧问题。
   4. 上面 1、2 都过了，再开始 **§12 的多功能改造**——那是在这个基座上做大重构，
      基座没验过就动手等于把两个未知叠在一起（见 §12.5 的风险要点）。

---

## 11. 仓库与敏感内容（2026-11-09 首次入库）

远端是 `https://github.com/Dloach/RFcapture`，**public**。

决定：代码公开，**真实码值和波形不公开**。理由不是怕代码被抄，而是这个仓库同时含有一份可用的
回放实现——真码值加进去就等于把自家门的钥匙贴在网上，任何人 clone 下来烧个 CC1101 就能开。

入库前做的脱敏：

| 文件 | 处理 |
|---|---|
| `docs/signals-2026-11-08.log` | **从不入库**，`.gitignore` 排除 `docs/signals-*.log` 和 `*.local.log`。文件仍留在本机 |
| `docs/DEBUG-2026-11-08.md` | 码值换占位符；§2.1/§2.2 的原始脉宽序列整块删掉，只留结构性读数（1T/3T/31T 比例、同步头间距） |
| `docs/DEV-STATE.md` §1 / §8.3 | 码值换占位符，并注明是占位符 |
| `README.md` / `docs/README.md` | 举例码值换占位符；把「本文码值都是示例」写进正文开头 |
| `src/app.cpp` | `RF_RX_SELFTEST` 的期望码值改成 `0x123456`（宏默认关，但源码在仓库里） |
| `src/ook_decoder.cpp` | 解释「右移一位」的注释改用占位码值举例 |
| `tests/host/test_decoder.cpp` | 断言里的码值换占位符；奇偶用例的注释不再引用真实 log 路径 |

占位符映射（**改文档时保持一致，别再引入真值**）：

| 真值 | 占位符 |
|---|---|
| 315 铁门 | `123456`（右移一位后的形态 `891A2B`） |
| 433 卷帘 上升/停止/下降 | `234567` / `23456D` / `23456F` |
| 2026-11-08 新采的三条 | `345678` / `345674` / `345671` |
| NFC 蓝色方块门禁卡（MIFARE Classic 1K） | `3CDEAD01` |
| NFC 手机卡模拟态读到的随机 UID | `2BEEF001` |

NFC 这两个占位符**故意保留首字节**（`0x3C` / `0x2B`）：§12.4 的推理用的就是「首字节既不是 NXP 的
`0x04`、也不是表示随机 UID 的 `0x08`」这个性质，换成 `123456` 那种就把论据一起换掉了。

另外，**2026-11-10 之后源码里不再出现任何真实 NFC UID**。`src/nfc_emu.cpp` 开机从板子自己的
`/nfc/capture.log` 里读记录，UID 只存在于设备 SPIFFS 和这份未入库的日志里。这是刻意的：
仓库是 public，而 `capture.log` 里是真实门禁凭据——和 `docs/signals-*.log` 不入库是同一条理由。
**同理，`nfc-emu` / `nfc-spike` 的串口输出（含 `[emu] rec[n] uid=...` 和整份 log 回灌）等同于
RF 脉宽 dump，别贴到公开 issue / 论坛 / pastebin 上。**

三点要注意：

1. **git 历史也是敏感面**。真值一旦提交过，删文件没用。本次是在 `.git` 从未推出去之前重建的历史
   （重新 `git init`、按模块重放提交、丢掉那条加原始 log 的提交），所以现在整个历史里都不含真值。
   **以后如果不小心把真值提交了，同样要重写历史，不能只删文件。**
2. **设备名保留了**（`maindoor` / `rollUP` / `rollSTOP` / `rollDOWN`）——门的种类在 README 里本来就写了，
   名字本身不构成额外泄露，留着叙述才读得通。
3. `RF_DIAG=1`（默认）开机就会把 NVS 里所有脉宽 dump 到串口。任何人拿到板子接串口就能读出来——
   这是设计如此（排障必需），但意味着**别把这个 dump 贴到公开 issue / 论坛 / pastebin 上**。

---

## 12. 多功能系统改造（2026-11-09 立项）

目标：把固件从「RF capture 专用机」改成多功能系统，RF capture 只是其中一个 app。
计划加的功能：键盘模拟（USB 有线 + BLE）、NFC、WiFi sniffer、SSH 工具。

**架构已定：单体固件 + 运行时 app 注册表**，不走 launcher / 多分区镜像。
理由在下面 §12.2 —— 瓶颈是 RAM 和 NVS，不是 flash，所以多分区换不来隔离收益，
却要每个镜像各背一份 ~600 KB 的 M5GFX + Arduino core。

**第一个功能是 NFC**（理由见 §12.3），依赖已经验证通过（§12.4）。

### 12.1 分区表：三块白送的空间（查证于 core 2.0.17 的 `default_8MB.csv`）

`platformio.ini` 用的是 `board_build.partitions = default_8MB.csv`，实际布局：

| 分区 | 偏移 | 大小 | 现状 |
|---|---|---|---|
| `nvs` | 0x9000 | **20 KB** | `rfcap` 5.4 KB + `rfsave` 2.8 KB ≈ 已占 40%+，**不可再生** |
| `otadata` | 0xE000 | 8 KB | 在，说明 OTA 机制已就位 |
| `app0` | 0x10000 | 3,342,336 B (3.19 MB) | 用了 635,945 B = 19% |
| `app1` | 0x340000 | **3.19 MB** | **完全空着** |
| `spiffs` | 0x670000 | **1.5 MB** | 没挂载，完全空着 |
| `coredump` | 0x7F0000 | 64 KB | 空 |

三个结论，按重要性排：

1. **NVS 只有 20 KB，这是全盘最紧的资源，而且长不大**（改分区表 = 毁掉采集数据，违反 §5 硬约束）。
   所以新功能的一切持久化状态（WiFi 凭据、SSH host key、按键宏、BLE 配对、NFC 采集记录）
   **都不许进 NVS**，放 `spiffs` 那 1.5 MB 或 SD 卡。
2. **flash 完全不是瓶颈**：五个功能全上估 ~1.65 MB，app0 装得下还剩 1.5 MB。
3. **`app1` 建议不做 launcher，做已知可用的 recovery 黄金镜像**——采集数据不可再生、
   烧录又要按 G0 进下载模式，有个能 OTA 回去的兜底很值。OTA 写 app1 不碰 NVS，安全。

堆（实测，见 §7）：**264 KB free，SD 挂载时最低 232 KB**。而 WiFi 驱动 ~50 KB、
Bluedroid BLE ~60 KB、libssh + mbedTLS 握手峰值 ~80 KB、NFC ~20 KB——**同时开必爆**。
运行时互斥是硬要求，不是设计选择。

### 12.2 五个功能的可行性

| 功能 | 结论 | 关键依据 |
|---|---|---|
| **USB 有线键盘** | 硬件可以，**当前编译配置关掉了** | 见下面「USB 那一坑」 |
| **BLE 键盘** | 可行，最贵之一（~60 KB 堆 / ~350 KB flash） | core 2.0.17 自带 `BLEHIDDevice.h`；S3 只有 BLE 没有经典蓝牙 |
| **NFC** | **可行，硬件已在手** | 见 §12.4，全部查证过 |
| **WiFi sniffer** | 可行，代价是 RF 采集必须停 | `esp_wifi_set_promiscuous()` 在 IDF 4.4 的 `esp_wifi.h` 里就有；无 PSRAM → 忙信道必丢包 |
| **SSH** | 技术可行，法律上要慎重 | 见下面「SSH」 |

**USB 那一坑（已查证，别踩）**：`boards/cardputer_adv.json` 里 `hwids` 是 `0x303A`——乐鑫自己的 VID，
所以 USB-C 接的确实是 S3 **原生 USB**，不是 UART 桥，硬件上能做 HID。
但 `extra_flags` 里是 `-DARDUINO_USB_MODE=1`，而 core 的 `boards.txt:144` 写着这个值等于
**「Hardware CDC and JTAG」**；`USBHIDKeyboard` 整个类被 `#if CONFIG_TINYUSB_HID_ENABLED` 包着
（`libraries/USB/src/USBHID.h:20`），要的是 **USB-OTG / TinyUSB，即 `ARDUINO_USB_MODE=0`**。
S3 只有**一个 USB PHY**（`hal/usb_phy_types.h`：`USB_PHY_CTRL_OTG` vs `USB_PHY_CTRL_SERIAL_JTAG`，二选一）。

好消息：预编译库的 `tools/sdk/esp32s3/sdkconfig` 里 `CONFIG_TINYUSB_ENABLED=y`、HID/CDC/MSC/MIDI/DFU/VENDOR 全开，
**不用重编 IDF**。坏消息：切到 mode 0 会同时改掉串口控制台和**烧录方式**
（变成 1200-bps-touch 复位，不再是 `--before usb_reset`），正好撞上 §10 第 4 条那个 G0 下载模式的坑。
**这一步必须硬件在手才敢动，动之前先确认能刷回来。**

**SSH**：[ewpa/LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32) 明确写了在 **core 2.0.17 + ESP32-S3** 上测过，
client / server / SCP 都有，还给了 RAM 调参宏——技术上唯一能直接用的。
但 libssh 是 **LGPL-2.1**，而本仓库 public 且**没有 LICENSE**，引进来就有传染性义务。
代价 ~300–500 KB flash、握手峰值 ~60–100 KB RAM。
**如果只是想连局域网设备敲几条命令，telnet 客户端 ~10 KB、纯 lwIP socket、core 2.x 一定能编。**

**射频互斥（这条决定架构）**：CC1101 的 OOK 接收是 GDO0 → GPIO CHANGE 中断 + `micros()` 拿 1 µs 分辨率，
WiFi/BLE 的 coex 任务和中断会带来几十 µs 到 ms 级 ISR 延迟抖动，**开着就一定丢边沿**，
§8.3 那个「底噪重、`isr≈2000/s`」会被放大一个量级。
所以进 RF 采集前必须 `WiFi.mode(WIFI_OFF)` + `BLEDevice::deinit(true)`，退出再恢复。
Bruce / Marauder 那一类固件也都是模式切换，没有谁真并发。
**RMT 不冲突**：WiFi/BLE 都不占 RMT，`ook_tx` 的 legacy RMT channel 0 是安全的。

### 12.3 参考固件：能拿的和不能拿的

| 项目 | License | 判定 |
|---|---|---|
| **Bruce** (pr3y) | **AGPL-3.0** | **不能** —— 双重否决：AGPL + `platformio.ini` 要求 pioarduino / core 3.x，正是 §5 锁死不能升的那个 |
| **Evil-M5Project** | **MIT** | **最佳架构参考** —— 明确支持 Cardputer ADV，README 推荐 core **2.1.4 及以下**，和本项目的 6.13.0 最接近 |
| **M5Unit-NFC** | **MIT** | **直接可用**，官方就有 Cap CC1101 + Cardputer ADV 的 env（§12.4） |
| **ESP32 Marauder** | **MIT** | sniffer 逻辑可摘（挑代码，别整体 build） |
| **M5Launcher** (bmorcelli) | MIT | 走多分区路线时再参考 |
| **LibSSH-ESP32** (ewpa) | **LGPL-2.1** | 技术可用，法律上先决定本仓库 license |
| T-vK/ESP32-BLE-Keyboard | **无 LICENSE 文件** | **别拷**——等于 all rights reserved。用 core 自带的 `BLEHIDDevice` |

### 12.4 Stage 0 已通过：NFC 依赖能共存

`pio run -e nfc-spike` **编译通过**。这是动手前唯一真正的未知项，现在消掉了。查证链条：

| 查证项 | 结果 |
|---|---|
| 工具链 | M5Unit-NFC 官方的 `CapCC1101_*_CardputerADV_*` env 用 `[arduino_latest] = espressif32 @ 6.12.0`，和本项目锁的 `6.13.0` **同一条平台线**（都是 core 2.x / IDF 4.4） |
| 解析出的版本 | `M5Unit-NFC@0.1.2` → `M5UnitUnified@0.6.4` → `M5HAL@0.1.4` → `M5Utility@0.3.1`，与 `M5Cardputer@1.1.1`（带 `M5GFX@0.3.0` / `M5Unified@0.3.0`）**无冲突** |
| CS 引脚 | `CapST25R3916` 的默认地址是 `0x06 /* SPI CS pin */`，**正好等于 `RF_NFC_CS = 6`** |
| SPI 总线抢占 | `wiring::addSPI()` 里是 `if (!SPI.bus()) SPI.begin(...)`——**总线已被 `cc1101.cpp:159` 开过就直接复用**，不会用它的引脚表覆盖 G40/G39/G14 |
| 万一 CC1101 没识别到 | `spiPins()` 取 `M5.getPin(sd_spi_sclk/miso/mosi)`，即板子的 SD/SPI 共享总线，所以走 `NO RADIO` 降级路径时 NFC 仍能拿到正确引脚 |
| SPI mode 冲突 | CC1101 是 mode 0，ST25R3916 是 **mode 1 @ 10 MHz**。两边各自在 `beginTransaction(SPISettings)` 里，不冲突 |
| CS 管理 | `spics_io_num = GPIO_NUM_NC`，每个 unit 自己拉 CS——和现在 CC1101(G5) + SD(G12) 的用法一致 |
| 能力范围 | NFC-A/B/F/V，Detect / Dump / ReadWrite / ValueBlock / NDEF / **Emulation** / PolicyOverride，MIFARE Classic 带 crypto1 读写 |
| 尺寸 | spike 单独编 = 619,225 B / 23,440 B（含整套 M5GFX）。按符号名归类，detect+identify 这条路径只占 **~45 KB** NFC/ST25R3916 代码，比外部估的 60–150 KB 小 |

**能力边界（原来那句「MIFARE Classic 不支持」说得太粗，逐条查证后重写）**：

| 能力 | 支持 | 证据 |
|---|---|---|
| 读 UID / ATQA / SAK / 类型 / 容量 | ✅ **已上机验证** | 本节末尾那条真实记录 |
| MIFARE Classic 整卡 dump | ✅ **API 有** | `nfc_layer_a.hpp:329` `dump(mkey)`；`mifare.hpp:53` `DEFAULT_KEY{0xFF×6}` 就是标准出厂传输密钥 |
| MIFARE Classic 带 crypto1 读写 / value block / 访问位 | ✅ API 有 | `mifareClassicAuthenticateA/B`、`ReadAccessCondition`、`{Read,Write,Increment,Decrement,Transfer}ValueBlock` |
| 未知密钥的破解（nested / darkside） | ❌ **完全没有** | 全库 grep `nested\|darkside\|crack\|recoverKey` 只命中一条无关的 NDEF `apply_nested_message` |
| NFC-A 模拟，**任意 4 或 7 字节 UID** | ✅ **可以** | `emulation_layer_a_ST25R3916.cpp:151` `memcpy(wbuf, picc.uid, picc.size)`，`:163` 按 UID 长度切 `REG_AUXILIARY_DEFINITION`；`:142` 明确 **10 字节 UID 不支持** |
| MIFARE Classic 的**完整**模拟（应答 crypto1 认证） | ❌ **没有** | 模拟侧全文件 grep `crypto1\|crypt\|auth` **零命中**；`EmulationLayerA::begin(picc, ptr, size)` 是内存级模拟，给的是明文缓冲区 |

**所以「NFC capture」到底能做到什么，取决于门禁读卡器怎么认证**：

- **只认 UID 的门禁**（很多老系统就是这样）→ **能复制能回放**。读一次 UID，再用 NFC-A 模拟发同一个 UID 就行，这条路是通的。
- **要做 crypto1 认证的门禁** → **不能**。既能 dump（如果密钥是默认的 FF×6 或已知），但**模拟侧答不出认证**，所以回放不了。
- **密钥被改过、又不知道密钥** → **连 dump 都不行**，而且库里没有破解能力，这一条是硬边界。

UI 上按这三档给提示，别笼统写「支持 MIFARE Classic」。

spike 代码在 `src/nfc_spike.cpp`，整份被 `#ifdef RF_NFC_SPIKE` 包着。
**三个出货 env 都不定义这个宏，所以它编译成空 TU，产物字节不变**——已验证：
加了这个文件之后 `cardputer-adv` 仍是 **635,945 B / 26,264 B**，和加之前一模一样。
它同时也是将来上机测 NFC 的载体，别删。

**2026-11-09 上机确认（不只是编译）**：spike 刷进真板子后串口报
`[spike] ready=1 heap=356180 min=350296`——`addSPI()` 和 `units.begin()` 都返回真，
即 ST25R3916 **在 CS=G6 / SPI mode 1 / 10 MHz 上真的应答了**，上表里那些「查过源码所以应该没问题」
的推断有了一条硬证据。堆数字也值得记：356 KB free / 350 KB min，比出货固件的 264/232（§10）
高出一大截，因为 spike 没挂 SD、没初始化 CC1101 和 RMT。**别把 356 KB 当成 NFC app 可用预算**，
真 app 要和现有那套共存，预算按 232 KB min 那条线算。

心跳行是故意加的：ESP32-S3 的 USB-CDC 口每次复位都会重枚举，`setup()` 里那句一次性报告
基本抓不到，所以 `loop()` 每 2 秒重播一次状态。

顺带记一条踩坑：**在 G0 下载模式下用 pyserial 手动拉 DTR/RTS 复位会把它再次送进下载模式**
（`boot:0x23 DOWNLOAD`），因为 DTR 映射到 IO0。要么用 esptool 的
`--before default_reset --after watchdog_reset run`，要么像 spike 这样让固件自己周期性报告状态、
抓串口时完全不碰信号线（`p.dtr = False; p.rts = False` 再 `open()`）。

**无卡健康探测（同样已上机）**：连续 4 分钟、119 次心跳里 `detect()` 一次都没命中，
`loops` 每秒只涨 1——正好等于 `detect(vector)` 的 1000 ms 默认超时，说明轮询在跑、
每秒约 1000 次 REQA 发出去但没有任何东西应答。「场上没卡」和「场没起来」在这个现象下
完全无法区分，所以 spike 加了寄存器转储，结果：

```
[spike] regs ok=1 opctl=CB(en=1 tx_en=1) mode=09(targ=0) txdrv=D0
```

| 寄存器 | 值 | 判读 |
|---|---|---|
| `REG_OPERATION_CONTROL` | `0xCB` | `en`=1 振荡器+稳压器已起；`rx_en`=1 接收开着；**`tx_en`=1 场是开的**；`wu`=0 不在唤醒模式；低两位=0b11 外部场检测器自动（对应 `unit_ST25R3916.cpp:250`） |
| `REG_MODE_DEFINITION` | `0x09` | `om`=0x08 **就是 ISO14443A**（见 `ST25R3916_definition.hpp:31` 的 `ISO14443A = 0x01 << 3`，别按数据手册印象猜成 0b000）；`targ`=0 是 initiator/poller 不是模拟；`nfc_ar`=0b01 自动恢复 |
| `REG_TX_DRIVER` | `0xD0` | 天线驱动等级非 0 |

**结论：无卡能查的项目全部通过，NFC 通路已验证到天线为止。**

**2026-11-10 整条链路读通了**（用手机贴着试的，手机在卡模拟态下会应答 REQA）：

```
[ 32577][V][nfc_layer_a.cpp:238] detect(): ATQA:0004
[ 32580][V][nfc_layer_a.cpp:246] detect(): Detect:ATQA:0004 SAK:08 2BEEF001 (MIFARE Classic 1K)
[ 32813][V][nfc_layer_a.cpp:298] activate(): ACTIVATED 2BEEF001 0
[ 32819][D][unit_ST25R3916_nfca.cpp:230] nfcaReceive(): Timeout
[ 32820][D][nfc_layer_a.cpp:2010] nfca_request_ats(): Failed to RATS 0
[spike] uid=2BEEF001 type=MIFARE Classic 1K atqa=0004 sak=08 user=752 total=1024
[spike] ready=1 loops=93 hits=1 heap=347788 min=342496
```

`detect → 防冲突 → select → activate → identify` 全部走通，`hits=1`。四点值得记：

1. **`Failed to RATS` 不是故障。** RATS 是 ISO14443-4（ISO-DEP）的东西，MIFARE Classic 本来就不支持，
   `identify()` 试一下、超时、回退，属于正常路径。将来在 app 里别把它当错误报给用户。
2. **`2BEEF001` 是随机 UID。** 真 MIFARE Classic 的 4 字节 UID 首字节一般是 `0x04`（NXP 厂商码），
   这里是 `0x2B`——手机在卡模拟态会给随机轮换 UID（Android 的隐私保护）。
   **所以这条记录只证明链路通，不能用来验证 UID 读取的稳定性**，那一步必须用实体卡。
3. **一次 detect+identify 吃掉约 8 KB 堆**：命中前 355,924，命中后 347,788，min 342,496。
   加上 §12.4 开头那条「真 app 要和现有 RF 那套共存，预算按 232 KB min 算」，这个量级完全放得下。
4. **手机只在亮屏解锁时才应答 REQA**，息屏后 NFC 控制器不响应。
   这也解释了为什么第一轮 4 分钟 119 次心跳零命中——不是硬件问题，是当时场上确实没有可应答的东西。
   拿手机当测试靶子可以，但别指望它随时可用。

**离机采集已验证（spike 现在会自动落盘）**：上机时发现一个反复咬人的问题——
检测记录只存在于串口那一瞬间，抓取窗口没开着就永久丢失，重启也全丢，
前后踩了两次。所以 spike 加了自动写文件，并且**已经端到端验过**：
板子拔掉 USB 靠电池跑了 4.2 分钟，回来插上后日志完好、自动回灌成功。

| 设计点 | 取值 | 理由 |
|---|---|---|
| 存储目标 | SD 优先，SPIFFS 兜底 | 有 SD 就能拔卡在电脑上看；没卡也照样存。**绝不写 NVS**（§12.1：20 KB 不可扩，里面是 4 条不可再生的 RF 记录） |
| 写入方式 | 每条记录 open/append/close | 慢，但台架工具随时会被拔电池，长期持有句柄会丢掉最后一次 flush 之后的全部数据 |
| 记录范围 | 成功（`H`）和 identify 失败（`F`）都写 | 「全部采集信息」包含失败的——答了 REQA 和防冲突却分类失败的才是最有意思的那种，只记成功等于把它藏起来 |
| 去重 | **不去重** | 卡贴在场里每秒产生一条，那是真实发生的事；`gap` 字段记了间隔，离线合并即可 |
| 读回方式 | 开机 5 s 后自动整份回灌串口，之后每 60 s 一次，超 64 KB 只回灌尾部 | **不接键盘**：`src/app.cpp:83` 那个自定义 `PollingKeyboardReader` 背后是 §4 还没定案的按键卡死，spike 不该继承那个坑。开机即回灌也不行——板子一插上就启动，那时宿主还没打开 CDC 口 |
| `uidLen` 字段 | 必记 | NFC-A 模拟只接受 4 或 7 字节 UID（`emulation_layer_a_ST25R3916.cpp:142`），**这个字段直接决定读到的卡能不能回放** |

代价：文件系统代码让 flash 从 640,049 涨到 **702,625 B（+62 KB）**，
SPIFFS 挂载把堆 min 从 342,496 拉到 **318,244（约 -24 KB）**。Stage 2 做真 app 时这两笔都要计入预算。

真实日志（跨两次刷写累积，证明 SPIFFS 扛过了重刷）：

```
# session ms=10213 nfcReady=1 regs=1 opctl=CB mode=09 txdrv=D0 free=351032
# session ms=1600  nfcReady=1 regs=1 opctl=CB mode=09 txdrv=D0 free=351032
H ms=248683 uid=3CDEAD01 type=MIFARE Classic 1K atqa=0004 sak=08 uidLen=4 user=752 total=1024 gap=0    heap=350756
F ms=251734 uid=3CDEAD01                            atqa=0004 sak=08 uidLen=4
H ms=253766 uid=3CDEAD01 type=MIFARE Classic 1K atqa=0004 sak=08 uidLen=4 user=752 total=1024 gap=5083 heap=342616
```

从这三条得到两个 Stage 2 必须处理的事实：

1. **`identify()` 不是每次都能过**——同一张卡、同样的 atqa/sak，三次读取里失败了 1 次（那条 `F`）。
   所以 app 里不能把 identify 当必然成功，UI 要能重试，不能一次失败就报「未知卡」。
2. **`uidLen=4` 落在模拟支持的范围内**，即这张卡在「只认 UID 的门禁」那一档下是可回放的。
   但 UID 首字节 `0x3C` 既不是 NXP 的 `0x04`、也不是 ISO14443-3 表示随机 UID 的 `0x08`，
   而且这三次是在场里连续读的（间隔 5 s），手机在同一个场里也可能保持同一个随机 UID 不变——
   **所以「UID 跨次稳定」这一条仍未证实**，需要把卡拿走、隔几秒再贴一次看是否还是 `3CDEAD01`。
   §12.4 前面那句「UID 稳定性必须用实体卡验」因此还没有销账。

两条踩坑记在这里，将来别再走一遍：

1. **别用裸 `nfcA.request()` 当探测**。它把卡留在 READY 态，而 REQA 只有 IDLE 态的卡会应答，
   于是探测本身会毒化紧随其后的 `detect()`，制造假阴性。要区分「场死了」和「防冲突失败」，
   用 `-DM5_LOG_LEVEL=5` 编译即可——`NFCLayerA::detect()` 本来就在 verbose 级打 ATQA。
   这个开关不会刷屏：`unit_ST25R3916.hpp` 里 verbose 日志是 **0 条**，热的 SPI/transceive 路径全静音，
   19 条都在 `nfc_layer_a.cpp`，且只在 request 成功时才触发。
2. **我原以为 `tx_en` 会随每次操作翻转**（`unit_ST25R3916.cpp:535/541` 那对 field-on/field-off），
   所以打算不读它。实测 `opctl` 稳定报 `tx_en=1`，翻转的是别的路径。上面那个假设是错的，
   但读 `en`（bit7，不随操作翻转）作为主判据这一点仍然成立。
3. **贴手机测试时，「响了」这个信号是有歧义的**——手机自己感到一个 NFC 场也会响/震动。
   当时 spike 只在串口打一行，屏幕上没有可读的命中证据，于是「有声音、屏幕有记录」
   既可能是板子读到了，也可能是手机的提示音加上屏幕那句静态的 `TAP A CARD` 被误读。
   现在 spike 屏幕上常驻 `polls=/hits=` 计数、命中后显示绿色 UID 块并把底行改成 `GOT n CARDS`，
   提示音也从 10 ms 改成 **150 ms / 2500 Hz**（这个长度不可能是手机的提示音）。
   一般教训：**上机验证的证据要落在被验证设备自己的屏幕上，不能只在宿主机串口里。**

### 12.4.1 多协议轮询结果 + 回放（复制卡）的两条硬边界（2026-11-10）

**多协议轮询已经跑通，蓝色圆形门禁的问题按用户决定收尾。**

spike 现在每一轮把 A/B/F/V 四个协议全扫一遍。这不是库支持的用法——`M5Unit-NFC` **没有多协议示例**，
`NFCB/NFCF/NFCV` 的 `Detect.ino` 各自在 `begin()` 之前把 `cfg.mode` 设一次就完事。ST25R3916 是
**单协议芯片**：每条 transceive 路径开头都有 `CHECK_MODE()`，模式不对直接返回 false 并报
`Illegal mode`。要轮询就得在协议之间调用公开的 `configureNFCMode()`（`unit_ST25R3916.hpp:112`）。

**踩到的坑（记下来别再走）**：四个 `configure_nfc_x()` 最后都会调 `nfc_initial_field_on()`，
而它在 `REG_OPERATION_CONTROL` 的 `tx_en` 已经置位时直接报 `Already tx_en` 返回 false
（`unit_ST25R3916.cpp:361`）。`configureNFCMode()` 开头那条 `CMD_STOP_ALL_ACTIVITIES` 停的是状态机，
**不清 `tx_en` 这个寄存器位**。后果是一轮里只有第一个协议能配置成功，后三个每次都失败，
`detect()` 立刻返回，循环空转到 ~150 Hz，串口被 `Already tx_en` 刷屏。
修法：切模式前先 `readOperationControl` 再 `writeOperationControl(opctl & ~(tx_en|rx_en))`，
**但保留 `en`（bit7）**——它不属于场开关，而且 `configure_nfc_v()` 会对同一个寄存器做读改写
（`en_fd_c1|en_fd_c0`），清了等于白清。

寄存器实测证据（`om` 是 `REG_MODE_DEFINITION` 的 bit5:3，对照 `InitiatorOperationMode`）：

```
[spike] mode A=09 B=14 F=1C V=70 (expect om 08 10 18 70)
```

四个 `om` 全对得上（A=ISO14443A `0x01<<3`，B=`0x02<<3`，F=FeliCa `0x03<<3`，V=SubCarrierStream `0x0E<<3`）。

代价：flash 702,625 → **731,541 B（+28.9 KB）**，堆 min 318,244 → **317,956**（基本没动）。
出货固件 `cardputer-adv` 重编仍是 **635,945 B / 26,264 B**，与基线一致。

**用户多卡实测结果**：试了 7 个物体，**全部是 `proto=A`，B/F/V 一次都没扫出来过**。
其中 1 张是真 NTAG 215（7 字节 UID，首字节 `0x04` = NXP 厂商码，硬编码稳定，可回放），
1 个是 MIFARE Classic 4K，其余 5 个报 `ISO14443-4`（`sak=28/20`，`user=0 total=0`）。
5 个 `ISO14443-4` 的 UID 每次都不同——**如果它们是同一个物体**（比如卡模拟态的手机），
这就是随机轮换 UID 的特征；但也完全可能是 5 张不同的卡，现有数据分不出来。

**蓝色圆形门禁**：用户说「就先这样吧」，问题按未定案收尾。最可能的解释是
**125 kHz EM4100 类 LF 卡**——ST25R3916 是 13.56 MHz HF，CC1101 是 sub-GHz，两个都物理上读不到，
属于硬件边界而不是固件 bug。**但这条没有被正式证实**（没有单独确认过那个圆形门禁在 A/B/F/V 下
都是零响应），所以留在这里当「大概率结论」，不当已销账项。

---

**下面是「复制信号开门」这个需求真正撞上的东西。两条边界，都在 M5Unit-NFC 0.1.2 里，
都不是运行时能发现的——第一条会失败关闭并打一行日志，第二条静默失败。**

#### 边界一：`EmulationLayerA::begin()` 只接受 NTAG2xx 和 MIFARE Ultralight

`emulation_layer_a.cpp:40`：

```cpp
if (!(picc.isNTAG2() || picc.type == Type::MIFARE_Ultralight)) {
    M5_LIB_LOGE("Not support %u %s", picc.type, picc.typeAsString().c_str());
    return false;
}
```

MIFARE Classic 被直接拒绝，所以蓝色方块门禁卡**不能用自己的类型名去模拟**。

**但这是策略，不是物理限制。** `ListenerST25R3916ForA::load_config()`
（`emulation_layer_a_ST25R3916.cpp:141`）往芯片里只写三样东西——UID 字节、ATQA、SAK——
外加 `REG_AUXILIARY_DEFINITION` 里的 4/7 字节 UID 标志位。**它从头到尾没读过 `picc.type`。**
type 只在后面三个地方被用到：`receive_callback` 的 READ / GET_VERSION / READ_SIG 分支，
以及 `totalSize()` / `unitSize()` 的边界检查。

所以：**用 Ultralight / NTAG215 当「载具类型」，再把 atqa/sak 用采集到的真值覆写回去，
放到空气中的就是这张卡真实的射频身份。** `PICC::emulate()` 会用 `emulation_settings[]`
（`nfca.cpp:197`）里的值填 atqa/sak——那是库认为该类型应该报什么，不是采集到什么，
所以必须在 `emulate()` 之后覆写。这就是 `src/nfc_emu.cpp` 里 `carrierFor()` 做的事，
屏幕上会明写 `CARRIER`，不假装类型是真的。

顺带一个正向发现：`emulation_settings[2]` 正好是 `{0x0004, 0x08}`，和这张门禁卡采集到的
atqa/sak **完全一致**。也就是说即使不覆写，Classic 1K 这一档的值也是对的——但覆写仍然要做，
因为其余类型对不上。

#### 边界二：模拟侧完全没有 crypto1

`crypto1` 在整个库里只出现在三个文件：`nfc/crypto/mifare_classic_crypto1.hpp`、
`unit/unit_ST25R3916.hpp`、`unit/unit_ST25R3916_nfca.cpp`——**全是读卡器侧**。
对 `emulation_layer_a.cpp` 和 `emulation_layer_a_ST25R3916.cpp` grep
`AUTH` / `0x60` / `0x61` / `crypto` / `Key`，**一个都没有**。库里也没有任何破解密钥的代码。

结论：**需要认证的门禁读不了也回放不了，这个库怎么改都不行。**

#### 为什么还是值得做：两种情况在板子上可区分

REQA、防冲突、SELECT 都是**芯片自己应答的**——`goto_off()` / `goto_idle()` 会清掉
`REG_NFCIP_1_PASSIVE_TARGET_DEFINITION` 的 `d_106_ac_a`，注释写的就是
"Enable auto response for NFC-A"。所以这三步**永远不会走到 `receive_callback()`**。

于是：

| 门禁读卡器的行为 | `receive_callback()` 收到的命令数 |
|---|---|
| 只认 UID，选完就开门 | **0 条** |
| 要认证 | `0x60` / `0x61`（AUTH key A / key B），后跟块号 |

**数一数收到了什么，就是整个实验。** 未认证的那一档下 `receive_callback` 一条命令都不会有，
所以「零命令 + 门开了」= 只认 UID；「出现 0x60」= 要认证，回放这条路直接断。

库对未知命令的处理是 `default: M5_LIB_LOGE("CMD:%02X %u", ...)` 然后返回 `State::Idle`
（`emulation_layer_a.cpp:220`）——结束会话。这是诚实的结果，不该编一个应答回去，
否则读卡器的行为反而没法解读了。

#### `nfc-emu` 构建：已烧录，验证到哪一步

新增 `src/nfc_emu.cpp`（`#ifdef RF_NFC_EMULATE`）+ `[env:nfc-emu]`。和 `nfc_spike.cpp` 分开两个文件，
理由和当初分开一样：读卡的 spike 已经验过了，不能被回放的改动碰坏。出货环境两个宏都不定义，
两个文件对它们都是空翻译单元。

```
pio run -e nfc-emu -t upload --upload-port COM10
```

`cfg.emulation` 必须在 `Units.begin()` **之前**设好——`configureEmulationMode()` 是从 `begin()` 里调的，
读卡模式和模拟模式是同一颗芯片的**互斥配置**，没有运行时开关。
所以 `nfc-emu` 是模拟器、`nfc-spike` 是读卡器，**切换靠重刷**。这条 Stage 2 必须处理成显式的模式切换，
不能做成一个 toggle。

几个刻意的设计：

| 设计点 | 取值 | 理由 |
|---|---|---|
| UID 从哪来 | 开机解析 `/nfc/capture.log`，**源码里一个真值都没有** | 仓库是 public，日志里是真实门禁凭据（见 §11） |
| 行格式兼容 | `id=`（多协议版）和 `uid=`（A-only 版）都认 | 日志跨了两次刷写累积，两代格式都在盘上 |
| 开机自动开始模拟 | rec[0]，不等按键 | 使用场景是单手拿着板子走到门前，「先找方向键再按回车再举起来」这套动作在读卡器前会手忙脚乱 |
| 串口/屏幕输出时机 | 只在 `state` 回到 `None`/`Off` 时 | 库自带的示例自己写着："printing stops the emulation answering for as long as it runs"。会话中打印会把一次成功的回放变成失败，还会赖错对象 |
| `M5_LOG_LEVEL` | **1（Error）**，不是 spike 的 5 | 模拟要在几百微秒内应答读卡器，日志越少越好。只留 Error 是为了保住 `receive_callback` 那条 `CMD:%02X`——它恰好就是本构建要找的 AUTH |
| `-DM5_UNIT_NFC_EMULATION_TRACE=1` | 打开库自带的状态 trace | 它只写内存，并且自己从 `goto_off()` dump（注释："The field is gone, so printing costs nothing here"），零成本拿到 OFF/IDLE/READY/ACTIVE/HALT 的完整路径。128 条 × 14 B |
| 命令 trace | 自己的 32 条 RAM 环 | 库的 trace 只记状态，不记命令字节；而命令字节才是判据 |
| 心跳 | 每 5 s 重播全部记录 | 和 spike 的 `dumpLog()` 同一个理由：USB-CDC 每次复位都重新枚举，开机那次一次性输出抓不到就永久丢了 |
| 界面 | 抄 `src/app.cpp` 的视觉语言，不自己发明 | 同一块 240×135 屏、同一副 60 键键盘。第二个固件长得不像产品，操作者就得重学一套「哪个键干什么」 |

**已验证（2026-11-10 上机）**：

```
[emu] ready=1 recs=8 fs=SPIFFS sel=0 emulating=1 state=O cmds=0 auth=0 heap=281760 min=249420
[emu] rec[0]* uid=<门禁卡> len=4 atqa=0004 sak=08 reads=2 type=MIFARE Classic 1K emulateAs=Ultralight CARRIER
[emu] rec[7]  uid=<NTAG215> len=7 atqa=0044 sak=00 reads=1 type=NTAG 215 emulateAs=NTAG215
[emu] on-air uid=<门禁卡> atqa=0004 sak=08 type=MIFARE Ultralight mem=1024
```

8 条记录全部从 SPIFFS 日志正确解析（`reads` 去重计数对得上：门禁卡 2 次，那条 `F` 被正确忽略）；
`emulating=1 state=O` = 状态机已上膛、在 `Off` 等读卡器，是 `start_emulation()` → `goto_off()` 之后
正确的待命态；`on-air` 那行证明**芯片里烧的是门禁卡真实的 UID/ATQA/SAK**，外面套的才是 Ultralight 载具。
flash **695,921 B**，RAM **25,964 B（7.9%）**，最小空闲堆 **249,420 B**（全屏 sprite 吃掉 64,800 B 之后）。
出货环境 `cardputer-adv` 重建仍是 **635,945 B / 26,264 B**，与基线逐字节一致。

#### 界面重做（2026-11-10，用户否决了第一版）

第一版是**一页塞完**：文字全 size 1，列表 + 详情块 + 状态行 + 提示行竖着摞，直接 `lcd.printf` 到面板，
键位只在一行 40 字符里写了三个。用户的原话是
「UI 太差了吧，根本不知道干怎么操作，这个设备有 60 个按键，你不给提示根本不知道从何入手，
页面内容也是随便一堆放一起，眼睛看花了也没弄懂」。三条都成立，重做如下：

| 改了什么 | 怎么改 |
|---|---|
| 一页塞完 | 拆成**三页**：`REPLAY`（当前在放什么、读卡器干了什么）／`CARDS`（选卡）／`KEYS`（键位表）。每页只回答一个问题 |
| 没有键位提示 | **每页页脚常驻**绿键帽 + 灰描述（`ENT re-arm` / `L cards` / `esc stop` / `H keys`），且 `H` 在任意页都能进 `KEYS` 页，`KEYS` 页列全部 6 个键位 |
| 状态读不懂 | 库的 `- O I R A H` 技术上正确、在门口毫无用处。换成 size 2 的**填色横幅**写整句：`WAITING FOR READER` / `READER HAS THE UID` / `READER SAID HALT` |
| 结论要自己推 | 加一条**彩色判定带**，直接把两个计数器翻译成人话：绿 `TOOK THE UID, ASKED NO KEY` / 红 `NEEDS A KEY - CANNOT CLONE` |
| 闪烁 | 全屏 `M5Canvas` sprite，整页画完一次推送 |
| 启动失败只有一声闷响 | `startEmulation()` 现在给四种不同的 `startFail` 文案上屏，不再三种拒绝共用一个 beep |

两个刻意保留的约束：

1. **`REPLAY` 页的 UP/DOWN 什么都不做。** 换「正在发射的卡」必须走 `CARDS` 页再按 ENTER，
   免得蹭到一下方向键就悄悄把门禁卡换了——在门口这是最坏的一种失误。
2. **横幅在会话进行中直接画到面板上，不走 sprite。** 整页 sprite 推送要 15–20 ms，
   足够让芯片错过读卡器的 frame-wait 窗口；库自带示例做的是同一件事（会话中重画一个 32×16 的状态框），
   这里只是把那个框放大成横幅，并且**只在状态跳变时**画。整页 `redraw()` 仍然只发生在 `None`/`Off` 和按键时。

排版是**算出来的，不是估的**：`setTextFont(1)` → `fontdata[1] == Font0` → `GLCDfont{font, font0_info, 6, 8, 7}`
（`lgfx_fonts.cpp:2441`），固定 6×8 点阵，所以每格宽度精确等于 6 px。据此：
`KEYS` 页 6 行键位 + 4 行说明用 10 px 行距，占 y=15..116，页脚横线在 118；
`CARDS` 页 `kRows` 从 6 提到 **7**（y=15..113），8 条记录里 7 条能一屏看完。
另外写了一个离屏脚本按同样坐标和 RGB565 值生成逐像素预览，并机器检查了 6 张图里
176 段文本的越界与同行重叠——0 处。**但预览只是排版检查，屏上观感仍要用户自己确认。**

**还没验证的、也是唯一要紧的那一步**：真实读卡器会不会认。这个只能人走到门前试。
建议的顺序是**先用手机试**——手机 NFC 靠近应该能让状态从 `O` 跳到 `I`/`R`/`A`，
在书桌前就能确认「芯片确实在发射、确实能被别的设备看到」，再走去门口，
免得把「载具方案根本不工作」和「门禁要认证」两件事混在一次走动里。

#### 对 Stage 2 的影响

「复制信号开门」是不是一个功能，**取决于门禁读卡器认不认证，而这一个问题只能上机回答**。
所以 Stage 2 的范围要等这次门口测试的结果再定：

- **零命令 + 门开了** → 回放是真实可用的功能，Stage 2 按「读卡 + 保存 + 回放」三件套做。
- **出现 `0x60`/`0x61`** → 回放这条路对本库是死的，Stage 2 缩成「读卡 / dump / 保存」，
  并且要在 UI 上**明确分三档说清能力边界**，不能笼统写「支持 MIFARE Classic」——
  这正是 Stage 2 六条硬要求里的第 3 条，现在有了具体的失败模式做例子。

### 12.5 分阶段计划

| 阶段 | 内容 | 能否只靠编译验证 |
|---|---|---|
| **0** | 依赖验证 spike | ✅ **已完成** |
| **1** | 抽出 app 抽象与注册表（纯重构，行为不变） | ⚠️ 不能——RF 采集/回放是否回归必须上机 |
| **2** | NFC app（读 UID / dump / 保存） | ⚠️ 部分——能不能读到卡必须上机 |
| **3** | 射频互斥框架（WiFi/BLE/CC1101 不并发） | ⚠️ 不能——ISR 抖动只能实测 |

Stage 1 要定下来并写进本文档的：什么是共享的（display / keyboard / 心跳 / 电池），
什么是每个 app 私有的（射频栈、store）。`src/app.cpp` 现在是 **1505 行单体**，首页五项是硬编码的。

**Stage 1 的风险要点**：§1 记着「本轮改动全部只编译验证过，没有上机测过」，§3 的按键卡死也还没定案。
**在未验证的基座上做大重构，等于把两个未知叠在一起。** 理想顺序是先上机确认 §10 第 9 条那两步
（压力测试 + 重采 4 条记录），再动 app.cpp。

### 12.6 未决

1. **本仓库要不要加 LICENSE。** 现在没有。一旦引入 LGPL 的 libssh，义务就来了。
   加了 license 也顺便解决 §11 里「public 但没声明授权」的悬空状态。
2. **NFC 采集记录存哪里**：`spiffs`（1.5 MB，未挂载，掉电可靠）还是 SD 卡（容量大，但要插卡）。
   倾向前者——SD 是可选件，NFC 记录不该依赖它。
3. **SSH 到底要哪个方向**：client（连出去）还是 server（连进来），交互式 shell 还是跑几条固定命令。
   答案决定是上 libssh（LGPL，~450 KB）还是 telnet（~10 KB，无法律负担）。
4. `app1` 做 recovery 镜像的话，怎么触发回滚、以及它和 §10 第 4 条那个 G0 下载模式怎么配合。


---

## 13. NFC 工具固件（2026-11-11 凌晨，`nfc-read`）

一个固件、一个首页菜单、四件事：

| 菜单项 | 作用 | 按键 |
|---|---|---|
| `NFC READ` | 扫卡并维护收藏夹 | `S` 扫描，`ENTER` 详情，`N` 改名，`D` 删除，`esc` 返回 |
| `NFC USE` | 把选中的卡放上射频 | `ENTER` 重新上膛，`;`/`.` 换卡，`esc` 停止并返回 |
| `BACKUP TO TF` | 收藏夹整份写到 SD | `ENTER` |
| `RESTORE FROM TF` | 从 SD 读回 | `ENTER` + YES/NO 确认（默认 NO） |

存储：SPIFFS `/nfc/cards.txt` 为主，SD `/nfc/cards.bak` 为备份槽（`src/nfc_store.h`）。
旧 `nfc-spike` 留下的 `/nfc/capture.log` 会在收藏夹不存在时**自动迁移**（已验证：8 条全部迁入）。

### 13.1 两个把这一轮拖了很久的 bug

**1. 键盘芯片 TCA8418 会被高频 I2C 打死。**
症状：按键几下后整机"冻住"，屏幕不动、按键无反应；串口心跳正常（主循环在跑），
但 `KEY_LCK_EC(0x03)=00`（芯片自己都没计数），`CFG(0x01)=01`（I2C 还能读）、INT 一直是高。
根因：`renderHeader()` 里**每帧**（30 Hz）调 `M5.Power.getBatteryLevel()`，加上 NFC 栈起来后
I2C 负载更高，键盘控制器的扫描被搞停。改法：**电量 3 秒缓存一次**（照 `src/app.cpp` 的做法）。
补充事实：这个状态**MCU 复位清不掉**（软复位也不够），只有**拔电**能恢复；
所以还加了每 8 秒自动重初始化键盘兜底。
> 排查口径：以后遇到"屏幕不动"，先看心跳里 `ev` 是否还在涨、`klck` 是否为 0，再判断是键盘还是渲染。

**2. `NFC USE` 看起来卡死，其实是整页没画。**
`emulating` 一置位，主循环就切到"只重绘 16px 状态条"的分支，而 USE 整页**从未被绘制过**，
屏幕上留的还是首页 → 看起来像冻住。**关键线索：按 `esc` 能退出**，说明程序没死。
改法：进 USE 时先 `render()` 一次、`startEmulation()` 后再 `render()` 一次；
`emu.auth` 变化时也要整页重绘（判定带依赖它）。
> 教训：只重绘局部的优化，必须保证"整页至少画过一次"。

### 13.2 已验证（2026-11-11 01:4x，上机）

```
[read] addSPI=1 units.begin=1
[read] store=SPIFFS cards=8 err=-
[read] kbd cfg=01 int=00
[read] t=35049 keys=5 homeSel=1 ev=10 int=0 kcfg=01 klck=01     <- 键盘正常在跑
[read] key 0x2 scr=0 homeSel=1 sel=0                            <- `.` 下移
[read] ready=1 cards=8/24 ... scr=4 emulating=1 state=...       <- USE 页
```

- 四项菜单上下可选、ENTER 可进 ✅
- 读取页：扫描入库、列表显示**完整 UID** + 类型缩写 + 读取次数 ✅
- 详情页：`NAME / UID / TYPE / ATQA / READ` 分行带标签 ✅
- `NFC USE` 页面正常显示：卡名/UID、状态横幅、判定带、底部键位 ✅
- `BACKUP/RESTORE` 目前报 `NO SD CARD`（手头没插 TF 卡），代码路径未实测 ⚠️

### 13.3 还没验证的

1. **USE 真的能被读卡器看到吗**：拿手机靠近，看状态横幅是否从 `WAITING FOR READER` 变到
   `READER HAS THE UID`；门口读卡器是否会 `NEEDS A KEY - CANNOT REPLAY`（0x60/0x61）。
2. **TF 备份/恢复**：插卡后跑一遍 `BACKUP TO TF` → 拔卡 → `RESTORE FROM TF`。
3. 键盘自愈（每 8 秒重初始化）是否会丢按键，需要长时间按键测试。

### 13.4 环境

```
pio run -e nfc-read -t upload --upload-port COM10
```

Flash 约 735 KB / RAM 27 KB；`nfc-spike`（多协议探针）和 `nfc-emu`（纯模拟，旧）仍在，
后者已被 `nfc-read` 的 `NFC USE` 取代，可以把 `nfc-emu` 退役。
### 13.5 NFC 模拟（NFC USE）实测结论（2026-11-11）

**读取侧完全可用，模拟侧在这块硬件 + 这个库上做不到开门。**

实测过程（全部上机）：

| 尝试 | 结果 |
|---|---|
| 运行时 `configureEmulationMode()` 切换 | 状态机显示已上膛（`state=Off`），但**射频上不应答** |
| 改为重启切换模式（`cfg.emulation` 在 `Units.begin()` 前设好） | 真的在发射了：`emulate uid=2BF6276F atqa=0004 sak=08 carrier=Ultralight` |
| 手机当读卡器贴上去 | 手机报「读卡失败，本卡片可能不是NFC卡片」——**能听到应答，但过不了类型判定** |
| 门口读卡器 | 毫无反应 |
| 身份呈现改为「一致的 Ultralight」（`T` 键切换） | 仍然不行 |
| 会话期间静默（不打印，避免打断应答） | 仍然不行 |

**这张门禁卡的性质**：手机 App 复制同一张卡后**能开门** → 门禁只认 UID，不做 crypto1 认证。
也就是说理论上模拟可行，卡在**模拟实现的射频层细节**上。

**为什么难**：M5Unit-NFC 的模拟层只支持 NTAG/Ultralight 载具，`load_config()` 只写
UID/ATQA/SAK，没有 crypto1，也没有运行时模式切换（这就是为什么 `nfc-emu` 原本是独立固件）。
手机能听到说明帧在发，但读卡器侧还要过 ATQA/SAK/SELECT 的一致性判定，这一步这个库没打磨过。

**如果还要做「复制卡开门」，现实的三条路**：

1. **换 PN532**（约 ¥30，I2C/SPI）：它有专门的 target 模式，社区里用来做 UID 复制的案例多，
   成功率高得多；代价是要重新接一个模块、换库。
2. **ChameleonUltra / Flipper** 这类专用设备，直接就是干这个的。
3. **放弃模拟**：这块 CAP.CC1101 就做「读取 + 收藏 + 备份」，模拟功能从菜单里去掉，
   避免给人「按了没反应」的错觉。

**保留在代码里的东西**：`NFC USE` 页、身份切换、重启切换模式、静默应答——都是对的，
换到能用的射频层（PN532）时这些 UI 和状态机可以直接搬。
### 13.6 ✅ 复制卡开门成功（2026-11-12 00:0x，上机验证）

**门开了。** 下面是唯一验证通过的组合，改任何一项都要重新验证。

| 项 | 取值 |
|---|---|
| 载具类型 | `Type::MIFARE_Ultralight`（4 字节 UID）或 `NTAG_215`（7 字节） |
| 内存镜像 | **Ultralight 页布局**：`mem[0..3]=UID`、`mem[4]=BCC(UID)`、后面留 0（7 字节 UID 用官方的 `embed_uid()`） |
| ATQA / SAK | **用采集到的真值覆写**（本例 MIFARE Classic 1K：`ATQA=0004`、`SAK=08`），不要用载具自己的 |
| 每轮必须调 | `Units.update()` **和** `emu.update()`（官方例程的 `loop()` 就是这样） |
| 串口打印 | 会话期间**绝对不能打印**（库的示例原话：printing stops the emulation answering） |
| 模式配置 | `cfg.emulation = true` 必须在 `Units.begin()` **之前**（所以靠重启切换模式，NVS 存一个字节） |

关键排错过程（都可以复现）：

| 版本 | 手机 | 门口读卡器 |
|---|---|---|
| 运行时 `configureEmulationMode()` 切换 | 看不到 | 无反应 |
| 重启切换模式（`cfg.emulation` 正确） | 「已存在这张卡」（UID 读对） | 状态机 `Off→Idle→Ready`，**不进阶到 Active**，门不开 |
| + ATQA/SAK 改成 Classic 1K 真值 | — | **门开了** |

**两个此前误判的点**：

1. **UID 搞错了**：列表里第一张卡是 `2BF6276F`，而门卡是 `3C02B611`。`NFC USE` 默认选第一张，
   于是门口测的全是错的卡——白白浪费了好几轮。「门口没反应」的真正原因有一半是这个。
2. **ATQA/SAK 必须与真卡一致**：读卡器在 SELECT 之后会按 ATQA/SAK 决定这张卡该怎么处理。
   只把 UID 放上去、类型报成载具默认的 Ultralight，读卡器就停在 Ready 不执行。

**状态机颜色**（官方例程那块色块，排错时很好用）：
红=Off、蓝=Idle、黄=Ready、绿=Active、品红=Halt。门口的读卡器如果能推到 Ready 说明 UID 已经读到了。
---

## 14. retry/：干净的 NFC 工具（2026-11-12 00:35，✅ 上机全通）

`Y:\2026\cardputer\retry\` 是一个**独立工程**，按官方例程重写，三个功能都验证通过：

| 功能 | 状态 |
|---|---|
| 读卡（`READ CARDS`，S 扫描，自动入库） | ✅ |
| 存储（SPIFFS `/nfc/retry.txt`，自动从主固件 `cards.txt` 导入一次） | ✅ 导入 2 条 |
| 模拟（`CARD LIST` → ENTER → 重启进入模拟） | ✅ **门口开门** |

```
pio run -d Y:\2026\cardputer\retry -e retry-emu -t upload --upload-port COM10
```

### 14.1 为什么它能成而 `nfc-read` 不能

`nfc-read` 是在读卡器固件（含键盘、SPIFFS、SD、64 KB sprite、心跳、状态机）上"加"了模拟，
而模拟对时序极其敏感。retry 把模拟路径剥到和官方例程一样：

| 项 | retry（能开） | nfc-read（不能开） |
|---|---|---|
| 模拟启动 | `cfg.emulation` 在 `Units.begin()` 前，靠重启切换 | 同样思路，但同进程里还有一大堆东西 |
| 会话中键盘 | **只在 state=Off/None 时**每 400 ms 轮询一次 | 每 250 ms 轮询（会话中也在） |
| 会话中重绘 | 只画 16 px 状态条，直接写面板 | 曾经整屏 pushSprite（64 KB / 15 ms） |
| SD / SPIFFS | 模拟模式完全不挂载 SD | 曾在上膛前 `SD.begin()`/`SD.end()` |
| 内存镜像 | 官方例程那份 64 B Ultralight 镜像 + 嵌入 UID | 自己拼的布局 |
| 界面 | 直接画，无 sprite | 64 KB canvas |

### 14.2 成功配方（最终版）

1. `cfg.emulation = true; cfg.mode = NFC::A;` **在 `Units.begin()` 之前**，切换模式靠 NVS 标记 + 重启。
2. `addSPI(units, unit, 10 MHz, 1)` → `units.begin()`。
3. `picc.emulate(Type::MIFARE_Ultralight, uid, 4)`（7 字节 UID 用 `NTAG_215`）。
4. **把采集到的 ATQA/SAK 写回** `picc.atqa/picc.sak`（本例 `0004`/`08`）。
5. 把 UID 嵌进官方那份 Ultralight 内存镜像（4 字节：`mem[0..3]=uid; mem[4]=BCC`）。
6. `emu.begin(picc, mem, sizeof(mem))`（64 B，别用 1 KB）。
7. `loop()` 里 **`units.update()` 再 `emu.update()`**，中间不插入任何慢操作。
8. 会话中**绝不打印**（库的示例原话），状态条直接画面板。

### 14.3 两个反复踩到的坑（与模拟无关，但会伪装成模拟故障）

1. **键盘芯片 TCA8418 被高频 I2C 打死**：状态"屏幕不动、按键全无响应"，`ev` 不涨、
   `KEY_LCK_EC=0`，I2C 仍可读。根因是每帧调 `M5.Power.getBatteryLevel()`（30 Hz I2C）。
   **MCU 复位救不回来，只能拔电。** 修法：电量 3 秒缓存；模拟模式下几乎不碰 I2C。
2. **UID 选错卡**：`CARD LIST` 第一张是 `2BF6276F`，而门卡是 `3C02B611`。
   之前多轮"门口没反应"其实是模拟错了卡。

### 14.4 待办

- retry 还没有 TF 备份/恢复（主固件有，可移植）。
- rename（`N` 键）已实现未测。
- 主固件的 `nfc-read` 可以废弃，或者只保留其"读取+收藏"部分。