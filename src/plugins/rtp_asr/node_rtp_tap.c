/*
 * node_rtp_tap.c - RTP tap feature-arc graph node.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runs on ip4-unicast / ip6-unicast before ip-lookup. For each UDP packet
 * whose destination (or source) port is in the configured RTP accept-set,
 * parses the 12-byte RTP header, extracts SSRC, and looks up or creates a
 * per-SSRC session in the per-worker bihash+pool.
 *
 * This minimum-compile pass does NOT copy the RTP payload into a ring. The
 * codec-decode + Sherpa-ONNX worker side gets wired in a later pass.
 *
 * Per-packet budget target: < 100 ns steady-state (see FUNC_SPEC.md §3.2).
 */

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/feature/feature.h>
#include <vnet/ip/ip4.h>
#include <vnet/ip/ip6.h>
#include <vnet/udp/udp_packet.h>
#include <rtp_asr/rtp_asr.h>

VNET_FEATURE_INIT (rtp_asr_tap_ip4, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "rtp-asr-tap",
  .runs_before = VNET_FEATURES ("ip4-lookup"),
};

VNET_FEATURE_INIT (rtp_asr_tap_ip6, static) = {
  .arc_name = "ip6-unicast",
  .node_name = "rtp-asr-tap",
  .runs_before = VNET_FEATURES ("ip6-lookup"),
};

typedef struct
{
  u32 sw_if_index;
  u32 ssrc;
  u16 src_port;
  u16 dst_port;
  u8  payload_type;
  u8  is_ip6;
  u8  created;
  u8  pad;
} rtp_asr_trace_t;

static u8 *
format_rtp_asr_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  rtp_asr_trace_t *t = va_arg (*args, rtp_asr_trace_t *);
  s = format (s,
	      "rtp-asr: sw_if %u %s %u->%u ssrc=0x%08x pt=%u %s",
	      t->sw_if_index, t->is_ip6 ? "ip6" : "ip4",
	      t->src_port, t->dst_port, t->ssrc, t->payload_type,
	      t->created ? "NEW" : "hit");
  return s;
}

#define foreach_rtp_asr_error                                                 \
  _ (PROCESSED, "rtp packets processed")                                      \
  _ (NEW_SESSION, "new sessions created")                                     \
  _ (NOT_UDP, "packets not UDP (pass-through)")                               \
  _ (NOT_RTP, "UDP packets not matching rtp heuristic")                       \
  _ (MALFORMED, "malformed rtp headers")

typedef enum
{
#define _(sym, str) RTP_ASR_ERROR_##sym,
  foreach_rtp_asr_error
#undef _
    RTP_ASR_N_ERROR,
} rtp_asr_error_t;

static char *rtp_asr_error_strings[] = {
#define _(sym, str) str,
  foreach_rtp_asr_error
#undef _
};

/* ---- RTP header layout (first 12 bytes) ----
 *   V=2 P X CC  M  PT    sequence_number
 *   timestamp
 *   ssrc
 */
typedef CLIB_PACKED (struct {
  u8  vpxcc;        /* [V:2][P:1][X:1][CC:4] */
  u8  mpt;          /* [M:1][PT:7] */
  u16 seq;
  u32 timestamp;
  u32 ssrc;
}) rtp_hdr_t;
STATIC_ASSERT_SIZEOF (rtp_hdr_t, 12);

static_always_inline int
rtp_port_accepted (u16 port)
{
  rtp_asr_main_t *rm = &rtp_asr_main;
  if (port == rm->rtp_port_well_known)
    return 1;
  if (port >= rm->rtp_port_min && port <= rm->rtp_port_max)
    return 1;
  return 0;
}

static_always_inline int
rtp_parse (const u8 *udp_payload, u32 udp_payload_len,
	   u8 *out_pt, u16 *out_seq, u32 *out_ts, u32 *out_ssrc)
{
  if (udp_payload_len < sizeof (rtp_hdr_t))
    return -1;
  const rtp_hdr_t *h = (const rtp_hdr_t *) udp_payload;
  u8 version = (h->vpxcc >> 6) & 0x3;
  if (version != 2)
    return -1;
  *out_pt = h->mpt & 0x7f;
  *out_seq = clib_net_to_host_u16 (h->seq);
  *out_ts = clib_net_to_host_u32 (h->timestamp);
  *out_ssrc = clib_net_to_host_u32 (h->ssrc);
  return 0;
}

static_always_inline void
rtp_asr_observe_v4 (rtp_asr_worker_t *w, ip4_header_t *ip0, u32 ip_len,
		    u32 sw_if_index, u32 pkt_bytes, f64 now,
		    rtp_asr_trace_t *trace,
		    u32 *n_new, u32 *n_not_rtp, u32 *n_malformed,
		    u32 *n_processed)
{
  if (ip0->protocol != IP_PROTOCOL_UDP)
    {
      w->not_udp++;
      return;
    }

  u8 ihl = (ip0->ip_version_and_header_length & 0x0f) * 4;
  if (ip_len < ihl + sizeof (udp_header_t))
    return;

  udp_header_t *udp0 = (udp_header_t *) ((u8 *) ip0 + ihl);
  u16 sport = clib_net_to_host_u16 (udp0->src_port);
  u16 dport = clib_net_to_host_u16 (udp0->dst_port);

  if (!rtp_port_accepted (sport) && !rtp_port_accepted (dport))
    {
      w->not_rtp++;
      (*n_not_rtp)++;
      return;
    }

  u32 udp_len = clib_net_to_host_u16 (udp0->length);
  if (udp_len < sizeof (udp_header_t))
    return;
  u32 udp_payload_len = udp_len - sizeof (udp_header_t);
  const u8 *udp_payload = (const u8 *) (udp0 + 1);

  u8  pt;
  u16 seq;
  u32 ts, ssrc;
  if (rtp_parse (udp_payload, udp_payload_len, &pt, &seq, &ts, &ssrc) != 0)
    {
      w->malformed_rtp++;
      (*n_malformed)++;
      return;
    }

  rtp_asr_session_key4_t k = { 0 };
  k.src = ip0->src_address;
  k.dst = ip0->dst_address;
  k.src_port = sport;
  k.dst_port = dport;
  k.ssrc = ssrc;

  int created = 0;
  rtp_asr_session_t *s =
      rtp_asr_session_lookup_or_create4 (w, &k, &created);
  if (PREDICT_FALSE (!s))
    return;

  if (created)
    {
      s->sw_if_index = sw_if_index;
      s->payload_type = pt;
      s->last_seq = seq;
      s->last_rtp_ts = ts;
      (*n_new)++;
    }
  s->packets++;
  s->bytes += pkt_bytes;
  s->last_seen = now;
  s->last_seq = seq;
  s->last_rtp_ts = ts;

  w->packets++;
  w->bytes += pkt_bytes;
  (*n_processed)++;

  trace->sw_if_index = sw_if_index;
  trace->ssrc = ssrc;
  trace->src_port = sport;
  trace->dst_port = dport;
  trace->payload_type = pt;
  trace->is_ip6 = 0;
  trace->created = (u8) created;
}

static_always_inline void
rtp_asr_observe_v6 (rtp_asr_worker_t *w, ip6_header_t *ip0, u32 ip_len,
		    u32 sw_if_index, u32 pkt_bytes, f64 now,
		    rtp_asr_trace_t *trace,
		    u32 *n_new, u32 *n_not_rtp, u32 *n_malformed,
		    u32 *n_processed)
{
  if (ip0->protocol != IP_PROTOCOL_UDP)
    {
      w->not_udp++;
      return;
    }

  if (ip_len < sizeof (ip6_header_t) + sizeof (udp_header_t))
    return;

  udp_header_t *udp0 = (udp_header_t *) (ip0 + 1);
  u16 sport = clib_net_to_host_u16 (udp0->src_port);
  u16 dport = clib_net_to_host_u16 (udp0->dst_port);

  if (!rtp_port_accepted (sport) && !rtp_port_accepted (dport))
    {
      w->not_rtp++;
      (*n_not_rtp)++;
      return;
    }

  u32 udp_len = clib_net_to_host_u16 (udp0->length);
  if (udp_len < sizeof (udp_header_t))
    return;
  u32 udp_payload_len = udp_len - sizeof (udp_header_t);
  const u8 *udp_payload = (const u8 *) (udp0 + 1);

  u8  pt;
  u16 seq;
  u32 ts, ssrc;
  if (rtp_parse (udp_payload, udp_payload_len, &pt, &seq, &ts, &ssrc) != 0)
    {
      w->malformed_rtp++;
      (*n_malformed)++;
      return;
    }

  rtp_asr_session_key6_t k = { 0 };
  k.src = ip0->src_address;
  k.dst = ip0->dst_address;
  k.src_port = sport;
  k.dst_port = dport;
  k.ssrc = ssrc;

  int created = 0;
  rtp_asr_session_t *s =
      rtp_asr_session_lookup_or_create6 (w, &k, &created);
  if (PREDICT_FALSE (!s))
    return;

  if (created)
    {
      s->sw_if_index = sw_if_index;
      s->payload_type = pt;
      s->last_seq = seq;
      s->last_rtp_ts = ts;
      (*n_new)++;
    }
  s->packets++;
  s->bytes += pkt_bytes;
  s->last_seen = now;
  s->last_seq = seq;
  s->last_rtp_ts = ts;

  w->packets++;
  w->bytes += pkt_bytes;
  (*n_processed)++;

  trace->sw_if_index = sw_if_index;
  trace->ssrc = ssrc;
  trace->src_port = sport;
  trace->dst_port = dport;
  trace->payload_type = pt;
  trace->is_ip6 = 1;
  trace->created = (u8) created;
}

static uword
rtp_asr_tap_node_fn (vlib_main_t *vm, vlib_node_runtime_t *node,
		     vlib_frame_t *frame)
{
  u32 n_left_from, *from, *to_next;
  rtp_asr_main_t *rm = &rtp_asr_main;
  u32 thread_index = vm->thread_index;
  rtp_asr_worker_t *w = vec_elt_at_index (rm->per_worker, thread_index);
  u32 next_index = node->cached_next_index;
  f64 now = vlib_time_now (vm);
  u32 n_processed = 0, n_new = 0, n_not_rtp = 0, n_malformed = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;
      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  u32 bi0 = from[0];
	  vlib_buffer_t *b0 = vlib_get_buffer (vm, bi0);
	  u32 next0 = RTP_ASR_NEXT_PASS;
	  rtp_asr_trace_t trace0 = { 0 };
	  int traced = 0;

	  if (n_left_from > 1)
	    {
	      vlib_buffer_t *nb = vlib_get_buffer (vm, from[1]);
	      clib_prefetch_load (nb);
	      CLIB_PREFETCH (nb->data, 2 * CLIB_CACHE_LINE_BYTES, LOAD);
	    }

	  to_next[0] = bi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  ip4_header_t *ip0 = vlib_buffer_get_current (b0);
	  u8 version = (ip0->ip_version_and_header_length >> 4);
	  u32 pkt_bytes = vlib_buffer_length_in_chain (vm, b0);
	  u32 sw_if_index = vnet_buffer (b0)->sw_if_index[VLIB_RX];

	  if (PREDICT_TRUE (version == 4))
	    {
	      u32 ip_len = clib_net_to_host_u16 (ip0->length);
	      rtp_asr_observe_v4 (w, ip0, ip_len, sw_if_index, pkt_bytes,
				  now, &trace0, &n_new, &n_not_rtp,
				  &n_malformed, &n_processed);
	      traced = 1;
	    }
	  else if (version == 6)
	    {
	      ip6_header_t *ip6 = (ip6_header_t *) ip0;
	      u32 ip_len =
		  clib_net_to_host_u16 (ip6->payload_length) + sizeof (*ip6);
	      rtp_asr_observe_v6 (w, ip6, ip_len, sw_if_index, pkt_bytes,
				  now, &trace0, &n_new, &n_not_rtp,
				  &n_malformed, &n_processed);
	      traced = 1;
	    }

	  vnet_feature_next (&next0, b0);

	  if (PREDICT_FALSE (traced && (b0->flags & VLIB_BUFFER_IS_TRACED)))
	    {
	      rtp_asr_trace_t *t =
		  vlib_add_trace (vm, node, b0, sizeof (*t));
	      *t = trace0;
	    }

	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
					   n_left_to_next, bi0, next0);
	}
      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  vlib_node_increment_counter (vm, node->node_index, RTP_ASR_ERROR_PROCESSED,
			       n_processed);
  vlib_node_increment_counter (vm, node->node_index, RTP_ASR_ERROR_NEW_SESSION,
			       n_new);
  vlib_node_increment_counter (vm, node->node_index, RTP_ASR_ERROR_NOT_RTP,
			       n_not_rtp);
  vlib_node_increment_counter (vm, node->node_index, RTP_ASR_ERROR_MALFORMED,
			       n_malformed);
  return frame->n_vectors;
}

VLIB_REGISTER_NODE (rtp_asr_tap_node) = {
  .function = rtp_asr_tap_node_fn,
  .name = "rtp-asr-tap",
  .vector_size = sizeof (u32),
  .format_trace = format_rtp_asr_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = RTP_ASR_N_ERROR,
  .error_strings = rtp_asr_error_strings,
  .n_next_nodes = RTP_ASR_N_NEXT,
  .next_nodes = {
    [RTP_ASR_NEXT_PASS] = "ip4-lookup", /* overridden by feature arc */
  },
};
