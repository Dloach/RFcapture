# WEB FILES / RECORDER

这两个功能都不初始化 CC1101 / ST25R3916，只使用 WiFi 或板载麦克风 + TF 卡。

## WEB FILES

入口：launcher → **WEB FILES**。

启动时读取 SETTINGS 保存的最后一个 WiFi，连接后启动 HTTP 服务：

```text
http://<屏幕 IP>/
```

默认认证：

```text
user: admin
pass: cardputer
```

浏览器里可以：

- 浏览 TF 目录；
- 下载文件；
- 上传文件到当前目录（覆盖同名文件）；
- 删除文件（目录不递归删）。

限制与安全：

- 只暴露 TF 卡，不暴露 SPIFFS/NVS；
- WebServer 同一时间只处理一个客户端；
- **仅限可信局域网**；Basic auth 是简单口令，不是 TLS；
- 上传**逐块校验认证**：没有凭据的 POST /upload 不会写卡、收到 401，不留半份文件；
- 上传单文件上限 256 MB，且开传前要求剩余空间 ≥ 8 MB；中止（断线/取消）同样不留半份；
- 不要把 80 端口映射到公网。

修改密码：`src/web_app.cpp` 的 `kUser` / `kPass`。

## RECORDER

入口：launcher → **RECORDER**。

- 板载麦克风，16 kHz、mono、16-bit PCM；
- 保存到：

```text
/recordings/rec_000.wav
```

- `S` 开始 / 停止；
- `P` 播放最后一段（≤400 KB，约 12 秒；更长只录音不播放）；
- `esc` 回 launcher；
- 屏幕显示录音时间、峰值电平。

Cardputer ADV 的 ES8311 支持 mic/speaker 共用 I2S；原版 Cardputer 的 mic 和 speaker
共享引脚，本功能没有为原版做分时切换。

录音文件可以直接用 **WEB FILES** 下载，或用 **USB DISK** 拷出。

## 依赖

- `M5Unified` 的 `M5.Mic` / `M5.Speaker`；
- Arduino core 自带 `WebServer`、`WiFi`；
- 不需要额外库。

## 参考

- [M5 Cardputer Mic](https://docs.m5stack.com/ja/arduino/m5cardputer/mic)
- [AudioWiz](https://github.com/thedeadlypixel/AudioWiz)：Cardputer 麦克风录音/可视化参考
- [M5Unified](https://github.com/m5stack/M5Unified)：`Mic_Class` / `Speaker_Class`
