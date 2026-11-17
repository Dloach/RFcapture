# HARNESS verification

Date: 2026-11-17

## Host plugin

- profile bundle: `@local/dsh-cardputer-ws`
- endpoint: `ws://192.168.0.88:9101/cardputer?token=cardputer-dev`
- health:
  ```json
  {"ok":true,"name":"dsh-cardputer-ws","port":9101,"path":"/cardputer","clients":0,"sessions":true,"stt":true}
  ```

WebSocket probe passed:

- `hello`
- `ping` -> `pong`
- `sessions` -> title / preview / shortId list
- `open` -> `status` + `snapshot` for the same Session
- `create` -> `session-840da33f-534a-444e-a140-00f8f7608a0f`
- `prompt` -> assistant `message` = `OK` and `delta` = `OK`

STT probe passed with a 16 kHz mono WAV:

```text
Hello Car Butter test voice input.
```

## Firmware

`pio run -e cardputer-adv` succeeds with the WebSocket client, HARNESS deck,
message preview, voice stream, and physical-key event queue.

## Pending

USB port COM13 is absent at the time of this check, so the current WebSocket
HARNESS build has not been reflashed in this round. Final on-board verification:

1. connect Cardputer USB;
2. `pio run -e cardputer-adv -t upload --upload-port COM13`;
3. open HARNESS, confirm session deck;
4. send an English prompt and verify Desktop + Cardputer receive the same turn;
5. run `Fn+V` and verify the recognized text arrives in the composer/session.

Until then the goal remains active.
