"""TTS roundtrip: synthesize a complex English sentence with piper (neural TTS),
encode as G.711 µ-law RTP, inject through VPP rtp-asr, verify the transcript
contains the expected content words.

Why piper instead of espeak-ng:
  espeak-ng formant synthesis is acoustically far from natural speech; Moonshine
  was trained on natural speech and produces heavily garbled output for espeak
  ("the quick brown fox" → "the greek brown-fark jump").  Piper neural TTS
  produces near-natural waveforms that sit in-distribution for Moonshine even
  after 8 kHz G.711 telephony encoding (calibrated: ~86% word accuracy on the
  fox/dog pangram, vs ~55% with espeak-ng).

Accuracy bar: ≥ 5 of 8 distinctive content words must appear in the transcript
(case-insensitive substring match).  This is deliberately below the ~86% measured
so a single word miss or slight model variance does not flip a pass to a fail.

Skipped unless:
  - piper-tts Python package is installed (pip install piper-tts)
  - Piper voice model present: models/piper/en_US-lessac-medium.onnx
  - Moonshine model present:   models/sherpa-onnx-moonshine-tiny-en-int8/
  - ffmpeg binary on PATH (for anti-aliased downsample to 8 kHz)
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import time
import wave

import pytest

sys.path.insert(0, os.path.dirname(__file__))
from conftest import (  # noqa: E402
    EMITTER_HOST, EMITTER_PORT,
    PIPER_MODEL,
    model_available, piper_available,
)
from rtp_synth import wav_to_ulaw, build_rtp_pcap  # noqa: E402

# ---------------------------------------------------------------------------
# Test sentences and expected content words
# ---------------------------------------------------------------------------

# Classic pangram — phonetically diverse, widely represented in ASR training data.
ROUNDTRIP_TEXT = "the quick brown fox jumps over the lazy dog"

# Content words we expect to survive the round-trip through 8 kHz G.711 +
# Moonshine-tiny-int8.  "the" / "over" / "a" omitted — too short/common to
# distinguish a correct transcription from noise.
# Use root/stem forms so morphological variants match ("jumped" satisfies "jump").
EXPECTED_WORDS = ["quick", "brown", "fox", "jump", "lazy", "dog", "over"]

# Require ≥ this many hits.  Calibrated measurement: ~86% (≈ 6/7) in Docker.
# We require 5/7 to leave headroom for model variance across hardware.
MIN_WORD_HITS = 5

# ---------------------------------------------------------------------------
# Skip condition
# ---------------------------------------------------------------------------

pytestmark = pytest.mark.skipif(
    not model_available() or not piper_available() or not shutil.which("ffmpeg"),
    reason="piper model, Moonshine model, or ffmpeg not available — run models/fetch.sh",
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def synthesize_wav(text: str, wav_path: str) -> None:
    """Synthesize *text* to *wav_path* as 8 kHz 16-bit mono WAV.

    Pipeline: piper neural TTS (22050 Hz) → ffmpeg anti-aliased downsample.
    Using piper's Python API avoids an extra subprocess; ffmpeg handles the
    resampling (rtp_synth's simple linear interpolation aliases heavily at the
    22050→8000 Hz conversion ratio).
    """
    import tempfile
    from piper import PiperVoice

    voice = PiperVoice.load(PIPER_MODEL, use_cuda=False)

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
        tmp = tf.name
    try:
        with wave.open(tmp, "w") as wf:
            # synthesize_wav writes PCM + sets WAV header (channels/rate/width)
            voice.synthesize_wav(text, wf)
        subprocess.run(
            ["ffmpeg", "-y", "-i", tmp,
             "-ar", "8000", "-ac", "1", "-sample_fmt", "s16", wav_path],
            check=True, timeout=30,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    finally:
        os.unlink(tmp)


def vppctl(sock, *args):
    r = subprocess.run(
        ["vppctl", "-s", sock, *args],
        capture_output=True, text=True, timeout=10,
    )
    assert r.returncode == 0, f"vppctl {args} rc={r.returncode} err={r.stderr}"
    return r.stdout


def inject_and_collect(vpp_e2e, pcap: str, n_packets: int) -> list[dict]:
    """Wire pg0, replay pcap at 50 pps, collect JSON-UDP transcripts."""
    cli = vpp_e2e["cli_sock"]
    vppctl(cli, "create", "packet-generator", "interface", "pg0")
    vppctl(cli, "set", "interface", "state", "pg0", "up")
    vppctl(cli, "set", "interface", "ip", "address", "pg0", "10.0.0.1/24")
    vppctl(cli, "set", "interface", "rtp-asr", "pg0", "enable")
    vppctl(
        cli,
        "packet-generator", "new",
        "name", "stream0",
        "interface", "pg0",
        "node", "ethernet-input",
        "limit", str(n_packets),
        "rate", "50",
        "pcap", pcap,
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((EMITTER_HOST, EMITTER_PORT))
    sock.settimeout(5.0)

    # VPP 24.x: enable-stream with a name arg is silently ignored; no-arg enables all.
    vppctl(cli, "packet-generator", "enable-stream")

    stream_secs = n_packets / 50.0
    time.sleep(stream_secs + 3.0)

    transcripts = []
    deadline = time.time() + 15.0
    try:
        while time.time() < deadline:
            try:
                data, _ = sock.recvfrom(4096)
                transcripts.append(json.loads(data.decode("utf-8")))
            except socket.timeout:
                break
            except json.JSONDecodeError as e:
                pytest.fail(f"non-JSON UDP payload: {e}: {data!r}")
    finally:
        sock.close()

    return transcripts


def word_hits(expected_words: list[str], transcript: str) -> list[str]:
    """Return the expected words found (as substrings) in *transcript*."""
    t = transcript.lower()
    return [w for w in expected_words if w in t]


# ---------------------------------------------------------------------------
# Test
# ---------------------------------------------------------------------------

@pytest.mark.integration
@pytest.mark.slow
def test_tts_roundtrip(vpp_e2e):
    """Piper neural TTS → G.711 RTP pcap → VPP rtp-asr → Moonshine transcript.

    Verifies the full pipeline against a complex English sentence.  Expected
    transcript accuracy: ≥ 5/7 content words from ROUNDTRIP_TEXT.
    """
    tmp = vpp_e2e["tmp_path"]

    # 1. TTS → 8 kHz WAV
    wav_path = str(tmp / "tts.wav")
    synthesize_wav(ROUNDTRIP_TEXT, wav_path)

    # 2. WAV → G.711 µ-law RTP pcap
    pcap = str(tmp / "rtp.pcap")
    _, ulaw = wav_to_ulaw(wav_path)
    n_packets = build_rtp_pcap(ulaw, pcap)
    assert n_packets >= 50, f"TTS audio too short: {n_packets} packets"

    # 3. Inject through VPP and collect
    transcripts = inject_and_collect(vpp_e2e, pcap, n_packets)

    stats = vppctl(vpp_e2e["cli_sock"], "show", "rtp-asr", "stats")
    assert transcripts, (
        f"no transcript received\ninput: {ROUNDTRIP_TEXT!r}\nstats:\n{stats}"
    )

    # 4. Verify content word accuracy
    full_text = " ".join(r.get("text", "") for r in transcripts)
    hits = word_hits(EXPECTED_WORDS, full_text)

    assert len(hits) >= MIN_WORD_HITS, (
        f"only {len(hits)}/{len(EXPECTED_WORDS)} expected words found "
        f"(need {MIN_WORD_HITS})\n"
        f"input:      {ROUNDTRIP_TEXT!r}\n"
        f"transcript: {full_text!r}\n"
        f"found:      {hits}\n"
        f"missing:    {[w for w in EXPECTED_WORDS if w not in full_text.lower()]}"
    )
