"""pytest fixtures for vpp-rtp-asr tests.

Mirrors the pattern from vpp-ndpi/test/conftest.py: locate the built plugin
.so, generate a VPP startup.conf pointing at it, spawn VPP under a temp
runtime directory, and tear it down after the test.
"""

import os
import signal
import subprocess
import time

import pytest


SRC = os.environ.get("SRC_DIR", "/src")
PLUGIN_BUILD_DIR = os.environ.get("PLUGIN_BUILD_DIR", os.path.join(SRC, "build"))
MODEL_DIR = os.path.join(SRC, "models", "sherpa-onnx-moonshine-tiny-en-int8")
PIPER_MODEL = os.path.join(SRC, "models", "piper", "en_US-lessac-medium.onnx")
EMITTER_HOST = "127.0.0.1"
EMITTER_PORT = 17879


def model_available():
    needed = ["preprocess.onnx", "tokens.txt"]
    if not os.path.isdir(MODEL_DIR):
        return False
    return all(os.path.exists(os.path.join(MODEL_DIR, n)) for n in needed)


def piper_available():
    if not os.path.exists(PIPER_MODEL):
        return False
    try:
        import piper as _p  # noqa: F401
        return True
    except ImportError:
        return False


def find_plugin_so():
    for root, _, files in os.walk(PLUGIN_BUILD_DIR):
        for f in files:
            if f == "rtp_asr_plugin.so":
                return os.path.join(root, f)
    return None


def make_startup_conf(plugin_path, run_dir, log_file, prefix):
    plugin_dir = os.path.dirname(plugin_path)
    stats_sock = os.path.join(run_dir, "stats.sock")
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
socksvr {{
  socket-name {run_dir}/api.sock
}}
statseg {{
  socket-name {stats_sock}
}}
plugins {{
  path {plugin_dir}
  plugin default {{ disable }}
  plugin rtp_asr_plugin.so {{ enable }}
}}
"""


@pytest.fixture
def vpp_instance(tmp_path):
    """Spawn a VPP instance with the rtp_asr plugin loaded."""
    plugin = find_plugin_so()
    if not plugin:
        pytest.skip(f"rtp_asr_plugin.so not found under {PLUGIN_BUILD_DIR}")

    run_dir = tmp_path / "run"
    run_dir.mkdir()
    log_file = tmp_path / "vpp.log"
    prefix = f"rtp-asr-test-{os.getpid()}"

    conf = run_dir / "startup.conf"
    conf.write_text(make_startup_conf(plugin, str(run_dir), str(log_file), prefix))

    proc = subprocess.Popen(
        ["vpp", "-c", str(conf)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )
    # wait for socket
    cli_sock = run_dir / "cli.sock"
    for _ in range(50):
        if cli_sock.exists():
            break
        time.sleep(0.1)

    yield {
        "proc": proc,
        "cli_sock": str(cli_sock),
        "api_sock": str(run_dir / "api.sock"),
        "stats_sock": str(run_dir / "stats.sock"),
        "log_file": str(log_file),
    }

    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def _e2e_startup_conf(plugin_so, run_dir, log_file, prefix):
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
    """VPP with rtp_asr plugin + Moonshine model loaded via env vars."""
    plugin_so = find_plugin_so()
    if not plugin_so:
        pytest.skip("plugin .so not built")
    if not model_available():
        pytest.skip("Moonshine model not present — run models/fetch.sh")

    run_dir = tmp_path / "run"
    run_dir.mkdir()
    log_file = tmp_path / "vpp.log"
    prefix = f"rtp-asr-e2e-{os.getpid()}"
    conf = run_dir / "startup.conf"
    conf.write_text(_e2e_startup_conf(plugin_so, str(run_dir), str(log_file), prefix))

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
