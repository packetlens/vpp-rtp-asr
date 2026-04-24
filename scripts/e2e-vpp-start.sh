#!/usr/bin/env bash
# Start VPP with rtp-asr plugin, configure af_packet on the data-net interface.
# Runs inside the vpp service container in compose-e2e.yaml.
set -euo pipefail

PLUGIN_SO=$(find /src/build -name rtp_asr_plugin.so 2>/dev/null | head -1)
if [[ -z "$PLUGIN_SO" ]]; then
  echo "[vpp] ERROR: rtp_asr_plugin.so not found under /src/build — run 'docker compose run --rm build' first"
  exit 1
fi
PLUGIN_DIR=$(dirname "$PLUGIN_SO")

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
  path ${PLUGIN_DIR}
  plugin default { disable }
  plugin rtp_asr_plugin.so { enable }
}
EOF

echo "[vpp] Starting VPP with plugin from ${PLUGIN_DIR}"
vpp -c /tmp/vpp-e2e.conf &
VPP_PID=$!

# Wait for CLI socket (up to 20s)
for i in $(seq 1 200); do
  [[ -S /run/vpp/cli.sock ]] && break
  sleep 0.1
done
if [[ ! -S /run/vpp/cli.sock ]]; then
  echo "[vpp] ERROR: CLI socket never appeared; VPP log:"
  cat /tmp/vpp-e2e.log 2>/dev/null || true
  exit 1
fi
echo "[vpp] CLI socket ready"

# Detect which kernel interface is on the data network (172.21.x.x)
DATA_IF=$(ip -4 -br addr | awk '$3 ~ /^172\.21\./ {print $1}')
if [[ -z "$DATA_IF" ]]; then
  echo "[vpp] ERROR: cannot find data-net interface (172.21.x.x) — interfaces:"
  ip -br addr
  exit 1
fi
echo "[vpp] Data interface: ${DATA_IF} (will be claimed by af_packet)"

# Hand the data interface to VPP; keep the mgmt interface under the kernel
vppctl create af-packet host-if "${DATA_IF}" name data0
vppctl set interface state data0 up
vppctl set interface ip address data0 172.21.0.10/24
vppctl set interface rtp-asr data0 enable

echo "[vpp] af_packet data0 (172.21.0.10/24) configured, rtp-asr enabled"
echo "[vpp] Moonshine model: ${VPP_RTP_ASR_MODEL_DIR:-<not set>}"
echo "[vpp] Emitter target: ${VPP_RTP_ASR_EMITTER_JSON_UDP:-<not set>}"

# Remove kernel IP from data interface to avoid ARP conflicts
ip addr del 172.21.0.10/24 dev "${DATA_IF}" 2>/dev/null || true

wait "$VPP_PID"
