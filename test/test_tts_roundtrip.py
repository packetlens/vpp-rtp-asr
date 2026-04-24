"""TTS roundtrip: synthesize a known phrase with espeak-ng, encode as G.711
µ-law RTP, inject through VPP rtp-asr, verify the transcript contains the
expected content words.

Purpose: confirm the full pipeline works for synthesized speech, not just a
pre-recorded fixture.  Accuracy bar is intentionally loose — espeak-ng
produces robotic synthetic voice, and Moonshine-tiny-int8 on 8 kHz G.711 will
make some errors.  We check that at least half the distinctive content words
survive the round-trip.

Skipped unless:
  - espeak-ng is present on PATH
  - Moonshine model is present (models/sherpa-onnx-moonshine-tiny-en-int8/)
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(__file__))
from conftest import EMITTER_HOST, EMITTER_PORT, model_available  # noqa: E402
from rtp_synth import wav_to_ulaw, build_rtp_pcap  # noqa: E402

# ------------------------------------------------------------------
# Test parameters
# ------------------------------------------------------------------

# Phrase chosen via calibration against espeak-ng + 8 kHz G.711 + Moonshine-tiny-int8:
#   "the cat sat on the mat"  →  "The cat sat on the mat."  (near-perfect)
# Short CVC rhyming words are phonetically unambiguous for espeak-ng; the
# Moonshine model reproduces them reliably even at 8 kHz telephony quality.
ROUNDTRIP_TEXT = "the cat sat on the mat and the bat flew past the fat rat"

# Distinctive content words we expect to survive the round-trip.
# A word is "found" if it appears anywhere (case-insensitive) in the output.
EXPECTED_WORDS = ["cat", "sat", "mat", "bat", "rat"]

# Require at least this many expected words to match.
MIN_WORD_HITS = 3

# ------------------------------------------------------------------
# Skip conditions
# ------------------------------------------------------------------

pytestmark = pytest.mark.skipif(
    not model_available() or shutil.which("espeak-ng") is None,
    reason="Moonshine model or espeak-ng not available",
)


# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------

def synthesize_wav(text: str, wav_path: str) -> None:
    """Synthesize *text* to *wav_path* as 8 kHz 16-bit mono WAV.

    espeak-ng outputs at 22050 Hz; we pipe through ffmpeg for a proper
    anti-aliased downsample.  rtp_synth.wav_to_ulaw does a simple linear
    interpolation that aliases heavily at this conversion ratio, so we do the
    resampling here instead and hand off an already-8kHz WAV.
    """
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
        tmp = tf.name
    try:
        subprocess.run(
            ["espeak-ng", "-v", "en-us", "-s", "130", "-w", tmp, text],
            check=True, timeout=30,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
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
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert r.returncode == 0, f"vppctl {args} rc={r.returncode} err={r.stderr}"
    return r.stdout


def inject_pcap_and_collect(vpp_e2e, pcap: str, n_packets: int) -> list[dict]:
    """Wire up a pg0 interface, replay *pcap*, return received transcript records."""
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
    # VPP 24.x: enable-stream with a stream name is silently ignored;
    # the no-arg form enables all streams.
    vppctl(cli, "packet-generator", "enable-stream")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((EMITTER_HOST, EMITTER_PORT))
    sock.settimeout(5.0)

    # Wait for stream to finish (rate 50 pps) then drain any late arrivals.
    stream_duration = n_packets / 50.0
    time.sleep(stream_duration + 3.0)

    transcripts = []
    deadline = time.time() + 15.0
    try:
        while time.time() < deadline:
            try:
                data, _ = sock.recvfrom(4096)
                record = json.loads(data.decode("utf-8"))
                transcripts.append(record)
            except socket.timeout:
                break
            except json.JSONDecodeError as e:
                pytest.fail(f"non-JSON UDP payload: {e}: {data!r}")
    finally:
        sock.close()

    return transcripts


# ------------------------------------------------------------------
# Test
# ------------------------------------------------------------------

@pytest.mark.integration
@pytest.mark.slow
def test_tts_roundtrip(vpp_e2e):
    """Synthesize ROUNDTRIP_TEXT → G.711 RTP pcap → VPP → transcript.
    At least MIN_WORD_HITS of EXPECTED_WORDS must appear in the output."""
    # 1. TTS → wav
    wav_path = str(vpp_e2e["tmp_path"] / "tts.wav")
    synthesize_wav(ROUNDTRIP_TEXT, wav_path)

    # 2. wav → 8 kHz µ-law → RTP pcap
    pcap = str(vpp_e2e["tmp_path"] / "rtp.pcap")
    _, ulaw = wav_to_ulaw(wav_path)
    n_packets = build_rtp_pcap(ulaw, pcap)
    # espeak-ng should produce at least 1 s of speech (~50 packets)
    assert n_packets >= 50, f"TTS audio too short: {n_packets} RTP packets"

    # 3. Inject through VPP and collect transcripts
    transcripts = inject_pcap_and_collect(vpp_e2e, pcap, n_packets)

    stats = vppctl(vpp_e2e["cli_sock"], "show", "rtp-asr", "stats")
    assert transcripts, (
        f"no JSON-UDP transcript received\n"
        f"input text: {ROUNDTRIP_TEXT!r}\n"
        f"stats:\n{stats}"
    )

    # 4. Verify word overlap
    full_text = " ".join(r.get("text", "") for r in transcripts).lower()
    hits = [w for w in EXPECTED_WORDS if w in full_text]

    assert len(hits) >= MIN_WORD_HITS, (
        f"only {len(hits)}/{len(EXPECTED_WORDS)} expected words found "
        f"(need {MIN_WORD_HITS})\n"
        f"input:      {ROUNDTRIP_TEXT!r}\n"
        f"transcript: {full_text!r}\n"
        f"found:      {hits}\n"
        f"missing:    {[w for w in EXPECTED_WORDS if w not in full_text]}"
    )
