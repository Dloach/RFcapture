# RF LAB

`RF LAB` is a separate boot mode next to `RF TOOLS`.  It does not replace the
capture/replay flow; it adds the analysis and interchange tools:

```text
launcher -> RF LAB
              SPECTRUM
              SUB FILES
              GENERATE
              MOD ANALYZE
              EDIT / SEND
```

Everything is OOK unless explicitly stated.  The CC1101 on CAP.CC1101 is a
single-band-at-a-time transceiver; RF LAB retunes and restores the band around
each operation.

## SPECTRUM

- 40-point sweep around the selected band:
  - 315 MHz: ±0.6 MHz
  - 433.92 MHz: ±1.8 MHz
  - 868.35 MHz: ±1.8 MHz
- top: instantaneous RSSI bars;
- bottom: 10-row waterfall;
- `S` hold/run, `F` band, `esc` back.

This is an RSSI spectrum, not an FFT.  The CC1101 provides RSSI, not I/Q, so a
wideband waterfall is built by stepping the synthesizer.

## SUB FILES

TF `/subghz/*.sub` list.

- `ENT` import selected:
  - `Protocol: RAW` -> raw pulse train;
  - `Protocol: Princeton` -> EV1527-style key frame (`Bit`/`Key`/`TE`);
  - `Protocol: CAME` / `Nice` -> 12-bit PWM frame;
- `E` export every saved signal to `/subghz/<name>.sub` as Flipper RAW;
- `S` rescan;
- `esc` back.

The parser intentionally supports a subset: it does not execute custom presets
or BinRAW blocks.  Unknown key protocols are rejected rather than guessed.

## GENERATE

Generate and store a fixed-code frame:

```text
EV1527 / PT2262 / CAME / NICE
CODE   hex
BITS   12 or 24
BASE   1T in microseconds
```

- `; .` field, `, /` adjust;
- `ENT` save to the permanent list;
- `S` send;
- `B` sends 256 consecutive EV1527/PT2262 codes.  **Only use this on your own
  equipment**; it is deliberately a small, manual sweep, not a radio scanner.

The generated waveform is stored as a normal `SavedSignal`, so it can be sent,
edited, exported, or bound to a quick key through the existing RF tools.

## MOD ANALYZE

Samples the CC1101 RSSI for ~240 ms and classifies the envelope:

- mostly short high bursts -> `OOK / BURST`;
- continuously high, few edges -> `2-FSK/MSK?`;
- weak -> `WEAK / NOISE`.

It also prints the last decoded frame (`EV1527`, `PT2262`, or a supplementary
CAME/Nice 12-bit PWM classifier).

This is **not** an FSK/MSK demodulator.  The hardware path used by RF TOOLS is
OOK asynchronous GDO0; true FSK/MSK decoding needs a different demodulator and
is therefore reported as a heuristic, not a decoded payload.

## EDIT / SEND

Select a saved signal, then edit:

- `PULSE` index;
- that pulse's `WIDTH`;
- `BASE` 1T;
- `REPEAT` count (1–40);
- `OFFSET` −500…+500 kHz.

`ENT` saves the edited pulse/base, `S` transmits with repeat/offset, `X` exports
one `.sub`.

Offsets and repeat count are session overrides; the saved `.sub` export is the
base signal.

## References

- [Flipper .sub format](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/documentation/file_formats/SubGhzFileFormats.md)
- [flipperzero-bruteforce protocol examples](https://deepwiki.com/tobiabocchi/flipperzero-bruteforce/6-protocol-examples)
- [unigeek brand decoders / CAME timing discussion](https://github.com/lshaf/unigeek/pull/41)
- [CC1101 datasheet](https://www.ti.com/lit/ds/symlink/cc1101.pdf)

## Limits

- Imported RAW trains are truncated to `RF_STORED_PULSES` (160 pulses).
- `.sub` export is always RAW, never a brand-specific key file.
- CAME/Nice generation uses fixed 1T/2T/3T approximations; verify against your
  own receiver before relying on it.
- The spectrum is RSSI-stepped, not a true FFT.
- Modulation classification is an envelope heuristic only.
