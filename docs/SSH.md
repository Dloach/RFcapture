# SSH 客户端使用说明

基于 LibSSH-ESP32 5.10.0（LGPL-2.1）。进入 launcher 的 **SSH**，先选 profile，再连接。
WiFi 使用 SETTINGS 里保存的网络。

## SSH 列表

```text
SSH
* DEFAULT
+ NEW PROFILE
ENT open   E edit   D del   K key   esc back
```

- `ENT`：打开选中 profile；
- `E`：进入 profile 编辑；
- `D`：连按两次删除；
- `K`：从 TF 卡导入 Ed25519 私钥（见下）；
- `R`：重连选中 profile（会话关闭后回列表也可用）；
- `+ NEW PROFILE`：新建，用 `ENT` 进入。

当前最多 4 个 profile，保存在 NVS `ssh`。旧版单 profile 会自动迁移成 `DEFAULT`。

## Profile 页

```text
NAME / HOST / PORT / USER / AUTH / PASS|KEY / CONNECT
```

- `; .` 移动；
- `ENT` 编辑字段；`AUTH` 行按 `ENT` 在 `PASSWORD` / `ED25519 KEY` 间切换；
- `CONNECT` 行按 `ENT` 连接；
- `esc` 回列表。

编辑字段：

- `,` / `/` 左右移动光标；
- `BKSP` 删除光标前字符；
- 光标处插入；
- `ENT` 保存，`Fn+BKSP` 取消；
- 光标 `_` 闪烁，底部显示 `LEN / POS`。

## Ed25519 私钥

当前只支持**未加密的 OpenSSH Ed25519 私钥**：

1. 把私钥放到 TF 卡的 `/ssh/id_ed25519`（公钥 `/ssh/id_ed25519.pub` 可选）；
2. SSH 列表按 `K`，会复制到 SPIFFS `/spiffs/ssh/id_ed25519`；
3. profile 的 `AUTH` 切到 `ED25519 KEY`；
4. 服务器 `~/.ssh/authorized_keys` 里加入对应公钥。

导入失败会显示 `NO TF CARD` / `PUT KEY AT /ssh/id_ed25519`。

> 私钥和密码都是明文存储（SPIFFS/NVS）。设备丢失等于凭据泄露。

## Host key

- 首次连接新主机：显示 SHA256 指纹，`ENT` 信任并写入
  `/spiffs/ssh/known_hosts`，`esc` 拒绝。
- 主机 key 变更：报 `HOST KEY CHANGED`，不自动覆盖。
- 错误页 `K`：清空 known_hosts；`R`：重试；`ENT`：回列表。
- 目前没有“只删除某一条主机”的 UI，`K` 是全部清空。

## 终端

- 40 列 × 13 行，ANSI 16 色，UTF-8 框线回退为 ASCII；
- 64 行滚动历史：`Opt+;` 上翻，`Opt+.` 下翻；
- `Fn+; . , /` 方向键；
- `Ctrl+字母` 发送控制字符；
- `Fn+Q` 关闭会话，`Fn+BKSP` 退出到 SSH 列表；
- 单次 SSH task 栈 56 KB；串口每 30 s 打印 `heap/stack` 水位。

## 稳定性与限制

- 连接超时 15 s，30 s keepalive；
- 网络断开后回列表，可用 `R` 重试（没有无限自动重连）；
- 终端备用屏 `?1049h/l` 保存/恢复主屏；颜色/滚动区仍是子集；
- 不支持加密私钥、RSA key、多跳/代理、SFTP；
- 密码默认保存，可在 profile 里改；删除 profile 会清对应 NVS 数据。
