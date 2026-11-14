# WiFi Settings + SSH Client 开发计划

> 基线：`v1.2.0`（键盘功能）+ `1512e84`（BLE 设备列表）。
> 目标平台：M5Stack Cardputer ADV，ESP32-S3，8 MB Flash，**无 PSRAM**。
> 固件约束：`espressif32@6.13.0` / Arduino core **2.0.17** / ESP-IDF **4.4**，
> USB mode 0（TinyUSB CDC + HID），每个功能一次重启。
>
> 本文是可执行计划，不是最终代码。所有 API 名称按当前锁定的 core 2.0.17 /
> IDF 4.4 写；升级 core 前必须重新验证本文标注的兼容点。

---

## 0. 结论先行

1. **先做 launcher 滚动菜单**，再加 `SETTINGS`；否则第 5 个功能开始就放不下。
2. WiFi 不引入第三方库：用 Arduino-ESP32 自带 `WiFi.h` / `WiFiScan`，
   凭据用 `Preferences`（NVS）保存，自己画扫描/密码/连接 UI。
3. SSH 客户端首选 **LibSSH-ESP32**：
   - 基于 libssh，README 明确列出 **Arduino core 2.0.17** 与 **ESP32-S3**；
   - 提供 `libssh_begin()`、SSH client、known_hosts、SCP 示例；
   - 许可证 **LGPL-2.1**，静态链接发布需要满足可重链接义务；
     **已决定接受**（2026-11-13），按 LGPL 合规路径推进。
4. **M5-SSH-Wireguard** 是 Cardputer ADV 上最完整的现成 SSH 客户端（MIT），
   有 ANSI 终端、WiFi manager、profile、Ed25519 key，但它要求
   **Arduino core ≥ 3.2.6 / ESP-IDF 5.4**，与当前 pin 冲突；只借架构和终端状态机，
   不直接替换平台。
5. SSH 必须跑在**独立 FreeRTOS task**（参考 LibSSH 示例 51,200 B stack），
   不能塞进 Arduino `loop()` 的 8 KB 栈；UI task 与 SSH task 通过队列/双缓冲通信。
6. WiFi 与 BLE 不同时初始化；SSH app 不初始化 BLE，Settings app 不初始化 BLE。
   RF/NFC/键盘/SSH 仍然保持“一个功能一次重启”的隔离模型。

---

## 1. 当前基线与硬约束

| 项目 | 现状 | 对 WiFi/SSH 的影响 |
| --- | --- | --- |
| MCU | ESP32-S3，240 MHz，320 KB SRAM，无 PSRAM | libssh + WiFi 内存要实测；不能开大缓冲 |
| Flash | 8 MB，`default_8MB.csv` | SSH 代码/凭据/known_hosts 放 SPIFFS；不依赖 TF |
| Arduino core | 2.0.17 / IDF 4.4，固定 | LibSSH-ESP32 可用；M5-SSH-Wireguard 不可直接用 |
| USB | TinyUSB CDC + HID（mode 0） | 串口/COM 会变；WiFi 不冲突，但烧录仍用 1200bps touch/G0 |
| 启动模型 | launcher 写 NVS `appboot`，`ESP.restart()` | Settings/SSH 各加一个 App 数值；不要在运行时反复 init/deinit WiFi |
| 菜单 | 4 项固定布局，右侧 3×3 九宫格 | 必须先做滚动；否则 SETTINGS/SSH 放不下 |
| BLE | Bluedroid，键盘 app 内初始化 | SSH/Settings **不要**调用 BLE；避免 BT + WiFi 同时占内存 |
| 存储 | NVS 用于 appboot/RF/NFC；SPIFFS 用于 quickbind/NFC；TF 用于备份 | WiFi 凭据放 NVS；SSH profile/key/known_hosts 放 SPIFFS |
| 显示 | 240×135，M5GFX | WiFi 列表 5 行；终端 40×13（font 1） |
| 键盘 | `M5Cardputer.Keyboard.keysState()` / `keyList()` | 文本输入用 `ks.word`，不要复用菜单的 `; . , /` 动作 |

**不要改的数值**：`kAppBackup=3`、`kAppKbd=4` 已经写进 NVS appboot；新增
`kAppSettings=5`、`kAppSsh=6`，不要重排旧值。

---

## 2. 资料与开源项目调研

### 2.1 M5Stack / ESP32 官方资料

| 资料 | 用途 | 结论 |
| --- | --- | --- |
| [M5Stack Cardputer ADV 文档](https://docs.m5stack.com/en/core/Cardputer-Adv) | 板卡、屏幕、键盘、外设 | 确认 2.4 GHz WiFi、240×135、键盘矩阵；以官方文档为准 |
| [M5Cardputer 库](https://github.com/m5stack/M5Cardputer) | `M5Cardputer.Keyboard`、`M5Cardputer.Display` | 当前工程已用 `@^1.1.1`，不要为 WiFi 升级它 |
| [Arduino-ESP32 WiFi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html) | STA、扫描、连接、WiFiMulti | 直接用 `WiFi.h`；不需要额外 WiFi 库 |
| [WiFiScan.h（core 2.x）](https://github.com/espressif/arduino-esp32/blob/2.0.17/libraries/WiFi/src/WiFiScan.h) | `scanNetworks` / `scanComplete` / `SSID` / `RSSI` / `encryptionType` | 用 async 扫描，避免 UI 卡死 |
| [ESP-IDF 4.4 NVS](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-reference/storage/nvs_flash.html) | NVS 键值、掉电一致性 | 适合小凭据；不适合大 blob；默认不加密 |

**WiFi 关键事实**：

- ESP32-S3 只支持 **2.4 GHz**；5 GHz SSID 不会出现在扫描结果里。
- 支持 WPA2-PSK、WPA3-SAE（取决于 AP/IDF 配置）；企业级 802.1X 不支持，UI 明确报错。
- `WiFi.scanNetworks(async=true)` 后轮询 `WiFi.scanComplete()`：
  - `>= 0`：结果数量；
  - `WIFI_SCAN_RUNNING`（-2）：扫描中；
  - `WIFI_SCAN_FAILED`（-1）：失败。
- 连接状态用 `WiFi.status()`：
  - `WL_CONNECTED`、`WL_NO_SSID_AVAIL`、`WL_CONNECT_FAILED`、
    `WL_CONNECTION_LOST`、`WL_DISCONNECTED`。
- `WiFi.persistent(false)` + 自己的 NVS 存储，避免 SDK 再存一份、两套状态打架。
- `WiFi.setAutoReconnect(true)` 只负责链路断线重连，不负责“选哪个 saved network”。

### 2.2 SSH 库/项目对比

| 项目 | 技术栈 | 许可证 | 适配当前固件 | 评价 |
| --- | --- | --- | --- | --- |
| [ewpa/LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32) | libssh 0.11.5 移植到 Arduino/ESP32 | **LGPL-2.1** | **是**，README 明确 core 2.0.17、ESP32-S3 | **首选**；成熟，client/server/SCP/known_hosts 示例 |
| [bpivk/M5-SSH-Wireguard](https://github.com/bpivk/M5-SSH-Wireguard) | Cardputer ADV 完整 SSH + WireGuard + ANSI 终端 | MIT（README） | 否，要求 core ≥3.2.6 / IDF 5.4 | **最佳交互/终端参考**；只移植纯 C++ 终端状态机，不搬平台层 |
| [aat440hz/SSHClient-M5Cardputer](https://github.com/aat440hz/SSHClient-M5Cardputer) | M5Cardputer + LibSSH 最小示例 | MIT | 基本是 core 2.x 写法 | 适合做第一个“能连上”的 spike |
| [J-Rios/Arduino-esp32sshclient](https://github.com/J-Rios/Arduino-esp32sshclient) | libssh2 封装 | 仓库未明确/需核对 | 作者自述 proof of concept、不适合稳定系统 | 不选；只作为 libssh2 API 参考 |
| [libssh2](https://www.libssh2.org/) | C SSH 库，BSD-3-Clause | BSD-3 | 需自己写 ESP32/Arduino 封装 | 若必须避免 LGPL，可作为备选；工作量更大 |
| [WiFiManager](https://github.com/tzapu/WiFiManager) |  captive portal 配置 | MIT | 可用但不适合本 UI | 只参考“扫描/保存/重连”流程，不引入 |

**LibSSH-ESP32 的关键细节（来自其 README/示例）**：

- 入口：`#include "libssh_esp32.h"`，启动时调用 `libssh_begin()`。
- 官方 `exec` 示例把 SSH 放在 `xTaskCreatePinnedToCore(..., configSTACK = 51200, ...)`，
  不是 Arduino loop 栈。
- 示例包含 `verify_knownhost()`：
  `ssh_get_server_publickey` → `ssh_get_publickey_hash(SHA256)` →
  `ssh_session_is_known_server`，未知主机要显式确认。
- 可在编译期调整 `CHANNEL_MAX_PACKET`、`WINDOW_DEFAULT`、`BUFFER_SIZE_MAX`；
  默认对无 PSRAM 的 S3 要实测。

**M5-SSH-Wireguard 的可借用部分（MIT）**：

- WiFi manager：扫描、连接、保存、开机自动连接；
- SSH profile 文件格式与多 profile 菜单；
- ANSI/VT100 终端：40×14 / 20×7 字体、颜色、光标、滚动区、备用屏；
- 键位：`Fn+; . , /` 方向键、`Ctrl+字母`、`Fn+Q` 退出、G0 退出；
- Ed25519 key 文件布局。
- **不可直接借用**：它的 board manager/IDF 5.4 平台层、WireGuard 依赖、SD 卡路径假设。

### 2.3 许可证结论

当前仓库根目录**没有 LICENSE 文件**。在引入 LGPL 的 LibSSH-ESP32 前必须：

1. 给项目选一个许可证（建议 MIT，和现有依赖生态一致）；
2. 新增 `docs/THIRD-PARTY.md`，列出 LibSSH-ESP32 / libssh（LGPL-2.1）、
   M5Cardputer、M5Unit-NFC 等；
3. 发布二进制时提供可重链接所需的目标文件或完整构建产物说明；
4. 如果不想承担 LGPL 义务，改用 **libssh2（BSD-3）**，但要自己写封装，
   SSH 阶段预留至少多 3–5 天。

> **决策（2026-11-13）：接受 LGPL-2.1，使用 LibSSH-ESP32。**
> 发布合规包时至少包含：`LICENSE`（项目自身）、`docs/THIRD-PARTY.md`、
> LibSSH/libssh 的 LGPL 文本、可重链接的目标文件/源码与构建说明。

> 个人自用不分发时风险较低；一旦把 `.bin` 发给别人，LGPL 的“可重链接”要求就生效。

### 2.4 LGPL 速查（给后续开发/发布用）

**LGPL = GNU Lesser General Public License**，是给“库”用的弱 copyleft 许可证。
它允许你的程序（可以是 MIT/闭源）链接 LGPL 库，而不要求你把整个程序开源；
但分发时要满足：

1. 附上 LGPL 许可证文本和版权声明（`docs/THIRD-PARTY.md`）；
2. 如果改了 LibSSH/libssh 本身，改动的库代码仍按 LGPL 发布；
3. 接收者必须能**替换/重新链接**这个 LGPL 库。静态链接进 `.bin` 时，
   只发一个固件通常不够；需要同时提供可重链接的目标文件（`.o`/`.a`）或
   完整源码 + 可复现构建脚本，让用户能换库后重新链接。

对本项目的实际含义：

- 自己写 `ssh_app`、`terminal` 仍可按项目许可证（建议 MIT）发布；
- **LibSSH-ESP32 / libssh 仍是 LGPL**，不能因为项目是 MIT 就当成 MIT；
- 只在本地自用、不把固件给别人：一般不触发分发义务；
- 把 `.bin` 发到群里/网上/客户：需要做上面的合规包；
- 不想处理这些：用 **libssh2（BSD-3）**，但要多写封装，SSH 阶段多 3–5 天。

> 以上是工程上的通俗说明，不构成法律意见；真对外发布前按项目政策确认。

---

## 3. 目标交互与页面结构

### 3.1 launcher 滚动菜单

菜单项顺序（最终）：

```text
RF TOOLS
NFC TOOLS
KEYBOARD
BACKUP
SETTINGS
SSH            <- 后续
...            <- 后续功能继续追加
```

左侧可视 **4 行**（`y = 22 + row*22`），右侧九宫格不动：

```text
┌──────────────────────────────┐
│ CARDPUTER            87%    │  header
│ RF TOOLS        │ 1 2 3     │
│ NFC TOOLS       │ 4 5 6     │
│ KEYBOARD        │ 7 8 9     │
│ BACKUP          │           │
│ SETTINGS        │           │
│ SSH             │           │
│ 2/2  ^ v        │           │  scroll indicator / status
└──────────────────────────────┘
```

行为：

- `;` / `.`（或 `k` / `j`）上下移动，越界时滚动窗口；
- `ENTER` 进入；
- 滚动窗口保持选中项可见：
  ```cpp
  if (sel < menuTop) menuTop = sel;
  if (sel >= menuTop + kMenuVisible) menuTop = sel - kMenuVisible + 1;
  ```
- 右侧九宫格仍按 `pageFromModifiers()` 切换 MAIN/FN/AA/OPT；
- 选中项高亮条只画左侧 112 px，不压到分隔线；
- 在 `y=108` 的状态行显示 `sel+1/total` 或滚动提示，不新增 footer 行。

启动进度条 `kBootName[]` 增加 `SETTINGS`、`SSH`；`kMenuApp[]` 做 `sel -> App` 映射。

### 3.2 SETTINGS 页面

```text
SETTINGS
> WIFI SETTINGS
  SYSTEM          <- 后续
  ABOUT           <- 后续
```

第一期只做 `WIFI SETTINGS`。`SETTINGS` 本身用和 launcher 相同的滚动列表组件，
避免以后每加一项都改布局。

### 3.3 WIFI SETTINGS

主页面：

```text
WIFI
STATUS: CONNECTED
SSID: Home-2.4G
IP:   192.168.1.42
> SCAN NETWORKS
  SAVED NETWORKS
  FORGET CURRENT
```

按键：

- `S` 扫描；
- `ENTER` 进入选中项；
- `D` 忘记当前网络（确认一次）；
- `esc` 返回 SETTINGS。

扫描页：

```text
WIFI SCAN                    [SCANNING]
> Home-2.4G        -42  [*]
  Office           -61  [*]
  Guest            -70
  ...
S rescan   ENT select   esc back
```

- 最多显示 5 行，超过滚动；
- SSID 最多 32 字节，显示截断用 `...`；
- 右侧 `[*]` 表示加密，`[ ]` 表示开放；
- 信号强度用 `-dBm` 数字，不依赖图标字体；
- 同 SSID 多 BSSID 去重，保留最强 RSSI；隐藏 SSID 不显示（后续可手动添加）。

密码页：

```text
WIFI PASSWORD
SSID: Home-2.4G
[**************____]
Aa = shift   BKSP = delete
ENT connect   esc cancel
```

- 密码不显示明文，只显示 `*` 和光标；
- 长度 8–63（WPA2/WPA3）；开放网络跳过此页；
- 文本输入用 `M5Cardputer.Keyboard.keysState().word`，不要走菜单的 `; . , /`
  方向键映射；
- `Aa` 产生大写；Cardputer 键帽上的符号按实际字符输入；
- 不支持中文 SSID 输入（可扫描选择，键盘只输 ASCII 密码）。

连接页：

```text
CONNECTING
Home-2.4G
................
```

- 15 s 超时；
- 失败原因：`NO AP` / `WRONG PASSWORD` / `TIMEOUT` / `AUTH FAILED`；
- 成功后显示 IP 1 s，再回 WiFi 主页面；
- 只有成功才写入 NVS；
- 连接中 `esc` 取消（`WiFi.disconnect()`）。

已保存页：

```text
SAVED NETWORKS
> Home-2.4G
  Office
ENT connect   D forget   esc back
```

自动连接策略：

1. 开机读 NVS；
2. 优先 `lastSsid` 对应的记录；
3. 若失败，按保存顺序尝试其余记录（最多 3 个，串行，每个 10 s）；
4. 全部失败显示 `NO WIFI`，不阻塞 UI；
5. 连接成功后更新 `lastSsid` 和 `lastOkMs`。

### 3.4 SSH 页面（后续）

```text
SSH
> home-server
  lab
  + NEW PROFILE
```

连接后：

```text
┌──────────────────────────────┐
│ home-server       CONNECTED │
│ user@host:~$ ls             │
│ ...                         │
│                             │
│ Fn+BKSP back   Fn+Q close   │
└──────────────────────────────┘
```

- 终端 **40 列 × 13 行**（font 1，6×8；header 13，footer 118，正文 105）；
- 底部一行状态/提示，不用大 footer；
- `Fn+BKSP` 先断开会话再回 SSH profile 列表；
- `Fn+Q` 只断开会话、留在列表；
- `esc` 在终端里发送 `ESC`，不作为退出（避免和 vim 冲突）。

---

## 4. 数据与存储设计

### 4.1 WiFi 凭据（NVS）

命名空间 `wifi`，最多 5 个网络：

| key | 类型 | 说明 |
| --- | --- | --- |
| `ver` | u8 | 结构版本，当前 1 |
| `count` | u8 | 0..5 |
| `last` | u8 | 上次成功索引 |
| `ssid0..4` | string | SSID，≤32 字节 |
| `pass0..4` | string | 密码，≤63 字节 |
| `time0..4` | u32 | 上次成功时间（可选） |

写入规则：

- 扫描/失败不写；
- 连接成功才写；
- 相同 SSID 覆盖旧记录并移到 `last`；
- 超过 5 个时删最久未用；
- 所有写入在一个 `Preferences` 事务里完成（`begin`/`put`/`end`）。

> **安全说明**：默认 NVS 不加密，WiFi 密码以明文保存在 flash。设备丢失/被拆机
> 即可读出。第一期接受这个威胁模型；不要把设备借给不可信的人。
> 若以后要加密：需要改分区表增加 `nvs_keys` 分区并启用 NVS encryption，
> 这会改变现有 NVS 布局，必须单独做迁移，不能和本次功能混在一起。

### 4.2 SSH profile / key / known_hosts（SPIFFS）

```text
/spiffs/ssh/profiles.txt     # 每行一个 profile
/spiffs/ssh/known_hosts      # libssh known_hosts 格式
/spiffs/ssh/id_ed25519       # 可选私钥
/spiffs/ssh/id_ed25519.pub
```

`profiles.txt` 格式（版本 1，一行一个，字段用 `|`）：

```text
v1|home-server|192.168.1.10|22|alice|password|1
v1|lab|lab.example.com|22|root|key|1
```

字段：`version|name|host|port|user|auth|savePassword`。

- 第一版最多 4 个 profile；
- **已决定：`savePassword` 默认 1**，连接成功后把密码写入 NVS 命名空间
  `ssh` 的 `pass0..pass3`（不写进 `profiles.txt`，避免 `|`/换行转义问题）；
  用户可在 profile 编辑页清除，清除后每次连接提示；
- 密码只保存在本机 NVS，明文；设备丢失=凭据泄露，和 WiFi 密码同一威胁模型；
- 私钥只支持 Ed25519（libssh 0.11.5 / OpenSSH 新格式），第一版不做 RSA；
- `known_hosts` 走 libssh 的 known_hosts API，不自己实现哈希。

> SSH 密码/私钥同样是明文存储；和 WiFi 密码同一威胁模型。

SSH 密码单独放 NVS 命名空间 `ssh`：

| key | 类型 | 说明 |
| --- | --- | --- |
| `ver` | u8 | 1 |
| `pass0..3` | string | 对应 profile 的密码；空=未保存 |
| `last` | u8 | 最近使用的 profile |

- 连接成功且 `savePassword=1` 才写；
- profile 删除时同步删除对应 `passN`；
- 不写进 `profiles.txt`，避免分隔符/换行/长度问题；
- 用户清除密码时写空字符串，不删除整个 NVS 命名空间。

---

## 5. 代码架构

### 5.1 新增/修改文件

```text
src/
├── launcher.h/.cpp          # 滚动菜单、SETTINGS/SSH app 值、boot 名称
├── main.cpp                 # Running::kSettings / kSsh 分发
├── wifi_store.h/.cpp        # NVS 凭据、扫描结果、自动连接
├── settings_app.h/.cpp      # SETTINGS 页面 + WiFi UI 状态机
├── ssh_app.h/.cpp           # SSH profile 列表、连接状态机、任务
├── ssh_terminal.h/.cpp      # ANSI/VT100 解析 + 40×13 缓冲
└── ssh_store.h/.cpp         # profiles/known_hosts/key 路径（可并入 ssh_app）
tests/host/
├── test_wifi_store.cpp      # 序列化、去重、优先级
└── test_terminal.cpp        # ANSI 状态机
LICENSE
docs/THIRD-PARTY.md
```

### 5.2 App 数值

```cpp
enum App : uint8_t {
    kAppMenu     = 0,
    kAppRf       = 1,
    kAppNfc      = 2,
    kAppBackup   = 3,
    kAppKbd      = 4,
    kAppSettings = 5,
    kAppSsh      = 6,
};
```

`currentApp()` 校验上限改为 `kAppSsh`；`kBootName[]` 同步加两项；
`main.cpp` 增加 `Running::kSettings/kSsh` 与 dispatch。

### 5.3 WiFi 状态机

```cpp
enum class WifiState : uint8_t {
    kIdle,
    kScanning,
    kConnecting,
    kConnected,
    kFailed,
};
```

扫描（非阻塞）：

```cpp
if (WiFi.scanComplete() == WIFI_SCAN_FAILED) { /* show error */ }
if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) { /* spinner */ }
else if (WiFi.scanComplete() >= 0) {
    // collect WiFi.SSID(i), WiFi.RSSI(i), WiFi.encryptionType(i)
    // dedupe by SSID, keep strongest; sort by RSSI desc
    WiFi.scanDelete();
}
// start: WiFi.scanNetworks(true, false, false, 300);
```

连接（非阻塞）：

```cpp
WiFi.mode(WIFI_STA);
WiFi.persistent(false);
WiFi.setAutoReconnect(true);
WiFi.begin(ssid, pass);
// loop:
switch (WiFi.status()) {
  case WL_CONNECTED: saveOnSuccess(); state = kConnected; break;
  case WL_NO_SSID_AVAIL: fail("NO AP"); break;
  case WL_CONNECT_FAILED: fail("WRONG PASSWORD"); break;
  default: if (elapsed > 15000) fail("TIMEOUT"); break;
}
```

注意：

- 不要在 `loop()` 里 `while (!WiFi.isConnected()) delay()`；会卡 UI/看门狗。
- 连接成功后 `WiFi.localIP()` 存到 UI 状态，不每次重画都查。
- `WiFi.disconnect(true, false)` 关闭 STA；`eraseap=true` 只用于“彻底忘记”。

### 5.4 文本输入

密码/SSH 密码输入统一用：

```cpp
const auto& ks = M5Cardputer.Keyboard.keysState();
if (!ks.word.empty()) appendText(ks.word);   // 可能是 UTF-8/多字节
if (ks.del) backspace();
if (ks.enter) submit();
```

- 不要调用 `readActions()` 把 `; . , /` 当方向键；
- `ks.word` 是本次按键产生的字符；先过滤控制字符；
- 光标用 500 ms 闪烁 `_`，只重画输入行；
- 密码显示 `*`，不显示原文；SSH 密码同样；
- `Aa` 控制大小写，`Fn` 不参与文本输入（避免和快捷键冲突）。

### 5.5 SSH 任务模型

```cpp
// setup()
libssh_begin();
wifi_store::autoConnect();          // 非阻塞，15 s 超时
xTaskCreatePinnedToCore(sshTask, "ssh", 56 * 1024, nullptr, 5, nullptr, 1);

// sshTask(): 只做网络/SSH，不碰 LCD/Keyboard
// loop():   只做键盘/LCD，通过 QueueHandle_t 发按键，通过双缓冲读屏幕
```

- 参考 LibSSH `exec` 示例：`configSTACK = 51200`；本项目用 **56 KB**，
  并在串口打印 `uxTaskGetStackHighWaterMark()`。
- UI 与 SSH 共享终端缓冲，必须用 `SemaphoreHandle_t` 保护；
  推荐“SSH 写 back buffer，UI 在 vsync/定时器交换”的双缓冲，避免 tearing。
- SSH task 每 10 ms `vTaskDelay`，每轮 `ssh_channel_read_nonblocking`；
  不要 `ssh_channel_read` 阻塞。
- 连接/认证阶段可以短阻塞，但每次不超过 1 s，期间 `vTaskDelay(1)` 喂狗。

### 5.6 SSH 会话流程

```cpp
ssh_session s = ssh_new();
ssh_options_set(s, SSH_OPTIONS_HOST, host);
ssh_options_set(s, SSH_OPTIONS_PORT, &port);
ssh_options_set(s, SSH_OPTIONS_USER, user);
ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
ssh_options_set(s, SSH_OPTIONS_KNOWNHOSTS, "/spiffs/ssh/known_hosts");
ssh_connect(s);

// host key: verify_knownhost() -> 未知则 UI 确认 -> ssh_write_knownhost()
// auth:
ssh_userauth_password(s, nullptr, password);
// 或 ssh_userauth_publickey_auto(s, nullptr, nullptr);

ssh_channel ch = ssh_channel_new(s);
ssh_channel_open_session(ch);
ssh_channel_request_pty_size(ch, "xterm-256color", 40, 13);
ssh_channel_request_shell(ch);
```

读循环：

```cpp
char buf[512];
int n = ssh_channel_read_nonblocking(ch, buf, sizeof(buf), 0);
if (n > 0) terminalFeed(buf, n);
if (ssh_channel_is_eof(ch)) { /* closed */ }
```

### 5.7 终端解析器

第一版只做 shell 必需子集，后续再补：

| 类别 | 序列 | 说明 |
| --- | --- | --- |
| 光标 | `ESC[A/B/C/D`、`ESC[H`、`ESC[<r>;<c>H` | 上下左右/定位 |
| 擦除 | `ESC[J`、`ESC[K` | 清屏/清行 |
| 滚动 | `ESC[S/T`、`ESC[r` | 滚动区 |
| 颜色 | `ESC[0/1/4/7/30-37/40-47/90-97/100-107m` | 16 色映射 RGB565 |
| 备用屏 | `ESC[?1049h/l` | vim/htop |
| 其他 | `ESC[s/u`、`ESC 7/8`、OSC 标题 | 保存/恢复、吞掉 |

- 显示 **40×13**；滚动历史 100 行（`40*100*3 ≈ 12 KB`）；
- 只重画 dirty rows，避免整屏 flush；
- UTF-8 解码；框线字符映射 ASCII 回退；
- 不支持 Sixel/图片/鼠标；`vim` 可用基本编辑，`htop` 可用基本刷新。

### 5.8 SSH 键位

| 键 | 终端动作 |
| --- | --- |
| 可打印键 | 发送 UTF-8 字节 |
| `ENTER` | `\r` |
| `BKSP` | `0x7f` |
| `TAB` | `\t` |
| `esc`（单独按） | `0x1b`，不退出 |
| `Fn+; . , /` | `ESC[A/B/C/D` |
| `Ctrl+字母` | `0x01..0x1a` |
| `Aa` | Shift（由 `ks.word` 体现） |
| `Fn+BKSP` | 断开会话 → SSH profile 列表 |
| `Fn+Q` | 只断开会话 |
| `Opt` | 第一版不映射 Ctrl；可配置 |

> 与键盘功能一致：`Fn+` 是 ESC，`Fn+BKSP` 是返回。SSH 终端里 `esc` 必须能发给
> 服务器，所以不能拿它当退出。

---

## 6. 分阶段实施

### Phase 0：launcher 滚动 + SETTINGS 壳（约 1 天）

**目标**：第 5 个功能能放进去，且不破坏现有 4 个功能和九宫格。

任务：

1. `launcher` 增加 `kAppSettings=5`、`kAppSsh=6`（先占位）。
2. `menuTop`/`kMenuVisible=4`；`draw()` 画 `menuTop..menuTop+3`。
3. `menuLoop()` 上下移动时滚动窗口；`ENTER` 映射到 `kMenuApp[]`。
4. `kBootName[]` 加 `SETTINGS`、`SSH`；`currentApp()` 上限改 `kAppSsh`。
5. `main.cpp` 加 `Running::kSettings/kSsh`，先空实现（`setup` 画 “COMING SOON”）。
6. 状态行显示 `2/3` 或 `^/v` 提示；右侧九宫格回归测试。

验收：

- 4 项旧功能仍能进入；
- 第 5/6 项可见、可选中、可滚动；
- 九宫格四页仍工作；
- 启动进度条名称正确。

### Phase 1：WiFi 存储 + 扫描 + 密码 + 连接（约 2–3 天）

**目标**：能扫描、选 AP、输密码、连接成功、保存、开机自动连。

任务：

1. `wifi_store`：NVS 读写、增删、lastSsid、优先级；host 单测。
2. `settings_app` 页面：SETTINGS → WIFI 主页面。
3. 异步扫描 + 去重/排序 + 5 行滚动列表。
4. 密码输入页（masked、光标、BKSP、Aa、长度校验）。
5. 连接状态机 + 15 s 超时 + 错误分类。
6. 成功后保存 + 显示 IP；失败不写。
7. `settingsapp::setup()` 自动连接；`settingsapp::loop()` 非阻塞轮询。
8. 写 `tests/host/test_wifi_store.cpp`：序列化、覆盖、满 5 个淘汰、last 更新。

验收：

- 2.4 GHz AP 列表正确；5 GHz 不出现；
- 正确密码连接、拿到 IP；错误密码有明确提示；
- 断电重开自动连上上次网络；
- 开放网络跳过密码页；
- 保存/忘记/重连都正常；
- UI 不卡、看门狗不触发。

### Phase 2：WiFi 健壮性与体验（约 1 天）

- 隐藏 SSID：第一版不做手动输入；扫描不到就不显示；后续“ADD HIDDEN”再加。
- WPA3：用 WPA3 AP 实测；若 `WiFi.begin` 不支持则 UI 明确报 `AUTH TYPE NOT SUPPORTED`。
- 企业级 802.1X：扫描时识别并拒绝，提示 `ENTERPRISE NOT SUPPORTED`。
- 重连：`WiFi.setAutoReconnect(true)`；断线后 UI 状态从 CONNECTED → RECONNECTING。
- 增加“FORGET CURRENT”二次确认。
- 快速连续扫描/连接不泄漏（`scanDelete()`、`WiFi.disconnect`）。
- 增加 `WiFi.setSleep(false)` 的对比测试（SSH 延迟 vs 功耗）。

### Phase 3：SSH 技术 spike（约 1–2 天，先不接 UI）

**目标**：证明 LibSSH-ESP32 在当前 core 2.0.17 / S3 / 无 PSRAM 上能连、能跑 shell。

任务：

1. 在 `platformio.ini` 加 LibSSH-ESP32（固定 tag/commit，不用 master）。
2. 写 `tools/ssh_spike/` 或临时 `ssh_app`：WiFi 连上后 `ssh_new` →
   `ssh_connect` → `ssh_userauth_password` → `ssh_channel_request_shell`。
3. 开 56 KB task；串口打印 free heap / stack high-water。
4. 连接本机/局域网 OpenSSH，执行 `echo ok`、`ls`。
5. 测 host key 未知/变更路径。
6. 记录：
   - 连接耗时；
   - 峰值 heap；
   - 最小剩余 stack；
   - 30 min 会话稳定性。

**Go/No-Go**：

- 若 heap < 40 KB 或栈 < 8 KB：缩 buffer / 减终端历史 / 评估 libssh2；
- 若 LibSSH 在 core 2.0.17 编译失败：先别改平台；换 libssh2（BSD）方案；
- 若只能跑 exec 不能跑 shell：检查 `request_pty` / `request_shell`。

### Phase 4：SSH MVP（约 3–4 天）

- `ssh_app` profile 列表（先单 profile 也可，但文件格式按多 profile 设计）。
- `ssh_store`：SPIFFS profile 读写、known_hosts 路径。
- 密码认证 + host key 确认 UI。
- 终端 40×13 + 基本 ANSI（光标/清屏/SGR/滚动）。
- 键盘输入队列；Enter/BKSP/Tab/Esc/方向键/Ctrl。
- `Fn+BKSP` 断开回列表；`Fn+Q` 只断开。
- WiFi 未连接时显示 `NO WIFI`，引导去 SETTINGS。

验收：

- 密码登录 OpenSSH；
- `ls`、`cd`、`cat`、`nano` 基本可用；
- `Ctrl+C` 能中断 `ping`/`sleep`；
- `vim`/`htop` 至少能进出备用屏（不要求完美）；
- 网络断开有明确错误，不崩；
- 30 min 会话不重启、内存不持续下降。

### Phase 5：SSH 完善（约 3–5 天）

- 多 profile 菜单、新增/编辑/删除。
- Ed25519 key 认证（`ssh_pki_import_privkey_file`）。
- known_hosts 持久化 + 主机密钥变更警告。
- ANSI 补全：滚动区、插入/删除行、保存/恢复光标、UTF-8 框线。
- 滚动历史、字体 40×13 / 20×7 切换（可选）。
- 断线重连/keepalive；`SSH_OPTIONS_TIMEOUT`。
- 内存/栈 profile；长时间测试。
- 文档：`docs/SSH.md`、README、DEV-STATE。

---

## 7. 测试矩阵

### 7.1 WiFi

| 场景 | 预期 |
| --- | --- |
| 2.4 GHz WPA2 正确密码 | 连接、IP、保存、重启自动连 |
| 错误密码 | `WRONG PASSWORD`，不保存 |
| AP 不存在 | `NO AP`，可重试 |
| 开放网络 | 跳过密码页，直接连接 |
| WPA3 | 能连则连；不能连明确提示 |
| 5 GHz SSID | 不显示 |
| 企业级 | 显示但拒绝，提示不支持 |
| 隐藏 SSID | 不显示（第一版） |
| 保存 5 个后新增 | 淘汰最久未用 |
| 忘记当前 | NVS 删除，下次不自动连 |
| 扫描中按 esc | 取消/返回不崩 |
| 连接中按 esc | `WiFi.disconnect`，回主页面 |

### 7.2 SSH

| 场景 | 预期 |
| --- | --- |
| 密码登录 | shell 可用 |
| 错误密码 | 认证失败提示，可重试 |
| 未知 host key | 显示 SHA256，确认后写入 known_hosts |
| host key 变更 | 拒绝连接，警告 |
| Ctrl+C | 发送 0x03，远端中断 |
| `vim` 打开/退出 | 备用屏正常 |
| 网络拔掉 | 错误状态，不崩 |
| 30 min idle | 连接保持或 keepalive 重连 |
| 退出 | 关闭 channel/session，回列表/launcher |
| 低内存 | 不重启，stack watermark 有记录 |

### 7.3 回归

- RF 采集/回放/SCAN；
- NFC 读卡/模拟/改名；
- 键盘 USB/BLE/映射；
- BACKUP/RESTORE；
- 九宫格四页；
- USB mode 0 烧录；
- 三个 env 编译。

---

## 8. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| LibSSH LGPL-2.1 | 分发合规 | 加 LICENSE/THIRD-PARTY；提供可重链接目标文件；或改 libssh2 |
| core 2.0.17 太旧 | LibSSH 编译/运行问题 | Phase 3 先 spike；失败则 libssh2；不要轻易升级平台 |
| 无 PSRAM / 320 KB | SSH OOM/栈溢出 | 56 KB task；测 watermark；缩小终端历史/包缓冲 |
| WiFi + BLE 并存 | 内存/射频冲突 | SSH/Settings 不初始化 BLE；一次一个 app |
| WiFi 密码明文 | 设备丢失泄露 | 明确威胁模型；后续 NVS encryption 单独迁移 |
| SSH host key 未验证 | MITM | 第一版就做 known_hosts/TOFU，不跳过 |
| 2.4 GHz 干扰 | 扫描/连接不稳 | 重试/超时；SSH 用 keepalive；必要时换信道 |
| 菜单项增多 | UI 溢出 | 先做滚动；列表组件复用 |
| 烧录 COM 变化 | 外出开发受阻 | 文档写清 1200bps/G0；带 USB 数据线；保留可回滚固件 |
| M5-SSH 参考代码平台不同 | 直接复制失败 | 只移植纯终端状态机；不搬 IDF 5.4 依赖 |

---

## 9. 需要先做的仓库整理

1. 根目录加 `LICENSE`（建议 MIT）。
2. 加 `docs/THIRD-PARTY.md`，记录：
   - M5Cardputer / M5Unit-NFC / M5GFX（各自许可证）；
   - LibSSH-ESP32 + libssh（LGPL-2.1）；
   - 参考项目 M5-SSH-Wireguard（MIT）与 aat440hz（MIT），注明只参考未复制。
3. `README.md` 增加 WiFi/SSH 路线图链接到本文。
4. `.gitignore` 增加：
   ```text
   docs/wifi-*.log
   docs/ssh-*.log
   ```
   （避免把 SSID/密码/主机名日志提交。）
5. **LGPL 发布包（已接受 LGPL 后必须做）**：
   - `release/ssh-lgpl/` 放 LibSSH/libssh 的目标文件或源码；
   - 附 `BUILD.md`（PlatformIO/编译器版本、重链接命令）；
   - 附 `THIRD-PARTY.md` 与 LGPL 全文；
   - 只发 `.bin` 不算完成合规包。

---

## 10. 参考资料

### 官方 / 平台

- [M5Stack Cardputer ADV 文档](https://docs.m5stack.com/en/core/Cardputer-Adv)
- [M5Stack M5Cardputer 库](https://github.com/m5stack/M5Cardputer)
- [Arduino-ESP32 Wi-Fi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html)
- [Arduino-ESP32 WiFiScan.h (2.0.17)](https://github.com/espressif/arduino-esp32/blob/2.0.17/libraries/WiFi/src/WiFiScan.h)
- [ESP-IDF 4.4 NVS](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-reference/storage/nvs_flash.html)
- [ESP-IDF 4.4 Wi-Fi](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-reference/network/esp_wifi.html)

### SSH

- [LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32) — LGPL-2.1；core 2.0.17 / S3；`libssh_begin()`；known_hosts 示例
- [LibSSH-ESP32 COPYING](https://github.com/ewpa/LibSSH-ESP32/blob/master/COPYING)
- [LibSSH-ESP32 exec 示例](https://github.com/ewpa/LibSSH-ESP32/blob/master/examples/exec/exec.ino) — 51,200 B task stack
- [M5-SSH-Wireguard](https://github.com/bpivk/M5-SSH-Wireguard) — MIT；Cardputer ADV 完整终端/WiFi manager；需 core 3.x，参考用
- [SSHClient-M5Cardputer](https://github.com/aat440hz/SSHClient-M5Cardputer) — MIT；最小 LibSSH 示例
- [Arduino-esp32sshclient](https://github.com/J-Rios/Arduino-esp32sshclient) — libssh2 proof of concept；不用于生产
- [libssh2](https://www.libssh2.org/) — BSD-3；备用方案
- [libssh 官网](https://www.libssh.org/)

### 可选参考

- [WiFiManager](https://github.com/tzapu/WiFiManager) — MIT；captive portal 流程参考，不直接用
- [M5-SSH-Wireguard 终端实现](https://github.com/bpivk/M5-SSH-Wireguard/blob/main/ssh_client_adv.ino) — ANSI/VT100 状态机参考（MIT）

---

## 11. 下一步（按顺序）

1. Phase 0 菜单滚动 + SETTINGS 壳；
2. Phase 1 WiFi 扫描/连接/保存/自动连接；
3. Phase 2 WiFi 边界；
4. Phase 3 SSH spike + Go/No-Go；
5. Phase 4 SSH MVP；
6. Phase 5 SSH 完善；
7. 每个 Phase 结束更新 `README.md` / `docs/DEV-STATE.md` 并提交。

> **已决定的决策点：**
> **A. SSH 库：接受 LGPL 的 LibSSH-ESP32**（2026-11-13）。
> **B. SSH 密码：默认保存**（成功后写 NVS `ssh/passN`，可在 profile 编辑页清除；
> 明文存储，设备丢失即泄露）。
> Phase 0–2 仍可独立推进；Phase 3 直接按 LibSSH-ESP32 做 spike。

---

## 12. 实施状态（2026-11-14）

已落地（代码已提交前的开发树）：

| 阶段 | 状态 | 说明 |
| --- | --- | --- |
| Phase 0 菜单滚动 + SETTINGS | ✅ | launcher 六项、4 行滚动、SETTINGS 壳 |
| Phase 1 WiFi 扫描/连接/保存/自动连接 | ✅ | `settings_app` + `wifi_store` |
| Phase 2 WiFi 边界 | 🟡 | 外网 TCP 测试已加；WPA3/企业级/隐藏 SSID 尚未逐项实测 |
| Phase 3 SSH spike | ✅ | LibSSH-ESP32 5.10.0 在 core 2.0.17 编译、登录成功 |
| Phase 4 SSH MVP | ✅ | 密码认证、host key TOFU、40×13 终端 |
| Phase 5 SSH 完善 | 🟡 | 多 profile、光标编辑、host key 指纹确认、Ed25519 key、ANSI 16 色/UTF-8 回退/64 行历史、keepalive/超时/R 重试已完成；加密 key、自动无限重连、完整备用屏待做 |

与本文原计划的偏差（以代码为准）：

- 第一版 SSH profile 存在 **NVS `ssh`**，不是 SPIFFS `profiles.txt`；
  当前最多 4 个（`nameN/hostN/portN/userN/passN`），旧的单 profile 键会自动迁移到 profile 0。
- SSH 密码默认保存到 NVS `ssh/passN`（每个 profile 一份）；明文存储。
- host key 首次连接**显示 SHA256 并交互确认**；变更时仍会报
  `HOST KEY CHANGED`，错误页 `K` 清空 known_hosts、`R` 重试。
- 终端支持 ANSI 16 色、UTF-8 ASCII 回退、64 行历史（`Opt+; .`）；
  备用屏仍只清屏，不保存主屏。
- SSH 连接超时 15 s，keepalive 30 s；没有无限自动重连，错误页 `R` 手动重试。
- 详细使用说明见 `docs/SSH.md`。
- WiFi 从 SETTINGS 返回 launcher 走**软返回**，保留 WiFi 栈；这是为了让
  launcher header 图标反映真实连接状态。

下一步优先级：

1. Ed25519 加密私钥/RSA key（可选）与 host key 单条删除；
2. 自动重连/断线恢复、终端完整备用屏；
3. WiFi WPA3/隐藏 SSID/忘记网络确认；
4. 补 `LICENSE`、LGPL 可重链接发布包。
