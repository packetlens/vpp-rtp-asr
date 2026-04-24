"""Smoke test: the plugin loads cleanly and registers its CLI commands.

This is the minimal assertion — does VPP start with rtp_asr_plugin.so loaded,
and does `show rtp-asr stats` (or equivalent) respond? No RTP traffic yet.
"""

import subprocess

import pytest


@pytest.mark.integration
def test_plugin_loads(vpp_instance):
    """VPP came up and the CLI socket is reachable."""
    r = subprocess.run(
        ["vppctl", "-s", vpp_instance["cli_sock"], "show", "version"],
        capture_output=True,
        text=True,
        timeout=5,
    )
    assert r.returncode == 0, r.stderr
    assert "vpp" in r.stdout.lower()


@pytest.mark.integration
@pytest.mark.xfail(reason="CLI commands not implemented yet (v1 scaffold)")
def test_rtp_asr_show_stats(vpp_instance):
    r = subprocess.run(
        ["vppctl", "-s", vpp_instance["cli_sock"], "show", "rtp-asr", "stats"],
        capture_output=True,
        text=True,
        timeout=5,
    )
    assert r.returncode == 0, r.stderr
    assert "packets" in r.stdout.lower()
