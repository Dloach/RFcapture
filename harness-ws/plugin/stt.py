#!/usr/bin/env python3
"""Offline 16 kHz mono WAV -> one line of UTF-8 text using Vosk.

Invoked by the dsh-cardputer-ws Host plugin as:
    python stt.py <model-dir> <wav-path>
"""
import json
import sys
import wave

from vosk import KaldiRecognizer, Model

# On Windows, a redirected stdout uses the ANSI code page (GBK here), so Chinese
# text would reach the Host as GBK bytes and decode as U+FFFD.  Force UTF-8 on
# the pipe explicitly.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except AttributeError:
    pass


def main() -> int:
    if len(sys.argv) < 3:
        return 2
    model_path, wav_path = sys.argv[1], sys.argv[2]
    with wave.open(wav_path, "rb") as wf:
        if wf.getnchannels() != 1 or wf.getsampwidth() != 2:
            return 3
        rate = wf.getframerate()
        model = Model(model_path)
        rec = KaldiRecognizer(model, rate)
        rec.SetWords(False)
        text = []
        while True:
            data = wf.readframes(4000)
            if not data:
                break
            if rec.AcceptWaveform(data):
                part = json.loads(rec.Result()).get("text", "")
                if part:
                    text.append(part)
        part = json.loads(rec.FinalResult()).get("text", "")
        if part:
            text.append(part)
    print(" ".join(text).strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
