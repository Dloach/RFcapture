# HARNESS 看不到 Session：根因与修复记录

## 现象

Cardputer 进入 HARNESS 后，Session deck 为空或一直显示
`Waiting for Desktop...`，但：

- Desktop 插件 health 正常；
- 同一 WiFi 下 WS 能握手；
- Desktop 端会话明明存在。

## 根因链

### 1. 低堆保护误杀 `sessions` 控制帧

`src/harness_app.cpp` 的 `handleMessage()` 原来有一条固定门槛：

```cpp
if (ESP.getFreeHeap() < 18000) {
    usbLogf("[rx] skip len=%u low heap=%u", ...);
    return;
}
```

当时实测空闲堆只有几千字节：

```text
[mem] free=7456
[rx] skip len=15 low heap=7456
```

于是连 55 字节的 `hello`、1–2 KiB 的 `sessions` 列表都被直接丢弃，
`applySessions()` 从未执行，deck 自然是空的。WS 是连上的，但 UI 永远
收不到会话列表。

### 2. 语音静态缓冲把空闲堆压得更低

```cpp
int16_t voiceReadBuf[kVoiceSamples];
uint8_t voiceB64[10800];
```

这两个数组在 BSS 里常驻，即使不录音也占约 18 KiB，让本就不宽裕的
堆进一步低于 18 KiB 门槛。

### 3. 16bpp 全屏 Canvas 是最大单块分配

240×135×2 = 64.8 KiB。改成 8bpp 后为 32.4 KiB，空闲堆明显回升。
（`b491a95` 已改为 `canvas.setColorDepth(8)`。）

## 修复

文件：`src/harness_app.cpp`

### 按帧大小计算门槛

```cpp
const uint32_t need = static_cast<uint32_t>(length) * 2 + 3000;
if (ESP.getFreeHeap() < need) {
    usbLogf("[rx] skip len=%u need=%u heap=%u", ...);
    return;
}
```

- 小控制帧：`hello` / `sessions` / `status` 可以正常解析；
- 大快照：仍要求两倍帧长 + 3 KiB 的余量，避免 JSON 解析时 OOM panic。

### 语音缓冲按需分配

`voiceReadBuf` / `voiceB64` 改为指针：

- `voiceStart()` 时 `malloc`；
- 语音结束或失败时 `free`；
- 空闲时不再占 18 KiB BSS。

## 验证

修复后串口：

```text
[mem] free=19288 min=9304 msgs=4 bad=0 reset=0 hw=16000
```

- `free` 从约 5.9 KiB 提升到约 19 KiB；
- WS health：`clients:1`；
- Cardputer 发出的 `hi sent from cardputer` 出现在 Desktop 当前 Session；
- deck 能收到 `sessions` 并显示标题/预览。

## 相关提交

- `b491a95` — HARNESS app 语音输入、会话 UI、8bpp canvas；
- `ab606a7` — 低堆门槛按帧计算 + 语音缓冲动态分配（关键修复）；
- `f323c36` — `harness-selftest` 自启动验证；
- 本文件 — 根因归档。
