# Cardputer live sessions

Shares the Desktop's **live** DeepSeek Harness Sessions with an M5Stack Cardputer
over a LAN WebSocket. This replaces the ACP/HTTP relay: the plugin runs inside the
Desktop Host and calls `ctx.sessionController`, so the Cardputer and the Web UI
drive the *same* Agent and the *same* Session log.

## Endpoint

```text
ws://<desktop-lan-ip>:9101/cardputer?token=<token>
GET http://<desktop-lan-ip>:9101/cardputer/health?token=<token>
```

Port **9100 is deliberately left free** for the old `harness-bridge` rollback.
Authentication is a shared token, accepted as the `token` query parameter,
`X-Cardputer-Token`, or `Authorization: Bearer`. Only loopback, RFC1918 and
link-local peers are accepted unless `allowNonLan` is set. There is no TLS.

## Device → Host

| Frame | Meaning |
|---|---|
| `{"type":"ping"}` | Liveness; answered with `pong`. |
| `{"type":"sessions"}` | List visible Sessions. |
| `{"type":"open","sessionId":"…"}` | Subscribe to one Session and receive its snapshot. |
| `{"type":"create","cwd":"…"}` | Create a Session, then open it. |
| `{"type":"prompt","sessionId":"…","text":"…"}` | Queue a user prompt. |
| `{"type":"cancel","sessionId":"…"}` | Cancel the running turn. |
| `{"type":"stt.begin","rate":16000,"channels":1,"bits":16}` | Start streamed voice input. |
| `{"type":"stt.chunk","data":"<base64 PCM>"}` | Append audio. |
| `{"type":"stt.end"}` | Finish and transcribe. |

## Host → device

| Frame | Meaning |
|---|---|
| `{"type":"hello","protocol":1}` | Sent once after the upgrade. |
| `{"type":"sessions","items":[…]}` | `id`, `shortId`, `title`, `preview`, `updatedAt`, `running`, `blank`. |
| `{"type":"opened","sessionId":"…"}` | The subscription was accepted. |
| `{"type":"snapshot","sessionId":"…","messages":[…],"cursor":n}` | Recent committed history. |
| `{"type":"delta","sessionId":"…","text":"…"}` | Assistant text fragment, coalesced (~60 ms / 512 chars). |
| `{"type":"message","sessionId":"…","seq":n,"role":"user\|assistant","text":"…"}` | One committed message, from either end. |
| `{"type":"status","sessionId":"…","running":true}` | Turn started or settled. |
| `{"type":"accepted"}` / `{"type":"cancelled"}` / `{"type":"created"}` | Action receipts. |
| `{"type":"stt","text":"…"}` | Recognized speech. |
| `{"type":"error","error":"…"}` | Failure detail. |

## Behaviour

- **Deltas** come from `sessionController.follow({ assistantStream: true })`'s
  `assistant-stream` frames: `chunk.type === "text-delta"` carries the model's
  partial text, so the Cardputer renders incrementally instead of polling.
- **Desktop → Cardputer** works because the follow stream carries every committed
  `user/message` and `assistant/message` event, whichever surface produced it.
- **Titles/previews** come from the `title` projection plus one bounded history
  read per recent Session, cached per `updatedAt` (10 Sessions by default).
- **Voice** streams base64 PCM in chunks; the Host assembles a WAV in a temp file
  and runs the bundled `stt.py` (Vosk). The device needs no 64 KB static buffer.

## Limitations

- LAN only, no TLS; the token is a shared secret.
- Prompt acceptance is not completion; completion arrives as `message` + `status`.
- Tool-permission prompts raised by a turn are answered in the Desktop, not on
  the Cardputer.
