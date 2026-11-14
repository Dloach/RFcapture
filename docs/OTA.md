# 无线烧录（OTA）

固件使用 ESP32 的 **ArduinoOTA**，走 WiFi。分区表 `default_8MB.csv` 自带双 OTA：

```text
app0  3.3 MB
app1  3.3 MB
spiffs 1.5 MB
```

当前固件约 2 MB，OTA 槽放得下。OTA 只更新 app 分区，**NVS / SPIFFS / WiFi 凭据保留**。

## 第一次必须 USB

OTA 服务本身要先有一次 USB 烧录：

```powershell
pio run -e cardputer-adv -t upload --upload-port COM13
```

## 设备端

1. launcher → **SETTINGS**；
2. 选 **OTA UPDATE**，`ENT`；
3. 页面应显示：
   - `WIFI CONNECTED`
   - 本机 IP（例如 `192.168.1.42`）
   - `HOST cardputer.local`
   - `PORT 3232`
   - `AUTH ON`
4. **保持这个页面打开**；离开页面会 `ArduinoOTA.end()`，上传会中断。

## 电脑端

优先用设备屏幕上的 IP：

```powershell
pio run -e cardputer-ota -t upload --upload-port 192.168.1.42
```

mDNS 能解析时也可以：

```powershell
pio run -e cardputer-ota -t upload --upload-port cardputer.local
```

`cardputer-ota` 环境定义在 `platformio.ini`：

```ini
upload_protocol = espota
upload_port = cardputer.local
upload_flags = --auth=cardputer-ota
```

## 认证密码

当前默认：

```text
cardputer-ota
```

要改密码，同时修改两处：

- `src/settings_app.cpp` 的 `kOtaPassword`
- `platformio.ini` 的 `--auth=...`

## 页面状态

- `WAITING FOR WIFI`：WiFi 还没连上；
- `READY`：服务已启动，可以上传；
- `UPLOADING` + 进度条：正在写 app1/app0；
- `DONE - REBOOTING`：写完自动重启；
- `ERROR n`：OTA 错误，看串口/PlatformIO 输出。

## 故障排查

- **`pio` 找不到 cardputer.local**：用屏幕 IP；Windows mDNS 不稳定。
- **连接被拒绝**：确认设备仍停在 OTA UPDATE 页，且 IP 没变。
- **auth failed**：密码不一致，检查上面两处。
- **上传中断**：不要离开页面、不要断电；重新进入 OTA 页再试。
- **新固件起不来**：G0 + USB 重新烧 `cardputer-adv` 即可，OTA 不会覆盖 NVS/SPIFFS。
- **OTA 不是 RF/NFC 模式**：OTA 页面属于 SETTINGS，射频/读卡功能此时未初始化。

## 安全

OTA 只应在可信局域网使用；密码是简单共享口令，不是强认证。不要在公共 WiFi 打开 OTA 页面。
