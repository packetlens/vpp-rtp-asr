#!/usr/bin/env bash
# VPP startup for baresip-call lab.
# Configures two af_packet interfaces (caller-side and callee-side), enabling
# VPP as an L3 router between the two baresip containers.  rtp-asr-tap runs
# on both interfaces, so every RTP packet in both directions is transcribed.
set -euo pipefail

PLUGIN_SO=$(find /src/build -name rtp_asr_plugin.so 2>/dev/null | head -1)
if [[ -z "$PLUGIN_SO" ]]; then
  echo "[vpp] ERROR: rtp_asr_plugin.so not found — run 'docker compose run --rm build' first"
  exit 1
fi

# Copy plugin into VPP's system dir so af_packet and our plugin share one path.
SYS_PLUGIN_DIR=/usr/lib/x86_64-linux-gnu/vpp_plugins
cp "$PLUGIN_SO" "$SYS_PLUGIN_DIR/"
echo "[vpp] Installed rtp_asr_plugin.so → ${SYS_PLUGIN_DIR}"

mkdir -p /run/vpp

cat > /tmp/vpp-lab.conf << EOF
unix {
  nodaemon
  log /tmp/vpp-lab.log
  full-coredump
  cli-listen /run/vpp/cli.sock
  gid vpp
}
api-segment { prefix rtp-asr-lab }
plugins {
  path ${SYS_PLUGIN_DIR}
  plugin default { disable }
  plugin af_packet_plugin.so { enable }
  plugin rtp_asr_plugin.so { enable }
}
EOF

echo "[vpp] Starting VPP"
vpp -c /tmp/vpp-lab.conf &
VPP_PID=$!

# Wait for VPP CLI socket to appear AND start accepting connections (up to 30s)
READY=0
for i in $(seq 1 300); do
  if vppctl -s /run/vpp/cli.sock show version &>/dev/null; then
    READY=1; break
  fi
  sleep 0.1
done
if [[ $READY -eq 0 ]]; then
  echo "[vpp] ERROR: VPP CLI not accepting connections after 30s; log:"
  cat /tmp/vpp-lab.log 2>/dev/null || true
  exit 1
fi
echo "[vpp] CLI socket ready"

# Detect which kernel interfaces are on caller-net (10.66.22.x) and callee-net (10.66.23.x)
CALLER_IF=$(ip -4 -br addr | awk '$3 ~ /^10\.66\.22\./ {print $1}' | sed 's/@.*//')
CALLEE_IF=$(ip -4 -br addr | awk '$3 ~ /^10\.66\.23\./ {print $1}' | sed 's/@.*//')

echo "[vpp] caller-net interface: ${CALLER_IF:-<not found>}"
echo "[vpp] callee-net interface: ${CALLEE_IF:-<not found>}"

[[ -n "$CALLER_IF" && -n "$CALLEE_IF" ]] || {
  echo "[vpp] ERROR: could not find caller/callee interfaces"; ip -br addr; exit 1
}

# Hand both data interfaces to VPP; mgmt stays under the kernel
vppctl create host-interface name "${CALLER_IF}"
vppctl set interface state "host-${CALLER_IF}" up
vppctl set interface ip address "host-${CALLER_IF}" 10.66.22.10/24

vppctl create host-interface name "${CALLEE_IF}"
vppctl set interface state "host-${CALLEE_IF}" up
vppctl set interface ip address "host-${CALLEE_IF}" 10.66.23.10/24

# VPP creates connected routes automatically from set interface ip address.
# Explicit routes via own-IP create self-referential next-hops that break forwarding.

# Enable rtp-asr tap on BOTH interfaces — intercept RTP in both directions
vppctl set interface rtp-asr "host-${CALLER_IF}" enable
vppctl set interface rtp-asr "host-${CALLEE_IF}" enable

# Remove kernel IPs to avoid ARP conflicts (VPP handles ARP now)
ip addr del 10.66.22.10/24 dev "${CALLER_IF}" 2>/dev/null || true
ip addr del 10.66.23.10/24 dev "${CALLEE_IF}" 2>/dev/null || true
echo "[vpp] kernel IPs removed — VPP handles ARP on both sides"

echo "[vpp] Configured:"
echo "      host-${CALLER_IF} = 10.66.22.10/24  rtp-asr: ON"
echo "      host-${CALLEE_IF} = 10.66.23.10/24  rtp-asr: ON"
echo "      Moonshine model: ${VPP_RTP_ASR_MODEL_DIR:-<not set>}"
echo "      Emitter: ${VPP_RTP_ASR_EMITTER_JSON_UDP:-<not set>}"

wait "$VPP_PID"
