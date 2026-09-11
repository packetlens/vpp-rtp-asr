/*
 * asr_worker.c - Linux worker thread pool + SPSC ring per VPP worker.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * One pthread per VPP worker. VPP graph node enqueues payload records into
 * the per-worker ring; the pthread dequeues, decodes, resamples, and (in
 * later stages) feeds Sherpa-ONNX + emits transcripts. The ring is a
 * fixed-size SPSC lock-free buffer using release/acquire atomics.
 *
 * Minimum-compile scope: ring + thread + loop that decodes+resamples and
 * logs a short summary via vlib_log. Sherpa/emitter integration lands in
 * later stages.
 */

#include <rtp_asr/rtp_asr.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define RTP_ASR_RING_SLOTS        1024   /* power of 2, ~s of 20ms frames */
#define RTP_ASR_RING_PAYLOAD_MAX  512    /* bytes; Opus VBR can be up to ~1500, trim */
#define RTP_ASR_PCM_SCRATCH       4096   /* samples at native rate — G.729 worst-case fits */
#define RTP_ASR_PCM_16K_SCRATCH   8192   /* samples @ 16 kHz after upsample */

typedef struct rtp_asr_ring_slot_
{
  u32 session_idx;       /* pool index in the owning worker */
  u32 payload_len;
  u32 rtp_ts;
  f64 vpp_time;
  u8  payload[RTP_ASR_RING_PAYLOAD_MAX];
} rtp_asr_ring_slot_t;

typedef struct rtp_asr_ring_
{
  rtp_asr_ring_slot_t *slots;           /* RTP_ASR_RING_SLOTS entries */
  _Atomic u32          head;            /* producer — VPP worker */
  _Atomic u32          tail;            /* consumer — Linux worker */
} rtp_asr_ring_t;

typedef struct rtp_asr_linux_worker_
{
  pthread_t       thread;
  u32             vpp_worker_idx;
  _Atomic int     shutdown;
  u64             processed;
  u64             decode_errors;
  u64             resample_errors;
  u64             segments_emitted;
} rtp_asr_linux_worker_t;

typedef struct rtp_asr_linux_pool_
{
  rtp_asr_linux_worker_t *workers;      /* vec */
  u32                     n_workers;
} rtp_asr_linux_pool_t;

/* ---------- ring accessor helpers ---------- */

static int
rtp_asr_ring_init (rtp_asr_worker_t *w)
{
  rtp_asr_ring_t *r = clib_mem_alloc (sizeof (*r));
  if (!r)
    return -1;
  clib_memset (r, 0, sizeof (*r));
  r->slots = clib_mem_alloc (sizeof (rtp_asr_ring_slot_t) * RTP_ASR_RING_SLOTS);
  if (!r->slots)
    {
      clib_mem_free (r);
      return -1;
    }
  atomic_store_explicit (&r->head, 0, memory_order_relaxed);
  atomic_store_explicit (&r->tail, 0, memory_order_relaxed);
  w->payload_ring = r;
  return 0;
}

int
rtp_asr_ring_enqueue (rtp_asr_worker_t *w, u32 session_idx,
		      const u8 *payload, u32 payload_len,
		      u32 rtp_ts, f64 vpp_time)
{
  rtp_asr_ring_t *r = w->payload_ring;
  if (PREDICT_FALSE (!r))
    return -1;

  u32 head = atomic_load_explicit (&r->head, memory_order_relaxed);
  u32 tail = atomic_load_explicit (&r->tail, memory_order_acquire);

  u32 next_head = (head + 1) & (RTP_ASR_RING_SLOTS - 1);
  if (PREDICT_FALSE (next_head == tail))
    return -1; /* ring full */

  if (payload_len > RTP_ASR_RING_PAYLOAD_MAX)
    payload_len = RTP_ASR_RING_PAYLOAD_MAX;

  rtp_asr_ring_slot_t *slot = &r->slots[head];
  slot->session_idx = session_idx;
  slot->payload_len = payload_len;
  slot->rtp_ts = rtp_ts;
  slot->vpp_time = vpp_time;
  clib_memcpy (slot->payload, payload, payload_len);

  atomic_store_explicit (&r->head, next_head, memory_order_release);
  return 0;
}

static int
rtp_asr_ring_dequeue (rtp_asr_worker_t *w, rtp_asr_ring_slot_t *out)
{
  rtp_asr_ring_t *r = w->payload_ring;
  if (!r)
    return -1;
  u32 tail = atomic_load_explicit (&r->tail, memory_order_relaxed);
  u32 head = atomic_load_explicit (&r->head, memory_order_acquire);
  if (tail == head)
    return -1; /* empty */
  *out = r->slots[tail];
  atomic_store_explicit (&r->tail, (tail + 1) & (RTP_ASR_RING_SLOTS - 1),
			 memory_order_release);
  return 0;
}

/* ---------- linux worker thread ---------- */

static rtp_asr_linux_pool_t rtp_asr_pool;

#define RTP_ASR_SEGMENT_SECONDS_DFLT 2.0f

static void
flush_segment (rtp_asr_linux_worker_t *lw, rtp_asr_session_t *s)
{
  if (!s->pcm_window || s->pcm_samples == 0)
    {
      s->pcm_samples = 0;
      return;
    }

  if (rtp_asr_sherpa_is_loaded ())
    {
      char text[2048];
      int n = rtp_asr_sherpa_decode_chunk (s->pcm_window, s->pcm_samples,
					   text, sizeof text);
      if (n > 0)
	{
	  rtp_asr_emitter_publish (s, text, /*is_final=*/1,
				   s->segment_rtp_ts_first,
				   s->segment_rtp_ts_last);
	  s->segments_emitted++;
	  lw->segments_emitted++;
	}
    }
  /* reset accumulator regardless of recognition outcome */
  s->pcm_samples = 0;
  s->segment_rtp_ts_first = 0;
}

static int
ensure_pcm_window (rtp_asr_session_t *s)
{
  if (s->pcm_window && s->pcm_window_cap > 0)
    return 0;
  rtp_asr_main_t *rm = &rtp_asr_main;
  f32 secs = rm->segment_seconds > 0.0f ? rm->segment_seconds
					: RTP_ASR_SEGMENT_SECONDS_DFLT;
  /* +50% headroom for a single-chunk overshoot */
  u32 cap = (u32) ((secs * 16000.0f) * 1.5f);
  if (cap < 32000) cap = 32000;
  s->pcm_window = clib_mem_alloc (cap * sizeof (f32));
  if (!s->pcm_window)
    return -1;
  s->pcm_window_cap = cap;
  s->pcm_samples = 0;
  return 0;
}

static void
process_slot (rtp_asr_worker_t *w, rtp_asr_linux_worker_t *lw,
	      const rtp_asr_ring_slot_t *slot,
	      f32 *pcm_native, f32 *pcm_16k)
{
  rtp_asr_session_t *s;
  if (pool_is_free_index (w->sessions, slot->session_idx))
    return;
  s = pool_elt_at_index (w->sessions, slot->session_idx);

  if (!s->codec_ctx)
    {
      if (rtp_asr_codec_setup_for_session (s) != 0)
	{
	  lw->decode_errors++;
	  return;
	}
    }

  u32 n_native = RTP_ASR_PCM_SCRATCH;
  if (rtp_asr_codec_decode (s, slot->payload, slot->payload_len,
			    pcm_native, &n_native) != 0)
    {
      lw->decode_errors++;
      return;
    }
  if (n_native == 0)
    return; /* decoder buffered — wait for next packet */

  u32 n_16k = RTP_ASR_PCM_16K_SCRATCH;
  if (rtp_asr_resample_to_16k (s, pcm_native, n_native, pcm_16k, &n_16k) != 0)
    {
      lw->resample_errors++;
      return;
    }
  if (n_16k == 0)
    return;

  if (ensure_pcm_window (s) != 0)
    return;

  /* Append to per-session PCM accumulator. If it would overflow the
   * configured window, flush the current segment first. */
  rtp_asr_main_t *rm = &rtp_asr_main;
  u32 segment_target = (u32) ((rm->segment_seconds > 0.0f
				   ? rm->segment_seconds
				   : RTP_ASR_SEGMENT_SECONDS_DFLT)
			      * 16000.0f);

  if (s->pcm_samples == 0)
    s->segment_rtp_ts_first = slot->rtp_ts;
  s->segment_rtp_ts_last = slot->rtp_ts;

  u32 space = s->pcm_window_cap - s->pcm_samples;
  u32 to_copy = n_16k < space ? n_16k : space;
  clib_memcpy (s->pcm_window + s->pcm_samples, pcm_16k,
	       to_copy * sizeof (f32));
  s->pcm_samples += to_copy;

  if (s->pcm_samples >= segment_target)
    flush_segment (lw, s);

  lw->processed++;
}

static void *
linux_worker_main (void *arg)
{
  rtp_asr_linux_worker_t *lw = (rtp_asr_linux_worker_t *) arg;
  rtp_asr_main_t *rm = &rtp_asr_main;
  rtp_asr_worker_t *w = vec_elt_at_index (rm->per_worker, lw->vpp_worker_idx);

  f32 *pcm_native = clib_mem_alloc (RTP_ASR_PCM_SCRATCH * sizeof (f32));
  f32 *pcm_16k    = clib_mem_alloc (RTP_ASR_PCM_16K_SCRATCH * sizeof (f32));
  if (!pcm_native || !pcm_16k)
    goto out;

  while (!atomic_load_explicit (&lw->shutdown, memory_order_acquire))
    {
      rtp_asr_ring_slot_t slot;
      int drained = 0;
      /* Drain up to ~64 slots then yield */
      for (int i = 0; i < 64; i++)
	{
	  if (rtp_asr_ring_dequeue (w, &slot) != 0)
	    break;
	  process_slot (w, lw, &slot, pcm_native, pcm_16k);
	  drained++;
	}
      if (!drained)
	usleep (500); /* 0.5 ms backoff when idle */
    }

out:
  if (pcm_native) clib_mem_free (pcm_native);
  if (pcm_16k)    clib_mem_free (pcm_16k);
  return NULL;
}

/* ---------- public API ---------- */

int
rtp_asr_worker_pool_start (u32 vpp_worker_count)
{
  rtp_asr_main_t *rm = &rtp_asr_main;
  u32 n = vpp_worker_count == 0 ? 1 : vpp_worker_count;

  /* Allocate per-VPP-worker rings up-front. */
  for (u32 i = 0; i < n; i++)
    {
      rtp_asr_worker_t *w = vec_elt_at_index (rm->per_worker, i);
      if (!w->payload_ring && rtp_asr_ring_init (w) != 0)
	return -1;
    }

  vec_validate (rtp_asr_pool.workers, n - 1);
  rtp_asr_pool.n_workers = n;

  for (u32 i = 0; i < n; i++)
    {
      rtp_asr_linux_worker_t *lw = &rtp_asr_pool.workers[i];
      lw->vpp_worker_idx = i;
      atomic_store_explicit (&lw->shutdown, 0, memory_order_relaxed);
      if (pthread_create (&lw->thread, NULL, linux_worker_main, lw) != 0)
	return -2;
    }
  return 0;
}

void
rtp_asr_worker_pool_stop (void)
{
  for (u32 i = 0; i < rtp_asr_pool.n_workers; i++)
    {
      rtp_asr_linux_worker_t *lw = &rtp_asr_pool.workers[i];
      atomic_store_explicit (&lw->shutdown, 1, memory_order_release);
    }
  for (u32 i = 0; i < rtp_asr_pool.n_workers; i++)
    {
      rtp_asr_linux_worker_t *lw = &rtp_asr_pool.workers[i];
      pthread_join (lw->thread, NULL);
    }
  vec_free (rtp_asr_pool.workers);
  rtp_asr_pool.n_workers = 0;
}

/* Accessors for CLI / stats */
u64
rtp_asr_worker_processed_total (void)
{
  u64 total = 0;
  for (u32 i = 0; i < rtp_asr_pool.n_workers; i++)
    total += rtp_asr_pool.workers[i].processed;
  return total;
}

u64
rtp_asr_worker_decode_errors_total (void)
{
  u64 total = 0;
  for (u32 i = 0; i < rtp_asr_pool.n_workers; i++)
    total += rtp_asr_pool.workers[i].decode_errors;
  return total;
}

u64
rtp_asr_worker_segments_total (void)
{
  u64 total = 0;
  for (u32 i = 0; i < rtp_asr_pool.n_workers; i++)
    total += rtp_asr_pool.workers[i].segments_emitted;
  return total;
}
