#!/usr/bin/env bash
# VPP startup for pcap-replay lab — single caller-net interface, rtp-asr enabled.
set -euo pipefail

PLUGIN_SO=$(find /src/build -name rtp_asr_plugin.so 2>/dev/null | head -1)
if [[ -z "$PLUGIN_SO" ]]; then
  echo "[vpp] ERROR: rtp_asr_plugin.so not found — run 'docker compose run --rm build' first"
  exit 1
fi

SYS_PLUGIN_DIR=/usr/lib/x86_64-linux-gnu/vpp_plugins
cp "$PLUGIN_SO" "$SYS_PLUGIN_DIR/"
echo "[vpp] Installed rtp_asr_plugin.so → ${SYS_PLUGIN_DIR}"

mkdir -p /run/vpp

cat > /tmp/vpp-pcap.conf << EOF
unix {
  nodaemon
  log /tmp/vpp-pcap.log
  full-coredump
  cli-listen /run/vpp/cli.sock
  gid vpp
}
api-segment { prefix rtp-asr-pcap }
plugins {
  path ${SYS_PLUGIN_DIR}
  plugin default { disable }
  plugin af_packet_plugin.so { enable }
  plugin rtp_asr_plugin.so { enable }
}
EOF

vpp -c /tmp/vpp-pcap.conf &
VPP_PID=$!

READY=0
for i in $(seq 1 300); do
  if vppctl -s /run/vpp/cli.sock show version &>/dev/null; then
    READY=1; break
  fi
  sleep 0.1
done
[[ $READY -eq 1 ]] || { echo "[vpp] CLI not ready after 30s"; cat /tmp/vpp-pcap.log; exit 1; }
echo "[vpp] CLI ready"

# Find the kernel interface on caller-net (10.66.22.x)
CALLER_IF=$(ip -4 -br addr | awk '$3 ~ /^10\.66\.22\./ {print $1}' | sed 's/@.*//')
[[ -n "$CALLER_IF" ]] || { echo "[vpp] caller-net interface not found"; ip -br addr; exit 1; }
echo "[vpp] caller-net interface: ${CALLER_IF}"

vppctl create host-interface name "${CALLER_IF}"
vppctl set interface state "host-${CALLER_IF}" up
vppctl set interface ip address "host-${CALLER_IF}" 10.66.22.10/24
vppctl set interface rtp-asr "host-${CALLER_IF}" enable

ip addr del 10.66.22.10/24 dev "${CALLER_IF}" 2>/dev/null || true
echo "[vpp] kernel IP removed — VPP handles ARP on caller-net"

echo "[vpp] host-${CALLER_IF} = 10.66.22.10/24  rtp-asr: ON"
echo "[vpp] model: ${VPP_RTP_ASR_MODEL_DIR:-<not set>}"
echo "[vpp] emitter: ${VPP_RTP_ASR_EMITTER_JSON_UDP:-<not set>}"

wait "$VPP_PID"
