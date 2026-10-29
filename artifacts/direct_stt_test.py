#!/usr/bin/env python3
"""Direct STT probe against rockplatform.win voice stream from the PC,
bypassing the board entirely: anonymous legacy session, TTS WAV as PCM16."""

import json
import struct
import sys
import time
import wave

import websocket

WAV = sys.argv[1] if len(sys.argv) > 1 else r"artifacts\tts\stop.wav"
URL = "wss://rockplatform.win/api/v1/voice/stream"

with wave.open(WAV, "rb") as w:
    raw = w.readframes(w.getnframes())
    rate, channels = w.getframerate(), w.getnchannels()

samples = struct.unpack(f"<{len(raw) // 2}h", raw)
if channels == 2:
    samples = samples[0::2]
if rate != 16000:
    # nearest-neighbour resample to 16 kHz is plenty for an STT liveness probe
    ratio = rate / 16000
    count = int(len(samples) / ratio)
    samples = [samples[min(len(samples) - 1, int(i * ratio))] for i in range(count)]
pcm = struct.pack(f"<{len(samples)}h", *samples)

ws = websocket.create_connection(URL, timeout=15)
ws.send(json.dumps({
    "type": "start", "locale": "ru-RU", "sample_rate_hz": 16000,
    "recognizer_mode": "buffered_v1", "limit": 10,
}))
deadline = time.time() + 20
frames = []
while time.time() < deadline:
    try:
        ws.settimeout(5)
        op, data = ws.recv_data()
    except websocket.WebSocketTimeoutException:
        break
    if op != 1:
        continue
    text = data.decode("utf-8", errors="replace")
    frames.append(text)
    kind = json.loads(text).get("type")
    print(text[:300])
    if kind == "ready":
        for off in range(0, len(pcm), 3200):
            ws.send_binary(pcm[off:off + 3200])
            time.sleep(0.02)
        ws.send(json.dumps({"type": "commit"}))
    if kind in ("result", "error"):
        break
ws.close()
