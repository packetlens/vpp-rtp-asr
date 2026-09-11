"""End-to-end: replay a G.711 µ-law RTP pcap through VPP with the rtp-asr
plugin loaded + a real Moonshine model, capture JSON-UDP transcripts, assert
the transcript contains expected words from the test wav's ground truth.

Skipped unless a Moonshine model is fetched under
`models/sherpa-onnx-moonshine-tiny-en-int8/` (see models/fetch.sh).
"""

import json
import os
import socket
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(__file__))
from conftest import MODEL_DIR, EMITTER_HOST, EMITTER_PORT, model_available  # noqa: E402
from rtp_synth import wav_to_ulaw, build_rtp_pcap  # noqa: E402


TEST_WAV = os.path.join(MODEL_DIR, "test_wavs", "8k.wav")

pytestmark = pytest.mark.skipif(
    not model_available(), reason="Moonshine model not present — run models/fetch.sh"
)


def vppctl(sock, *args):
    r = subprocess.run(
        ["vppctl", "-s", sock, *args],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert r.returncode == 0, f"vppctl {args} rc={r.returncode} err={r.stderr}"
    return r.stdout


@pytest.mark.integration
def test_sherpa_loaded(vpp_e2e):
    out = vppctl(vpp_e2e["cli_sock"], "show", "rtp-asr", "stats")
    assert "sherpa loaded:      yes" in out, out


@pytest.mark.integration
@pytest.mark.slow
def test_replay_transcribes(vpp_e2e):
    """Inject a G.711 µ-law RTP pcap of a known wav via VPP packet-generator,
    assert a JSON-UDP transcript arrives containing a ground-truth word."""
    # 1. Build the pcap from the bundled test wav.
    pcap = str(vpp_e2e["tmp_path"] / "rtp.pcap")
    _, ulaw = wav_to_ulaw(TEST_WAV)
    n_packets = build_rtp_pcap(ulaw, pcap)
    assert n_packets > 50  # ~4 seconds of audio at 20ms/packet

    # 2. UDP sink on the emitter port.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((EMITTER_HOST, EMITTER_PORT))
    sock.settimeout(15.0)

    try:
        # 3. Configure VPP: pg interface + feature arc + packet-generator stream.
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
        # VPP 24.x: enable-stream with a name arg is silently ignored; use no-arg form to enable all.
        vppctl(cli, "packet-generator", "enable-stream")

        # Wait for the stream to finish: rate=50 pps, ~242 pkts → ~5s.
        time.sleep(7)

        # 4. Wait for transcripts. Moonshine-tiny int8 on CPU takes <1s per 4s chunk.
        transcripts = []
        deadline = time.time() + 20.0
        while time.time() < deadline and len(transcripts) < 1:
            try:
                data, _ = sock.recvfrom(4096)
                record = json.loads(data.decode("utf-8"))
                transcripts.append(record)
            except socket.timeout:
                break
            except json.JSONDecodeError as e:
                pytest.fail(f"non-JSON UDP payload: {e}: {data!r}")

        stats2 = vppctl(cli, "show", "rtp-asr", "stats")
        assert transcripts, (
            f"no JSON-UDP transcript received within 20s\nstats:\n{stats2}"
        )

        full_text = " ".join(r.get("text", "") for r in transcripts).upper()
        # Expected words from test_wavs/trans.txt for 8k.wav:
        #   "YET THESE THOUGHTS AFFECTED HESTER PRYNNE LESS WITH HOPE THAN APPREHENSION"
        # Tiny int8 model on 8kHz upsampled audio won't be perfect — check loose.
        found_any = any(
            word in full_text
            for word in ("HESTER", "THOUGHTS", "HOPE", "APPREHENSION")
        )
        assert found_any, (
            "transcripts received but no expected word hit: "
            f"{[r.get('text') for r in transcripts]!r}"
        )
    finally:
        sock.close()
