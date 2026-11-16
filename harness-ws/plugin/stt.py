#!/usr/bin/env python3
"""Offline 16 kHz mono WAV -> one line of UTF-8 text.

The engine is chosen from the model directory so switching is a config change,
not a code change:
  - model.int8.onnx present -> sherpa-onnx SenseVoice (Chinese, high accuracy,
    robust to quiet/low-SNR mic audio)
  - otherwise               -> Vosk KaldiRecognizer (legacy small-cn model)

Invoked by the dsh-cardputer-ws Host plugin as:
    python stt.py <model-dir> <wav-path>
"""
import array
import json
import os
import sys
import wave

# On Windows, a redirected stdout uses the ANSI code page (GBK here), so Chinese
# text would reach the Host as GBK bytes and decode as U+FFFD.  Force UTF-8 on
# the pipe explicitly.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except AttributeError:
    pass


def read_wav(path):
    with wave.open(path, "rb") as wf:
        if wf.getnchannels() != 1 or wf.getsampwidth() != 2:
            return None, None
        rate = wf.getframerate()
        data = wf.readframes(wf.getnframes())
    return data, rate


def sensevoice(model_dir, data, rate):
    import sherpa_onnx

    rec = sherpa_onnx.OfflineRecognizer.from_sense_voice(
        model=os.path.join(model_dir, "model.int8.onnx"),
        tokens=os.path.join(model_dir, "tokens.txt"),
        use_itn=True,
        language="zh",
        num_threads=4,
    )
    samples = array.array("h")
    samples.frombytes(data)
    if sys.byteorder != "little":
        samples.byteswap()
    stream = rec.create_stream()
    stream.accept_waveform(rate, [x / 32768.0 for x in samples])
    rec.decode_stream(stream)
    return stream.result.text.strip()


def vosk(model_dir, data, rate):
    from vosk import KaldiRecognizer, Model

    rec = KaldiRecognizer(Model(model_dir), rate)
    rec.SetWords(False)
    text = []
    for i in range(0, len(data), 8000):
        if rec.AcceptWaveform(data[i:i + 8000]):
            part = json.loads(rec.Result()).get("text", "")
            if part:
                text.append(part)
    part = json.loads(rec.FinalResult()).get("text", "")
    if part:
        text.append(part)
    return " ".join(text).strip()


def main() -> int:
    if len(sys.argv) < 3:
        return 2
    model_dir, wav_path = sys.argv[1], sys.argv[2]
    data, rate = read_wav(wav_path)
    if data is None:
        return 3
    if os.path.exists(os.path.join(model_dir, "model.int8.onnx")):
        text = sensevoice(model_dir, data, rate)
    else:
        text = vosk(model_dir, data, rate)
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())