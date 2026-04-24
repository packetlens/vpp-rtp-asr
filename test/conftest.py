"""pytest fixtures for vpp-rtp-asr tests.

Mirrors the pattern from vpp-ndpi/test/conftest.py: locate the built plugin
.so, generate a VPP startup.conf pointing at it, spawn VPP under a temp
runtime directory, and tear it down after the test.
"""

import os
import signal
import subprocess
import tempfile
import time

import pytest


PLUGIN_BUILD_DIR = os.environ.get(
    "PLUGIN_BUILD_DIR",
    os.path.join(os.environ.get("SRC_DIR", "/src"), "build"),
)


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
