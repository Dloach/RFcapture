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
   - `PORT 3232  PASS xxxxxxxxxx`
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
upload_flags = --auth=READ_FROM_DEVICE_SCREEN   ; 换成设备屏幕显示的 PASS
```

## 认证密码

**每台设备一个、不进源码仓库**：首次打开 OTA 页面时由硬件随机数生成
10 位口令，存 NVS（`ota`/`pass`），直接显示在 OTA 页的 `PASS ...` 行。
上传前把屏幕上的值填进 `platformio.ini` 的 `--auth=...`（或命令行
`--upload-flags "--auth=屏幕值"`）。

忘记口令时重进 OTA 页即可看到（它一直在 NVS 里）；要换新口令，
进 SETTINGS 删 NVS `ota` 命名空间或直接整块擦除前先备份（谨慎操作）。

## 烧录时的屏幕（USB 与 OTA 一致）

- **USB 烧录**（`pio ... -t upload` / `tools/flash.ps1`）：主机的
  1200 bps touch 一到，固件立即挂起当前功能、全屏显示
  **USB FLASH / PC IS UPLOADING** 与空进度条，然后才复位进 ROM
  下载模式；烧录期间固件不运行，**这帧画面保持到烧录完成**。
  期间所有按键无效——设备只认主机。
- **OTA 烧录**：`UPLOADING` 一起即独占设备（WS / 麦克风 / 聊天 / 按键
  全部停止），进度条覆盖**接收+写入**全过程，完成后停在
  `DONE - REBOOTING`（100%）再重启。

## 页面状态

- `WAITING FOR WIFI`：WiFi 还没连上；
- `READY`：服务已启动，可以上传；
- `UPLOADING` + 进度条：正在写 app1/app0；
- `DONE - REBOOTING`：写完自动重启；
- `ERROR n`：OTA 错误，看串口/PlatformIO 输出。

## 故障排查

- **`pio` 找不到 cardputer.local**：用屏幕 IP；Windows mDNS 不稳定。
- **连接被拒绝**：确认设备仍停在 OTA UPDATE 页，且 IP 没变。
- **auth failed**：密码不一致——用设备 OTA 页 `PASS ...` 行显示的值。
- **上传中断**：不要离开页面、不要断电；重新进入 OTA 页再试。
- **新固件起不来**：G0 + USB 重新烧 `cardputer-adv` 即可，OTA 不会覆盖 NVS/SPIFFS。
- **OTA 不是 RF/NFC 模式**：OTA 页面属于 SETTINGS，射频/读卡功能此时未初始化。

## 安全

OTA 只应在可信局域网使用；密码是简单共享口令，不是强认证。不要在公共 WiFi 打开 OTA 页面。
