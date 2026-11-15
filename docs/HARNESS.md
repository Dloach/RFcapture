# Cardputer ↔ DeepSeek Harness

Status: standalone bridge tested; Cardputer HARNESS firmware flashed. The desktop
profile never loads a third-party plugin.

## Architecture

```text
Cardputer HARNESS app
   HTTP (LAN, X-Cardputer-Token)
        ↓
harness-bridge/standalone.py (port 9100)
        ↓
dsh --profile cardputer (ACP child, NDJSON JSON-RPC)
        ↓
persisted DSH sessions (%USERPROFILE%\.dsh\sessions)
```

The bridge is a separate process. It does not install a DSH profile plugin, so
it does not trigger the desktop safe-mode warning.

## Start

```powershell
powershell -ExecutionPolicy Bypass -File Y:\2026\cardputer\harness-bridge\run-standalone.ps1
```

Health:

```text
http://192.168.0.88:9100/cardputer/health
```

The bridge removes stale ACP children before starting. If it is force-killed, an
orphan ACP child can hold a Session; the run script cleans only our own
`dsh-standalone` + `cardputer` children.

## API

```text
GET  /cardputer/health
GET  /cardputer/sessions
POST /cardputer/new       {"cwd":"..."}
GET  /cardputer/messages?session=<id>&limit=80
POST /cardputer/prompt    {"session":"<id>","text":"..."}
POST /cardputer/cancel    {"session":"<id>"}
POST /cardputer/close     {"session":"<id>"}
POST /cardputer/stt       body = 16 kHz mono PCM16 WAV
```

## Cardputer app

launcher → **HARNESS** 首先进入 **Session 列表**（不是默认聊天）：

- 每项显示首条用户内容作为标题，下面显示短 ID + 最后一条消息预览；
- `;` / `.` 上下移动，列表自动滚动，光标不会跑出屏幕；
- `ENT` 打开当前高亮 Session；
- `Fn+N` 新建会话并直接进入；
- `Fn+C`: bridge host/token 配置；
- `esc` 从列表返回 launcher。

聊天页：

- `Fn+S` 返回 Session 列表；
- `Fn+V`: 2 s 语音 → `/stt` → 填入输入框；
- 输入英文，`ENT` 发送；
- `Fn+;` / `Fn+.` 滚动聊天；
- `esc` 返回 Session 列表。

NVS namespace `harness`: `host`, `token`, `session`。

## Windows 端看不到会话

Cardputer 的 Session 写在：

```text
C:\Users\dloach\.dsh\sessions\--Y-2026-cardputer--\<sessionId>\
```

Desktop Web GUI 当前工作区是 `G:\Projects`，所以默认列表里看不到
`Y:\2026\cardputer` 下的 Session。处理：

1. 在 DSH 里打开/添加工作区 `Y:\2026\cardputer`；
2. 刷新会话列表；
3. 选择最新的 Session（可能暂时没有标题）。

## 两端接力

bridge 在每轮回复后自动 `session/close`，释放持久化 Session：

1. Cardputer 发消息 → ACP 回复 → bridge close；
2. Windows 打开同一个 Session，查看历史并继续；
3. Windows 离开/关闭该 Session 后，Cardputer 再发消息，ACP resume 同一个 Session；
4. Cardputer 的 `/messages` 读同一个 JSONL 历史，因此 Windows 端的回复也能显示。

同一时间只能一端拥有活动 Agent；另一端发 prompt 会得到 `BUSY`/resume 错误。

## 语音

```text
Vosk model: Y:\2026\cardputer\harness-bridge\models\vosk-model-small-cn-0.22
Vosk venv:  Y:\2026\cardputer\harness-bridge\.venv
```

Vosk is offline Chinese/English recognition; accuracy depends on the microphone
and model. It is not a Chinese IME.

## Known limits

- LAN only, no TLS; token is a shared secret.
- Prompt acceptance is not completion; the Cardputer polls committed history.
- Tool permission requests are auto-allowed by the standalone bridge for this
  trusted workspace. The desktop plugin path is intentionally not used.
- The desktop and Cardputer cannot hold the same Agent simultaneously.
