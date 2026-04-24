#!/usr/bin/env bash
# Caller: add route through VPP to reach callee-net, then place a SIP call
# to bob (the callee) streaming the audio file.
set -euo pipefail

# Route to callee-net goes via VPP's caller-side IP
ip route add 10.66.23.0/24 via 10.66.22.10 2>/dev/null || true

echo "[caller] Routes:"
ip route show | grep 10.66

# Give the callee a moment to register its SIP UA
sleep 3

mkdir -p /tmp/baresip-caller
cp /src/labs/baresip-call/config/caller/config   /tmp/baresip-caller/config
cp /src/labs/baresip-call/config/caller/accounts /tmp/baresip-caller/accounts
echo "[caller] Dialing bob@10.66.23.20 ..."
exec baresip \
  -f /tmp/baresip-caller \
  -e "/dial sip:bob@10.66.23.20" \
  -t 45
