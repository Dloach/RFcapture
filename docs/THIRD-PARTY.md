# 第三方组件与许可证

本固件链接/使用以下第三方代码。发布二进制或源码包时请保留本文件与对应许可证文本。

| 组件 | 版本/来源 | 许可证 | 用途 |
| --- | --- | --- | --- |
| M5Cardputer | `m5stack/M5Cardputer@^1.1.1` | MIT | Cardputer 板级、键盘、显示 |
| M5Unified / M5GFX | 由 PlatformIO 依赖解析 | MIT | 显示与电源 |
| M5UnitUnified | `0.6.4` | MIT | Unit 总线 |
| M5Unit-NFC | `0.1.2` | MIT | ST25R3916 NFC |
| LibSSH-ESP32 | `5.10.0`，`https://github.com/ewpa/LibSSH-ESP32` | **LGPL-2.1** | SSH 客户端（基于 libssh） |
| libssh | 由 LibSSH-ESP32 内置（0.11.x） | **LGPL-2.1** | SSH 协议实现 |
| WiFi / Preferences / SPIFFS | ESP32 Arduino core 2.0.17 | LGPL-2.1 / Apache-2.0（按 core 内文件） | WiFi、NVS、文件系统 |

## LGPL-2.1 注意事项

LibSSH-ESP32 和 libssh 是 LGPL-2.1，不是 MIT。把它们静态链接进 `firmware.bin` 时：

1. 随发布包提供本文件和 LGPL-2.1 全文；
2. 如果修改了 LibSSH/libssh，修改部分仍按 LGPL-2.1 发布；
3. 让接收者能够替换/重新链接该库：提供可重链接的目标文件（`.o`/`.a`）
   或完整源码 + 可复现构建说明，而不只是 `.bin`；
4. 项目自身代码的许可证另行声明；LGPL 只覆盖库本身。

本仓库当前**尚未添加根目录 `LICENSE`**。对外分发前需要先确定项目自身许可证，
并补齐 LGPL 合规包（见 `docs/PLAN-wifi-ssh.md` §9）。

## 参考但未直接复制的项目

- [M5-SSH-Wireguard](https://github.com/bpivk/M5-SSH-Wireguard) — MIT；
  参考了 Cardputer SSH 的页面组织、WiFi manager 和终端 ANSI 思路。
- [SSHClient-M5Cardputer](https://github.com/aat440hz/SSHClient-M5Cardputer) — MIT；
  参考最小 LibSSH 连接流程。
- [WiFiManager](https://github.com/tzapu/WiFiManager) — MIT；只参考扫描/保存流程，
  本固件使用自己的 UI 与 NVS 存储。
