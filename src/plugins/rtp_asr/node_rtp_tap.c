/*
 * node_rtp_tap.c - RTP tap feature-arc graph node.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Feature-arc placement: ip4-unicast / ip6-unicast, before ip-lookup.
 * Per-packet budget: < 100 ns steady-state (bihash hit).
 *
 * See FUNC_SPEC.md §3 for the hot-path contract. This file MUST NOT call
 * any codec / resample / VAD / inference code — those live off-path on the
 * Linux worker thread pool (see asr_worker.c).
 *
 * Upstream candidate: this file + rtp_session.c (minus codec/ASR) are the
 * "RTP session-tracking feature arc" we propose to fd.io/vpp. Keep the
 * codec/ASR separation clean in the split.
 */

#include <rtp_asr/rtp_asr.h>
#include <vnet/feature/feature.h>

/* TODO(v1):
 *   - VLIB_NODE_FN (rtp_asr_tap_node): two-dual loop over frame->buffers[]
 *     - vlib_buffer_get_current() → UDP payload
 *     - port-range accept filter {5004, 16384..32768}
 *     - parse 12B RTP header, check V==2
 *     - bihash lookup via rtp_asr_session_lookup_or_create()
 *     - enqueue {session_idx, payload_ofs, len, rtp_ts, vpp_time} to SPSC ring
 *     - counter update, NEXT_PASS
 *   - VNET_FEATURE_INIT arcs: {ip4-unicast, ip6-unicast} before ip-lookup
 *   - VLIB_REGISTER_NODE with NEXT nodes PASS | DROP | PUNT
 *   - clib_error_t *rtp_asr_enable_disable() — vnet_feature_enable_disable
 *     on the registered arcs
 */
