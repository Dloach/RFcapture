# HARNESS realtime WebSocket rewrite

Status: **implemented.** The Host plugin (`harness-ws/plugin`) and the rewritten
firmware are live; see [HARNESS.md](HARNESS.md) for the shipped architecture,
the protocol, and the verification results. The design notes below are kept as
the rationale for choosing a Host plugin over the ACP relay.

## Why the current bridge cannot be realtime

Current:

```text
Cardputer -> HTTP poll -> standalone.py -> ACP child (separate profile) -> separate Session
```

ACP creates its own Agent, owns the Session write handle, and the desktop and
Cardputer cannot both hold it. HTTP polling only sees committed history. This is
a relay design, not live sync.

## Target

```text
Cardputer (WS + M5Canvas, non-blocking network)
        ⇅
dsh-cardputer-ws (Host plugin in the desktop profile)
        ⇅
ctx.sessionController (same Session as the Web UI)
```

The Host plugin subscribes to the same Session events, accepts prompt, and
forwards assistant deltas. Both desktop and Cardputer can watch the same live
Session.

## Host plugin

- Host bundle, no client UI required.
- Use `ctx.sessionController`:
  - list/create/resume if needed;
  - `prompt`;
  - `observeEvent` / event stream for committed events;
  - `cancel`.
- Expose a LAN WebSocket, e.g. `ws://192.168.0.88:9101/cardputer`.
- Messages:
  ```json
  {"type":"snapshot","sessionId":"...","messages":[...],"running":true}
  {"type":"message.delta","sessionId":"...","role":"assistant","text":"..."}
  {"type":"message","sessionId":"...","seq":123,"role":"assistant","text":"..."}
  {"type":"status","running":true}
  {"type":"prompt","sessionId":"...","text":"..."}
  ```
- Token auth, LAN only.
- Install only after verified; Creator mode's runtime inspection and
  plugin experiment make this manageable.

## Cardputer firmware

Follow [Cardputer Hermes Terminal](https://github.com/m0rg0t/cardputer-hermes-terminal):

- full-screen `M5Canvas` (240x135) and `pushSprite`;
- `dirty_` flag, never `fillScreen` in the key loop;
- non-blocking WS state machine;
- physical key press events (already implemented in `polling_keyboard.h`);
- transcript scroll offset, page older history at top;
- session deck: title + short id + last message preview;
- explicit connecting/loading/ready/error status.

Reference implementation details:

- `src/app_draw.cpp`: `fullScreenCanvas()`, `createSprite`, `pushSprite`;
- `src/app.cpp`: `dirty_` + `draw()`, `Keyboard.isChange()`;
- `docs/ARCHITECTURE.md`: WSS event flow.

## Open decisions

1. Host plugin is required for truly shared live Session. It is a third-party
   profile bundle and may trigger the DSH safe-mode prompt; Creator mode is the
   supported authoring path, not a security bypass.
2. ~~Without a Host plugin, use ACP relay only; not realtime.~~ Resolved: the
   Host plugin shipped, and the ACP relay has since been retired outright.
3. ~~If the safe-mode prompt cannot be accepted, fall back to a dedicated
   ACP/SDK profile.~~ Not needed; see HARNESS.md.
