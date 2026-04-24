#!/usr/bin/env bash
# Callee: add route through VPP to reach caller-net, then listen for
# incoming SIP calls.  The accounts file has answermode=auto so all
# incoming calls are auto-answered with no user interaction.
set -euo pipefail

ip route add 10.66.22.0/24 via 10.66.23.10 2>/dev/null || true

echo "[callee] Routes:"
ip route show | grep 10.66

mkdir -p /tmp/baresip-callee
cp /src/labs/baresip-call/config/callee/config   /tmp/baresip-callee/config
cp /src/labs/baresip-call/config/callee/accounts /tmp/baresip-callee/accounts
echo "[callee] Listening for SIP calls (auto-answer enabled) ..."
exec baresip -f /tmp/baresip-callee -t 90
