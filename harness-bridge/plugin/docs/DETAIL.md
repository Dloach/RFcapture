# Cardputer bridge

Local HTTP bridge for the Cardputer `HARNESS` app.

- binds `0.0.0.0:8765` (configurable);
- requires `X-Cardputer-Token`;
- lists Sessions and sends prompts through `ctx.sessionController`, so a prompt sent from the Cardputer appears in the same DSH Session;
- `/stt` accepts a 16 kHz mono WAV and runs the configured Vosk model.

Endpoints:

```text
GET  /cardputer/health
GET  /cardputer/sessions
POST /cardputer/new       {"cwd":"..."}
GET  /cardputer/messages?session=<id>&limit=80
POST /cardputer/prompt   {"session":"<id>","text":"..."}
POST /cardputer/cancel   {"session":"<id>"}
POST /cardputer/stt      body = WAV bytes
```

The bridge is LAN-only. Keep the token private and do not expose the port to the
public internet.
