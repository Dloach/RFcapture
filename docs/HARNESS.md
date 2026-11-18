# Cardputer ↔ DeepSeek Harness (live WebSocket)

Status: **live sharing implemented and verified on hardware.** The Cardputer and
the Desktop drive the *same* Session, through a DSH Host plugin. The old ACP/HTTP
relay has been retired — there is no rollback path any more.

## Architecture

```text
Cardputer HARNESS app                    Desktop
  M5Canvas UI, non-blocking WS client
        │  ws://192.168.0.88:9101/cardputer?token=...
        ▼
  dsh-cardputer-ws  (Host plugin, runs inside the Desktop Host)
        │  ctx.sessionController
        ▼
  the Session the Web UI is showing (same Agent, same log)
```

No second Agent, no separate Session, no polling: assistant text arrives as
`text-delta` stream frames, and both surfaces see each other's messages because
both read the same Session log.

## Host plugin

| | |
|---|---|
| Source (repo) | `Y:\2026\cardputer\harness-ws\plugin` |
| Installed copy | `C:\Users\dloach\cardputer-ws` (profile links here) |
| Endpoint | `ws://192.168.0.88:9101/cardputer?token=<token>` |
| Health | `GET http://192.168.0.88:9101/cardputer/health?token=<token>` |
| Port 9100 | unused — the old ACP/HTTP bridge was retired (see below) |
| Auth | shared token: `?token=`, `X-Cardputer-Token`, or `Authorization: Bearer` |
| Scope | loopback + RFC1918 + link-local only, no TLS |

Protocol tables are in `harness-ws/plugin/docs/DETAIL.md`. Frames used by the
device: `hello`, `sessions`, `opened`, `snapshot`, `delta`, `message`, `status`,
`accepted`, `cancelled`, `created`, `stt`, `error`; device sends `ping`,
`sessions`, `open`, `create`, `prompt`, `cancel`, `stt.begin/chunk/end`.

The plugin exposes these Host behaviours:

- `list()` for the Session deck, enriched with the `title` projection plus one
  bounded history read per recent Session (cached per `updatedAt`), and with
  `<system-reminder>` runtime context filtered out;
- `create()` + `open` for Fn+N;
- `prompt()` with `mode: 'queue'` so the Desktop shows the user message at once;
- `follow({ assistantStream: true })` for the snapshot, committed messages,
  running status and `text-delta` chunks (coalesced at ~60 ms / 512 chars before
  they reach the device);
- `cancel()`.

### Install / update

```powershell
# from the project, after editing the plugin
powershell -ExecutionPolicy Bypass -File Y:\2026\cardputer\tools\harness-plugin-sync.ps1
```

Then reload: **Settings → Plugins → Cardputer live sessions** (toggle off/on).
A toggle is enough — the loader re-imports the module, so no app restart is
needed. The bundle row is `include:cardputer-ws`.

## Cardputer firmware

`src/harness_app.cpp` (UI) and `src/harness_ws.cpp` (WebSocket client).

- **One full-screen backbuffer.** `M5Canvas` 240×135 at 16 bpp (64.8 KiB heap);
  every draw goes into the sprite and only a dirty frame is `pushSprite`ed. No
  `fillScreen` of the panel in the key path.
- **Non-blocking network.** No `HTTPClient` anywhere. The WS client keeps a
  state machine (`kDown` → `kHandshake` → `kOpen`); while connected it only
  drains bytes that already arrived. The single blocking call is the TCP connect
  on a retry (≤1.5 s, only while offline).
- **Memory.** The old 64 KiB static WAV buffer is gone — voice is captured in
  240 ms slices and streamed as base64 (`stt.chunk`), so the Host assembles the
  WAV. Static RAM is 54.2 % (177,580 B of 327,680) with the canvas on the heap.
- **Session deck is home.** Title, short id, last-message preview, `*` when the
  Session is running. 4 rows, scrolling window.

### Keys

| Where | Key | Action |
|---|---|---|
| Deck | `;` / `.` | move selection (auto-scrolls) |
| Deck | `ENT` | open the highlighted Session |
| Deck | `Fn+N` | create a Session and open it |
| Deck | `Fn+S` | refresh the list |
| Deck | `Fn+C` | link config (host / token / port) |
| Deck | `` ` `` | back to the launcher |
| Chat | `ENT` | send the prompt |
| Chat | `Fn+V` | voice input — `ENT` (or `Fn+V`) stops, 20 s cap; **see note below** |
| Chat | `Fn+;` / `Fn+.` (and `,` `;` `.` `/`) | scroll history |
| Chat | `Fn+S` | back to the deck |
| Chat | `Fn+N` | new Session |
| Chat | `Fn+X` | cancel the running turn |
| Chat | `` ` `` | back to the deck |

`Aa`/Shift is a modifier and never produces a character; `Backspace` deletes one
complete UTF-8 character. Both rely on the physical press-edge queue in
`src/polling_keyboard.h`, unchanged.

### Build and flash

```powershell
powershell -ExecutionPolicy Bypass -File Y:\2026\cardputer\tools\harness-build.ps1            # build
powershell -ExecutionPolicy Bypass -File Y:\2026\cardputer\tools\harness-build.ps1 -Upload -Port COM13
```

**Do not build directly in `Y:\2026\cardputer`.** This SMB share reports
zero-length files and stale directory metadata to a remote process; PlatformIO's
Library Dependency Finder stats every header it walks and hangs at
"Scanning dependencies..." indefinitely. The script mirrors `src/`, `boards/` and
`platformio.ini` to `C:\Users\dloach\build\cardputer` and builds there (~45 s).
`Y:` stays the source of truth.

### Diagnostics

`src/usb_log.h` writes one-line logs over the USB CDC port (the `COM` port the
Cardputer exposes; note `Serial` in this firmware is UART0 and is not routed to
USB):

```powershell
pio device monitor -p COM13 -b 115200
```

Expect `harness ready heap=… canvas=1`, `[wifi] up ip=…`, `[ws] connect …`,
`[ws] state=2`, then `[rx] …` per frame type. `[ws] down drop=tcp connect failed`
means the Host is unreachable on 9101.

## Verification

Automated checks (no device needed) live in `harness-ws/test/ws-probe.mjs` and
use Node's built-in WebSocket client:

```powershell
node Y:\2026\cardputer\harness-ws\test\ws-probe.mjs list
node Y:\2026\cardputer\harness-ws\test\ws-probe.mjs open <sessionId>
node Y:\2026\cardputer\harness-ws\test\ws-probe.mjs prompt <sessionId> "Reply with exactly: OK"
node Y:\2026\cardputer\harness-ws\test\ws-probe.mjs cancel <sessionId> "Write 400 words about radio"
```

Results observed against this Host:

| Check | Result |
|---|---|
| Plugin active, `0.0.0.0:9101` listening, health `sessions:true stt:true` | pass |
| Wrong token rejected at handshake | pass |
| `list` returns title / preview / short id / updatedAt / running | pass |
| `create` makes a Session and opens it | pass |
| `prompt` commits the user message immediately (no polling) | pass |
| Assistant text arrives as `delta` before completion (first delta ≈1 s) | pass |
| Committed `message` + `status running:false` follow | pass |
| `cancel` accepted, turn settles, running false | pass |
| Cardputer connects from 192.168.0.50 and receives frames | pass |
| Build clean, flashed to COM13 | pass (RAM 46.0 %, Flash 71.3 %) |

Still needs human eyes/hands (see the handover checklist in the final report):
screen flicker and smoothness, key feel, voice capture, and a Desktop-typed
message appearing on the Cardputer.

## The retired ACP/HTTP relay

There is no rollback path any more, by request. What was retired:

- `harness-bridge/standalone.py` (port 9100) and its `dsh --profile cardputer`
  ACP child — both stopped. That ACP child was the process that used to fight
  the Desktop for a Session write handle.
- Its autostart entry: `CardputerBridge.lnk` was moved out of the Startup folder
  to `harness-bridge/CardputerBridge.autostart.disabled.lnk`. Moving it back
  (and dropping the `.disabled` suffix) restores the old behaviour.
- `run-standalone.ps1` and `standalone.py` are still on disk but are not
  started by anything.

`harness-bridge/` itself must stay: the plugin's voice input runs that venv's
Python (`harness-bridge/.venv`). The recognition engine and its model are chosen
by the plugin's `sttModel` config, not by that directory.

## Known limits

- **Voice (`Fn+V`) works end-to-end.** Capture → base64 `stt.chunk` streaming →
  WAV on the Host → offline speech recognition → text into the input line.
  Chinese phrases are recognised on hardware, including full sentences.

  **Recognition engine: sherpa-onnx SenseVoice (int8)**, which replaced Vosk
  small-cn because the small Vosk model garbled ordinary sentences. `stt.py`
  picks the engine from the model directory, so switching back is a config
  change only:

  - model dir containing `model.int8.onnx` → SenseVoice
    (`use_itn=True`, so output carries punctuation);
  - otherwise → Vosk `KaldiRecognizer`.

  Current config: `sttModel` points at
  `C:\Users\dloach\cardputer-models\sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17`
  (228 MB `model.int8.onnx` + `tokens.txt`), `sherpa_onnx` 2.0.1 in the venv, and
  one utterance transcribes in ~1.8 s including model load.

  **Silence gate:** SenseVoice invents a sentence for an empty clip (a 1 s silent
  WAV transcribes to "没。"), so a capture whose loudest sample never exceeds 600
  is aborted on the device (`stt.abort`, status `VOICE FAILED`) instead of being
  transcribed.

  **Diagnosis without speaking:** the plugin keeps every capture under
  `sttCaptureDir` (default `stt-capture/` next to the installed plugin, i.e.
  `C:\Users\dloach\cardputer-ws\stt-capture`), and
  `harness-ws/test/stt-analyze.py` reports signal stats and re-transcribes the
  same WAV with a chosen engine. A real 7.2 s capture measured:

  ```text
  peak 17719 (54.1% FS) · rms 382 (-38.7 dBFS) · dc -0.5 · clipping none · snr ~28 dB
  sensevoice: 今天天气怎么样？一直下雨，一直下雨，我觉得不太舒服。
  ```

  Usage: `python stt-analyze.py <wav> --stats-only` or
  `--engine sensevoice --model <dir>`.

  The earlier "I2S bit clock ~16× too slow / ES8311 is clock master" diagnosis
  was **wrong** and sent debugging down a dead end. A pure-read self-test
  (`voiceSelfTest`, no render/send/keyboard) measures the hardware I2S rate at
  ~16 kHz, and ES8311 reg `0x01=0xBA` (MCLK=BCLK) proves the codec is an I2S
  **slave** clocked by the S3, not the master. Direct `i2s_read()` on
  M5Unified's port 0 does **not** panic — it reads real samples.

  The real bottleneck was the WebSocket send. `sendFrame` used to mask and write
  the payload in 128-byte chunks (~80 `tcp_write` calls for a 10 KiB audio
  frame). Once the lwIP send buffer (~5.7 KiB) filled, each small write stalled
  on the peer's delayed ACK (~60 ms), so one frame blocked ~4.8 s. That starved
  the capture loop (the I2S DMA overflowed), which is what made the *loop*
  throughput look like ~767 Hz — misread as a slow hardware clock.

  Fix: mask the whole frame into a static buffer and send it in **one**
  `socket_.write()` so lwIP segments and pipelines it. The mask buffer is static,
  not `malloc`: the heap is fragmented enough that a second ~10 KiB contiguous
  block fails even with ~76 KiB total free. After the fix a single frame sends in
  ~24 ms and the Host returns `stt.ready` → `stt`. The `rate:16000` label matches
  the true steady-state rate; the self-test drains the DMA backlog before
  measuring so it no longer reports a backlog-inflated ~17.5 kHz.

  Voice is now press-to-talk: `Fn+V` starts, `ENT` (or `Fn+V` again) stops early,
  with a 20 s hard cap. The capture loop polls the key queue itself because
  `handlePress` only runs from `loop()`, which is blocked during capture. The RX
  DMA was deepened from 8×128 to 12×256 samples (~192 ms slack) so the per-block
  send overhead no longer overflows the FIFO: the loop now sustains ~15.9 kHz
  (≈ real time) instead of ~14.8 kHz, while the boot low-watermark stays ~24 KiB,
  clear of the ~19 KiB session-load panic floor.
- **`Fn+V` shows a live `MIC` level bar** while recording, computed from each
  captured buffer, so "is the microphone hearing anything" is answerable on the
  device without a serial cable.
- LAN only, no TLS; the token is a shared secret stored in NVS.
- Prompt acceptance is not completion; completion is the `message` + `status`
  pair.
- Tool-permission prompts raised by a turn are answered in the Desktop, not on
  the Cardputer.
- One Host plugin instance per port; a second install on 9101 reports a listen
  error instead of crashing.
- The panel backbuffer is 8 bpp (256 colours). This is deliberate: 16 bpp cost
  64.8 KiB of heap and session loads used to panic when the heap dipped under
  ~19 KiB. At 8 bpp the same board shows 97 KiB free / 92 KiB low-watermark.
