# 远程 OTA

固件使用 ArduinoOTA over WiFi。分区表 `default_8MB.csv` 自带双 app 分区：

```text
app0   3.3 MB
app1   3.3 MB
spiffs 1.5 MB
```

OTA 只更新 app 分区，NVS / SPIFFS / WiFi 凭据保留。当前固件约 2.4 MB。

## 常驻 OTA

从本版开始，除 SETTINGS 页面外，任意 app 启动后都会：

1. 读取已保存 WiFi；
2. 异步连接；
3. 自动启动 ArduinoOTA（host `cardputer`，端口 3232，密码 `cardputer-ota`）；
4. 主循环持续 `ArduinoOTA.handle()`。

所以不需要停在 `SETTINGS → OTA UPDATE` 页面。launcher、HARNESS、DRUM、
RECORDER 等 app 都可以被远程烧录。

SETTINGS 的 OTA 页面仍保留独立生命周期，用于现场查看 IP/进度；在这页里
退出会 `ArduinoOTA.end()`，这是旧行为。

## 第一次仍要 USB

OTA 服务本身要先通过 USB 烧录一次：

```powershell
pio run -e cardputer-adv -t upload --upload-port COM13
```

## 电脑端

拿到设备 IP（路由器 DHCP 或 SETTINGS 页面）后：

```powershell
pio run -e cardputer-ota -t upload --upload-port 192.168.0.74
```

mDNS 可用时：

```powershell
pio run -e cardputer-ota -t upload --upload-port cardputer.local
```

环境定义：

```ini
upload_protocol = espota
upload_port = cardputer.local
upload_flags = --auth=cardputer-ota
```

## 修改密码

同时修改：

- `src/ota_service.cpp` 的 `kPassword`；
- `src/settings_app.cpp` 的 `kOtaPassword`；
- `platformio.ini` 的 `--auth=...`。

## 安全

OTA 只在可信局域网使用；密码是共享口令，不是强认证。不要把 3232 端口
直接暴露到公网。远程跨网访问应走 VPN/Tailscale，不要做公网端口转发。
