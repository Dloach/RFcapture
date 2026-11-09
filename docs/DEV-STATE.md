# 开发状态交接（最后更新 2026-11-09）

换机器继续开发前记录。**代码在 NAS 上，不在任何 Qoder 会话目录里**：
macOS 是 `/Volumes/Project/2026/cardputer`，Windows 上映射成 `Y:\2026\cardputer`——同一个目录，别另存一份。
上一轮会话的串口日志留在原 Mac 的 `/tmp/cap.log`（不随项目走），需要时用 `tools/cap-serial.py` 重抓。

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

---

## 1. 当前固件：功能完整，已上机验证

最近一次 `pio run`：**Flash 635945 B / 19.0%，RAM 26264 B / 8.0%**（`diag-off` 环境：632961 B / 26256 B）。
`platform = espressif32@6.13.0` 已锁死（原因见 §5 硬约束）。

| 功能 | 状态 |
|---|---|
| 315 / 433 / 868 MHz OOK 采集 | ✅ 可用，但要注意波形质量（见 §8.3） |
| 采集页：选中 / 保存 / 直接发送 / 换频段 / 删除 / 清空 | ✅ |
| 采集页 `RAW` 行（解不出码但保留波形） | ✅ 需通过质量门限才入库（见 §9.1） |
| 每条记录的 `q00`–`q99` 波形质量分 | ✅ 新增，判据见 §8.5 |
| 永久保存（NVS，8 条） | ✅ 现有 4 条，**都是修 `onEdge()` 之前采的，建议重采**（见 §8.3） |
| 发送：**RMT 硬件时序 + 零间隙连发 + 700ms 定长** | ✅ rollUP/rollDOWN/maindoor 实测都能开 |
| CC1101 没接/没识别到时的降级 | ✅ 新增，标题栏显示 `NO RADIO`，RF 三项菜单拒绝进入（TF 备份/恢复仍可用） |
| 首页 5 项：CAPTURE / SEND / SCAN RF / BACKUP TF / RESTORE TF | ✅ |
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

三点要注意：

1. **git 历史也是敏感面**。真值一旦提交过，删文件没用。本次是在 `.git` 从未推出去之前重建的历史
   （重新 `git init`、按模块重放提交、丢掉那条加原始 log 的提交），所以现在整个历史里都不含真值。
   **以后如果不小心把真值提交了，同样要重写历史，不能只删文件。**
2. **设备名保留了**（`maindoor` / `rollUP` / `rollSTOP` / `rollDOWN`）——门的种类在 README 里本来就写了，
   名字本身不构成额外泄露，留着叙述才读得通。
3. `RF_DIAG=1`（默认）开机就会把 NVS 里所有脉宽 dump 到串口。任何人拿到板子接串口就能读出来——
   这是设计如此（排障必需），但意味着**别把这个 dump 贴到公开 issue / 论坛 / pastebin 上**。
