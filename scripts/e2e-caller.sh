#!/usr/bin/env bash
# Synthesize speech with Piper TTS, downsample to 8 kHz, concatenate multiple
# repetitions into a single long WAV, then stream as ONE continuous G.711 µ-law
# RTP session.  A single ffmpeg session = single SSRC = single VPP session with
# enough audio for Moonshine to produce multiple 2-second transcript segments.
set -euo pipefail

PIPER_MODEL="${PIPER_MODEL:-/models/piper/en_US-lessac-medium.onnx}"
TEXT="the quick brown fox jumps over the lazy dog"
VPP_RTP_HOST="${VPP_RTP_HOST:-10.66.23.20}"
VPP_RTP_PORT="${VPP_RTP_PORT:-16400}"
REPEATS="${LOOPS:-6}"  # concat this many pangram repetitions → one long stream

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
DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/tts_8k.wav)
echo "[caller] Downsampled to 8 kHz (${DUR}s per repetition, ${REPEATS} repeats)"

# ── Concatenate REPEATS copies into a single long WAV ─────────────────────────
# Use ffmpeg concat to produce one long stream so VPP sees a single RTP session
# with a single SSRC and enough audio for multiple Moonshine 2s decode chunks.
CONCAT_INPUT=""
for i in $(seq 1 "$REPEATS"); do
  CONCAT_INPUT="${CONCAT_INPUT}-i /tmp/tts_8k.wav "
done
FILTER="concat=n=${REPEATS}:v=0:a=1"
ffmpeg -y $CONCAT_INPUT -filter_complex "$FILTER" /tmp/tts_long.wav \
  -loglevel error
TOTAL=$(ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/tts_long.wav)
echo "[caller] Concatenated ${REPEATS}× audio: ${TOTAL}s total"

# ── Stream as a SINGLE G.711 µ-law RTP session ───────────────────────────────
echo "[caller] Streaming ${TOTAL}s → ${VPP_RTP_HOST}:${VPP_RTP_PORT}"

# Capture TX stats before sending
TX_BEFORE=$(ip -s link show eth0 | awk '/TX:/{getline; print $2}')

ffmpeg -re -i /tmp/tts_long.wav \
  -vn -af "asetnsamples=n=160:p=1" \
  -ar 8000 -ac 1 -acodec pcm_mulaw \
  -f rtp "rtp://${VPP_RTP_HOST}:${VPP_RTP_PORT}" \
  -loglevel error

TX_AFTER=$(ip -s link show eth0 | awk '/TX:/{getline; print $2}')
echo "[caller] TX packets on eth0: before=${TX_BEFORE} after=${TX_AFTER} sent=$((TX_AFTER - TX_BEFORE))"

echo "[caller] Done — RTP stream complete; holding 60s for VPP transcription"
sleep 60
