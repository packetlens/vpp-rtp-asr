/*
 * rtp_session.c - Per-SSRC session table (bihash + pool).
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Upstream candidate: this file + node_rtp_tap.c (with codec/ASR removed)
 * are the "RTP session-tracking feature arc" proposed to fd.io. Keep the
 * split clean.
 */

#include <rtp_asr/rtp_asr.h>

int
rtp_asr_session_table_init (rtp_asr_worker_t *w, u32 capacity)
{
  if (capacity < 1024)
    capacity = 1024;

  u32 buckets = capacity / 4;
  if (buckets < 256)
    buckets = 256;

  clib_bihash_init_16_8 (&w->v4_ht, "rtp-asr-sessions-v4", buckets,
			 64ULL << 20 /* 64 MB */);
  clib_bihash_init_48_8 (&w->v6_ht, "rtp-asr-sessions-v6", buckets,
			 64ULL << 20 /* 64 MB */);
  return 0;
}

static rtp_asr_session_t *
rtp_asr_session_alloc (rtp_asr_worker_t *w)
{
  rtp_asr_session_t *s;
  pool_get (w->sessions, s);
  clib_memset (s, 0, sizeof (*s));
  s->first_seen = vlib_time_now (rtp_asr_main.vlib_main);
  s->last_seen = s->first_seen;
  s->sw_if_index = ~0;
  w->new_sessions++;
  return s;
}

rtp_asr_session_t *
rtp_asr_session_lookup_or_create4 (rtp_asr_worker_t *w,
				   const rtp_asr_session_key4_t *key,
				   int *created)
{
  clib_bihash_kv_16_8_t kv;
  clib_memcpy (&kv.key, key, sizeof (*key));

  if (clib_bihash_search_16_8 (&w->v4_ht, &kv, &kv) == 0)
    {
      *created = 0;
      return pool_elt_at_index (w->sessions, (u32) kv.value);
    }

  rtp_asr_session_t *s = rtp_asr_session_alloc (w);
  s->is_ip6 = 0;
  clib_memcpy (&s->key4, key, sizeof (*key));
  s->ssrc = key->ssrc;
  s->src_port = key->src_port;
  s->dst_port = key->dst_port;
  clib_memcpy (&kv.key, key, sizeof (*key));
  kv.value = (u64) (s - w->sessions);
  clib_bihash_add_del_16_8 (&w->v4_ht, &kv, 1 /* is_add */);
  *created = 1;
  return s;
}

rtp_asr_session_t *
rtp_asr_session_lookup_or_create6 (rtp_asr_worker_t *w,
				   const rtp_asr_session_key6_t *key,
				   int *created)
{
  clib_bihash_kv_48_8_t kv;
  clib_memcpy (&kv.key, key, sizeof (*key));

  if (clib_bihash_search_48_8 (&w->v6_ht, &kv, &kv) == 0)
    {
      *created = 0;
      return pool_elt_at_index (w->sessions, (u32) kv.value);
    }

  rtp_asr_session_t *s = rtp_asr_session_alloc (w);
  s->is_ip6 = 1;
  clib_memcpy (&s->key6, key, sizeof (*key));
  s->ssrc = key->ssrc;
  s->src_port = key->src_port;
  s->dst_port = key->dst_port;
  clib_memcpy (&kv.key, key, sizeof (*key));
  kv.value = (u64) (s - w->sessions);
  clib_bihash_add_del_48_8 (&w->v6_ht, &kv, 1 /* is_add */);
  *created = 1;
  return s;
}
