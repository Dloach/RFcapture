# DRUM

8-track, 16-step synthetic drum machine on the Cardputer ADV.

- 16 kHz mono, synthesized kick/snare/clap/closed hat/open hat/tom/low tom/perc;
- no samples and no PSRAM;
- pattern, BPM and mute state persisted in NVS `drum`;
- BPM 60–220, default 120.

## Controls

| Key | Action |
|---|---|
| `;` / `.` | move step |
| `,` / `/` | move track |
| `ENT` | toggle current hit |
| `P` | play / stop |
| `=` / `-` | BPM +5 / -5 |
| `M` | mute current track |
| `esc` | save and return to launcher |

## Audio path

`M5.Speaker` (ES8311) plays one synthesized step at a time. Each step is rendered
into a 16 KB mono buffer, so the feature fits the 8 MB/no-PSRAM board. Drum tails
are intentionally cut at the next step; this is a step sequencer, not a sampler.
