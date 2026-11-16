#!/usr/bin/env python3
"""Offline diagnosis of a captured Cardputer STT WAV.

Two jobs, both runnable on the exact audio the device sent (the Host plugin
keeps every capture under harness-ws/plugin/stt-capture/):

  1. Signal stats — peak / RMS / DC offset / clipping / a rough speech-vs-noise
     SNR.  Answers "is the mic gain actually too low?" with numbers, not guesses.
  2. Engine shootout — transcribe the SAME wav with Vosk (small or full cn) or
     sherpa-onnx (SenseVoice / Paraformer) so the model contribution is measured
     independently of the audio.

Usage:
  python stt-analyze.py <wav>                      # stats + Vosk small-cn
  python stt-analyze.py <wav> --stats-only         # stats, no ASR
  python stt-analyze.py <wav> --engine sensevoice --model <dir>
  python stt-analyze.py <wav> --engine vosk --model <path-to-vosk-model-cn-0.22>

Run with the bridge venv python (has vosk; pip install sherpa-onnx for the
SenseVoice/Paraformer engines).
"""
import argparse
import array
import math
import os
import sys
import wave

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except AttributeError:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_VOSK = os.path.normpath(
    os.path.join(HERE, "..", "..", "harness-bridge", "models", "vosk-model-small-cn-0.22")
)


def load_mono(path):
    with wave.open(path, "rb") as wf:
        if wf.getnchannels() != 1 or wf.getsampwidth() != 2:
            raise SystemExit(f"expected mono 16-bit WAV, got ch={wf.getnchannels()} width={wf.getsampwidth()}")
        rate = wf.getframerate()
        data = wf.readframes(wf.getnframes())
    samples = array.array("h")
    samples.frombytes(data)
    if sys.byteorder != "little":
        samples.byteswap()
    return samples, rate


def stats(samples, rate):
    n = len(samples)
    if n == 0:
        print("empty wav")
        return
    peak = max(abs(x) for x in samples)
    mean = sum(samples) / n
    rms = math.sqrt(sum((x - mean) ** 2 for x in samples) / n)
    clip = sum(1 for x in samples if abs(x) >= 32000)
    # Rough SNR: 20 ms frames, compare the loudest 10% (speech) to the quietest
    # 10% (noise floor).  Not a lab measurement, but enough to see if the mic is
    # whisper-quiet or healthy.
    fl = max(1, rate // 50)
    energies = []
    for i in range(0, n - fl, fl):
        seg = samples[i:i + fl]
        energies.append(sum(x * x for x in seg) / fl)
    energies.sort()
    k = max(1, len(energies) // 10)
    noise = sum(energies[:k]) / k
    speech = sum(energies[-k:]) / k
    snr = 10 * math.log10(speech / noise) if noise > 0 else float("inf")
    print(f"duration   : {n / rate:.2f} s  ({n} samples @ {rate} Hz)")
    print(f"peak       : {peak} / 32767  ({100 * peak / 32767:.1f} % full scale)")
    print(f"rms        : {rms:.0f}  ({20 * math.log10(rms / 32767) if rms > 0 else -999:.1f} dBFS)")
    print(f"dc offset  : {mean:.1f}")
    print(f"clipping   : {clip} samples >= 32000  ({100 * clip / n:.2f} %)")
    print(f"snr(rough) : {snr:.1f} dB  (loudest 10% vs quietest 10% frames)")
    if peak < 6000:
        print(">> peak is low: analog mic gain likely insufficient")
    if clip / n > 0.01:
        print(">> heavy clipping: gain too high or soft-limiter engaging")


def run_vosk(samples, rate, model):
    from vosk import KaldiRecognizer, Model
    m = Model(model)
    rec = KaldiRecognizer(m, rate)
    rec.SetWords(False)
    raw = samples.tobytes()
    text = []
    step = 8000
    for i in range(0, len(raw), step):
        if rec.AcceptWaveform(raw[i:i + step]):
            import json
            part = json.loads(rec.Result()).get("text", "")
            if part:
                text.append(part)
    import json
    part = json.loads(rec.FinalResult()).get("text", "")
    if part:
        text.append(part)
    return " ".join(text).strip()


def run_sherpa(samples, rate, model, kind):
    import sherpa_onnx
    if kind == "sensevoice":
        recognizer = sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=os.path.join(model, "model.int8.onnx"),
            tokens=os.path.join(model, "tokens.txt"),
            use_itn=True,
            language="zh",
            num_threads=4,
        )
    else:  # paraformer
        recognizer = sherpa_onnx.OfflineRecognizer.from_paraformer(
            paraformer=os.path.join(model, "model.int8.onnx"),
            tokens=os.path.join(model, "tokens.txt"),
            num_threads=4,
        )
    stream = recognizer.create_stream()
    stream.accept_waveform(rate, [x / 32768.0 for x in samples])
    recognizer.decode_stream(stream)
    return stream.result.text.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--engine", choices=["vosk", "sensevoice", "paraformer"], default="vosk")
    ap.add_argument("--model", default=DEFAULT_VOSK)
    ap.add_argument("--stats-only", action="store_true")
    args = ap.parse_args()

    samples, rate = load_mono(args.wav)
    print(f"=== {os.path.basename(args.wav)} ===")
    stats(samples, rate)
    if args.stats_only:
        return
    print(f"--- {args.engine} ({os.path.basename(args.model)}) ---")
    if args.engine == "vosk":
        text = run_vosk(samples, rate, args.model)
    else:
        text = run_sherpa(samples, rate, args.model, args.engine)
    print(f"text: {text}")


if __name__ == "__main__":
    main()