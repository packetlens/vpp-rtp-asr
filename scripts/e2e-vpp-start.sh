#!/usr/bin/env bash
# Start VPP as an L3 router between caller-net and sink-net.
# rtp-asr-tap intercepts transit RTP packets on both host interfaces.
# Runs inside the vpp service container in compose-e2e.yaml.
set -euo pipefail

PLUGIN_SO=$(find /src/build -name rtp_asr_plugin.so 2>/dev/null | head -1)
if [[ -z "$PLUGIN_SO" ]]; then
  echo "[vpp] ERROR: rtp_asr_plugin.so not found under /src/build — run 'docker compose run --rm build' first"
  exit 1
fi

# Install plugin into VPP's system dir so it lives alongside af_packet_plugin.so
SYS_PLUGIN_DIR=/usr/lib/x86_64-linux-gnu/vpp_plugins
cp "$PLUGIN_SO" "$SYS_PLUGIN_DIR/"
echo "[vpp] Installed rtp_asr_plugin.so → ${SYS_PLUGIN_DIR}"

mkdir -p /run/vpp

cat > /tmp/vpp-e2e.conf << EOF
unix {
  nodaemon
  log /tmp/vpp-e2e.log
  full-coredump
  cli-listen /run/vpp/cli.sock
  gid vpp
}
api-segment { prefix rtp-asr-e2e }
plugins {
  path ${SYS_PLUGIN_DIR}
  plugin default { disable }
  plugin af_packet_plugin.so { enable }
  plugin rtp_asr_plugin.so { enable }
}
EOF

echo "[vpp] Starting VPP"
vpp -c /tmp/vpp-e2e.conf &
VPP_PID=$!

# Wait until VPP CLI is accepting connections (socket exists AND responds)
READY=0
for i in $(seq 1 300); do
  if vppctl -s /run/vpp/cli.sock show version &>/dev/null; then
    READY=1; break
  fi
  sleep 0.1
done
if [[ $READY -eq 0 ]]; then
  echo "[vpp] ERROR: VPP CLI not ready after 30s; log:"
  cat /tmp/vpp-e2e.log 2>/dev/null || true
  exit 1
fi
echo "[vpp] CLI socket ready"

# Detect caller-side (10.66.21.x) and sink-side (10.66.23.x) kernel interfaces
CALLER_IF=$(ip -4 -br addr | awk '$3 ~ /^10\.66\.21\./ {print $1}' | sed 's/@.*//')
SINK_IF=$(ip -4 -br addr | awk '$3 ~ /^10\.66\.23\./ {print $1}' | sed 's/@.*//')

echo "[vpp] caller-side interface: ${CALLER_IF:-<not found>}"
echo "[vpp] sink-side interface:   ${SINK_IF:-<not found>}"

[[ -n "$CALLER_IF" && -n "$SINK_IF" ]] || {
  echo "[vpp] ERROR: could not find caller/sink interfaces"; ip -br addr; exit 1
}

# Create host-interfaces for both data networks
vppctl create host-interface name "${CALLER_IF}"
vppctl set interface state "host-${CALLER_IF}" up
vppctl set interface ip address "host-${CALLER_IF}" 10.66.21.10/24

vppctl create host-interface name "${SINK_IF}"
vppctl set interface state "host-${SINK_IF}" up
vppctl set interface ip address "host-${SINK_IF}" 10.66.23.10/24

# VPP automatically creates connected routes for both subnets when interface
# IPs are set — no explicit ip route add needed. Adding them with via=own-IP
# would create self-referential next-hops that override the connected routes
# and cause ~50% of forwarded packets to be misdirected back to ip4-local.

# Enable rtp-asr tap on BOTH interfaces — intercept RTP in both directions
vppctl set interface rtp-asr "host-${CALLER_IF}" enable
vppctl set interface rtp-asr "host-${SINK_IF}" enable

# Remove kernel IPs to avoid ARP conflicts
ip addr del 10.66.21.10/24 dev "${CALLER_IF}" 2>/dev/null || true
ip addr del 10.66.23.10/24 dev "${SINK_IF}" 2>/dev/null || true

echo "[vpp] Configured:"
echo "      host-${CALLER_IF} = 10.66.21.10/24  rtp-asr: ON"
echo "      host-${SINK_IF} = 10.66.23.10/24  rtp-asr: ON"
echo "      Moonshine model: ${VPP_RTP_ASR_MODEL_DIR:-<not set>}"
echo "      Emitter target: ${VPP_RTP_ASR_EMITTER_JSON_UDP:-<not set>}"

# Signal that VPP is fully configured (health check waits for this)
touch /tmp/vpp-interfaces-ready

# Background diagnostic: print interface and node counters every 10s
(
  while true; do
    sleep 10
    echo "=== [vpp-diag] kernel rx stats ==="
    ip -s link show "${CALLER_IF}" 2>/dev/null | grep -A2 'RX:' | head -5
    ip -s link show "${SINK_IF}"   2>/dev/null | grep -A2 'RX:' | head -5
    echo "=== [vpp-diag] vpp interface counters ==="
    vppctl -s /run/vpp/cli.sock show interface 2>/dev/null | grep -E 'host-|Name' | head -10
    echo "=== [vpp-diag] rtp-asr stats ==="
    vppctl -s /run/vpp/cli.sock show rtp-asr stats 2>/dev/null
  done
) &

wait "$VPP_PID"
