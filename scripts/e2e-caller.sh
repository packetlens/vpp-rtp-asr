#!/usr/bin/env bash
# Synthesize speech with Piper TTS, downsample to 8 kHz, stream as G.711 µ-law RTP
# to VPP's data interface. Runs inside the caller service container.
set -euo pipefail

PIPER_MODEL="${PIPER_MODEL:-/models/piper/en_US-lessac-medium.onnx}"
TEXT="the quick brown fox jumps over the lazy dog"
VPP_RTP_HOST="${VPP_RTP_HOST:-172.21.0.10}"
VPP_RTP_PORT="${VPP_RTP_PORT:-16400}"
LOOPS="${LOOPS:-3}"  # 3× pangram ≈ 12s total — guarantees ≥3 Moonshine 2s chunks

# ── Piper TTS → 22050 Hz WAV ─────────────────────────────────────────────────
echo "[caller] Synthesizing: ${TEXT}"
python3 - <<PYEOF
import wave, os, sys
from piper import PiperVoice
model = os.environ.get("PIPER_MODEL", "/models/piper/en_US-lessac-medium.onnx")
voice = PiperVoice.load(model, use_cuda=False)
with wave.open("/tmp/tts_22k.wav", "w") as wf:
    voice.synthesize_wav("${TEXT}", wf)
print("[caller] Piper TTS done", flush=True)
PYEOF

# ── Anti-aliased downsample to 8 kHz ─────────────────────────────────────────
ffmpeg -y -i /tmp/tts_22k.wav \
  -ar 8000 -ac 1 -sample_fmt s16 /tmp/tts_8k.wav \
  -loglevel error
echo "[caller] Downsampled to 8 kHz ($(ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/tts_8k.wav)s)"

# ── Stream as G.711 µ-law RTP ─────────────────────────────────────────────────
# -re         : read at real-time rate (50 pps for 8kHz/160-sample packets)
# -acodec pcm_mulaw / PT=0 / dst port in VPP's accepted range (16384-32768)
for i in $(seq 1 "$LOOPS"); do
  echo "[caller] RTP loop ${i}/${LOOPS} → ${VPP_RTP_HOST}:${VPP_RTP_PORT}"
  ffmpeg -re -i /tmp/tts_8k.wav \
    -vn -ar 8000 -ac 1 -acodec pcm_mulaw \
    -f rtp "rtp://${VPP_RTP_HOST}:${VPP_RTP_PORT}" \
    -loglevel error
  sleep 0.3
done

echo "[caller] Done — all RTP sent"
