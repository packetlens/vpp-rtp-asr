"""End-to-end: replay a G.711 µ-law RTP pcap through VPP with the rtp-asr
plugin loaded + a real Moonshine model, capture JSON-UDP transcripts, assert
the transcript contains expected words from the test wav's ground truth.

Skipped unless a Moonshine model is fetched under
`models/sherpa-onnx-moonshine-tiny-en-int8/` (see models/fetch.sh).
"""

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(__file__))
from rtp_synth import wav_to_ulaw, build_rtp_pcap  # noqa: E402


SRC = os.environ.get("SRC_DIR", "/src")
MODEL_DIR = os.path.join(SRC, "models", "sherpa-onnx-moonshine-tiny-en-int8")
TEST_WAV = os.path.join(MODEL_DIR, "test_wavs", "8k.wav")
EMITTER_HOST = "127.0.0.1"
EMITTER_PORT = 17879


def model_available():
    needed = ["preprocess.onnx", "tokens.txt"]
    if not os.path.isdir(MODEL_DIR):
        return False
    for n in needed:
        if not os.path.exists(os.path.join(MODEL_DIR, n)):
            return False
    return True


pytestmark = pytest.mark.skipif(
    not model_available(), reason="Moonshine model not present — run models/fetch.sh"
)


def find_plugin_so():
    build_dir = os.environ.get("PLUGIN_BUILD_DIR", os.path.join(SRC, "build"))
    for root, _, files in os.walk(build_dir):
        for f in files:
            if f == "rtp_asr_plugin.so":
                return os.path.join(root, f)
    return None


def make_startup_conf(plugin_so, run_dir, log_file, prefix):
    plugin_dir = os.path.dirname(plugin_so)
    return f"""
unix {{
  nodaemon
  log {log_file}
  full-coredump
  cli-listen {run_dir}/cli.sock
  gid vpp
}}
api-segment {{
  prefix {prefix}
}}
plugins {{
  path {plugin_dir}
  plugin default {{ disable }}
  plugin rtp_asr_plugin.so {{ enable }}
}}
"""


@pytest.fixture
def vpp_e2e(tmp_path):
    plugin_so = find_plugin_so()
    if not plugin_so:
        pytest.skip("plugin .so not built")

    run_dir = tmp_path / "run"
    run_dir.mkdir()
    log_file = tmp_path / "vpp.log"
    prefix = f"rtp-asr-e2e-{os.getpid()}"
    conf = run_dir / "startup.conf"
    conf.write_text(make_startup_conf(plugin_so, str(run_dir), str(log_file), prefix))

    env = os.environ.copy()
    env["VPP_RTP_ASR_MODEL_DIR"] = MODEL_DIR
    env["VPP_RTP_ASR_EMITTER_JSON_UDP"] = f"{EMITTER_HOST}:{EMITTER_PORT}"
    proc = subprocess.Popen(
        ["vpp", "-c", str(conf)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
        env=env,
    )
    cli_sock = run_dir / "cli.sock"
    for _ in range(80):
        if cli_sock.exists():
            break
        time.sleep(0.1)
    yield {"proc": proc, "cli_sock": str(cli_sock), "tmp_path": tmp_path}
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()


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
