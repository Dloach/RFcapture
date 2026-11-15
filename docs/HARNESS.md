# Cardputer ↔ DeepSeek Harness

Status: **host bridge and Cardputer app are implemented and compile; not yet installed/tested against the running Desktop profile.**

## Architecture

```text
Cardputer HARNESS app
   HTTP (LAN, X-Cardputer-Token)
        ↓
dsh-cardputer-bridge (DSH Host plugin, port 8765)
        ↓
ctx.sessionController
        ↓
the same DSH Session (messages, prompt, running state)
```

The bridge does **not** pretend to be a browser and does not open a second
session unless asked. It calls the Host session controller directly.

HTTP endpoints:

```text
GET  /cardputer/health
GET  /cardputer/sessions
POST /cardputer/new       {"cwd":"Y:/2026/cardputer"}
GET  /cardputer/messages?session=<id>&limit=80
POST /cardputer/prompt    {"session":"<id>","text":"..."}
POST /cardputer/cancel    {"session":"<id>"}
POST /cardputer/stt       body = 16 kHz mono PCM16 WAV
```

## DSH plugin

Install directory:

```text
Y:\2026\cardputer\harness-bridge\plugin
```

Local install path is staged to a **local NTFS directory** before installation,
because pnpm junctions to the SMB `Y:` path were not resolvable by Node on
this machine:

```text
C:\Users\dloach\.dsh\cardputer-bridge-plugin
```

Bridge config in `cordis.patch.yml`:

```yaml
host: 0.0.0.0
port: 8765
token: cardputer-dev
```

## Cardputer app

launcher → **HARNESS**.

- `Fn+C`: bridge host/token config;
- `Fn+S`: Session list (Enter open, N new);
- `Fn+V`: 2 s voice note → `/stt` → text inserted into input;
- type ASCII, `ENT` send;
- `Fn+;` / `Fn+.` scroll chat;
- `esc` back.

NVS namespace `harness`: `host`, `token`, `session`.

Voice model:

```text
Y:\2026\cardputer\harness-bridge\models\vosk-model-small-cn-0.22
```

Vosk venv:

```text
Y:\2026\cardputer\harness-bridge\.venv
```

## Safety / limits

- LAN only, no TLS; token is a simple shared secret.
- Vosk is offline Chinese/English recognition; accuracy depends on mic and model.
- Prompt acceptance is not completion; messages are polled, final assistant text
  appears when committed. Token streaming is not used in this first version.
- Tool permission prompts still belong to the DSH UI; this bridge does not
  auto-approve tools.
- The bridge plugin should not be enabled in the desktop profile until startup
  is verified in isolation; earlier startup blocking caused a DSH safe-mode
  prompt.
