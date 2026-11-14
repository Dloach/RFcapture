# SSH 客户端使用说明

基于 LibSSH-ESP32 5.10.0（LGPL-2.1）。进入 launcher 的 **SSH**，先选 profile，再连接。
WiFi 使用 SETTINGS 里保存的网络。

## SSH 列表

```text
SSH
* DEFAULT
+ NEW PROFILE
ENT connect   E edit   D del   esc back
```

- `ENT`：已有 profile 直接连接；`+ NEW PROFILE` 进入新建；
- `E`：进入选中 profile 的字段编辑页；
- `D`：连按两次删除；
- `R`：重连选中 profile（会话关闭后回列表也可用）；
- `+ NEW PROFILE`：新建，用 `ENT` 进入。

当前最多 4 个 profile，保存在 NVS `ssh`。旧版单 profile 会自动迁移成 `DEFAULT`。

## Profile 页

```text
NAME / HOST / PORT / USER / PASS / CONNECT
```

- `; .` 移动；
- `ENT` 编辑字段，或选中 `CONNECT` 连接；
- `esc` 回列表。

编辑字段：

- `Opt+,` / `Opt+/` 左右移动光标；裸 `,` / `/` 作为普通字符输入；
- `BKSP` 删除光标前字符；
- 光标处插入；
- `ENT` 保存，`Fn+BKSP` 取消；
- 光标 `_` 闪烁，底部显示 `LEN / POS`。

## Ed25519（默认关闭）

默认固件只做密码认证：profile 没有 `AUTH`/`KEY` 行，SSH 列表也没有 `K`。
Ed25519 的完整代码仍保留在 `src/ssh_app.cpp`，由编译宏控制：

```ini
build_flags = ... -DSSH_ENABLE_ED25519=1
```

重新开启后：

1. TF 卡 `/ssh/id_ed25519`，SSH 列表按 `K` 导入 SPIFFS；
2. profile 的 `AUTH` 切到 `ED25519 KEY`；
3. 服务器 `~/.ssh/authorized_keys` 加入对应公钥。

只支持未加密的 OpenSSH Ed25519 私钥。私钥/密码都是明文存储。

## Host key

- 首次连接新主机：显示 SHA256 指纹，`ENT` 信任并写入
  `/spiffs/ssh/hostkeys.txt`，`esc` 拒绝。
- 主机 key 变更：报 `HOST KEY CHANGED`，不自动覆盖。
- 错误页 `K`：清空 hostkeys；`R`：重试；`ENT`：回列表。
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
- 终端备用屏 `?1049h/l` 保存/恢复主屏；支持滚动区、插入/删除行、插入/删除字符；
  颜色/字符集仍是子集；
- 不支持 Ed25519（默认构建）、加密私钥、RSA key、多跳/代理、SFTP；
- 密码默认保存，可在 profile 里改；删除 profile 会清对应 NVS 数据。
