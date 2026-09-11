/*
 * resample.c - libswresample wrapper: codec-native rate → 16 kHz mono f32.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Moonshine expects 16 kHz mono f32. SwrContext is per-session, allocated
 * on first call, freed on session teardown. Design mirrors MediaBridge's
 * AudioDecoder::mSwr pattern.
 */

/* FFmpeg before VPP — see note in codec_decode.c. */
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#include <rtp_asr/rtp_asr.h>

#define RTP_ASR_ASR_SAMPLE_RATE 16000

typedef struct
{
  SwrContext *swr;
  u32         in_rate;
} swr_ctx_t;

static int
rtp_asr_resample_ensure_ctx (rtp_asr_session_t *s, u32 in_rate)
{
  swr_ctx_t *sc = s->swr_ctx;
  if (sc && sc->in_rate == in_rate)
    return 0;

  if (sc)
    {
      if (sc->swr)
	swr_free (&sc->swr);
    }
  else
    {
      sc = clib_mem_alloc (sizeof (*sc));
      clib_memset (sc, 0, sizeof (*sc));
      s->swr_ctx = sc;
    }
  sc->in_rate = in_rate;

#if LIBAVUTIL_VERSION_MAJOR >= 58 || defined(AV_CHANNEL_LAYOUT_MONO)
  AVChannelLayout in_layout = AV_CHANNEL_LAYOUT_MONO;
  AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_MONO;
  int rv = swr_alloc_set_opts2 (&sc->swr,
				&out_layout, AV_SAMPLE_FMT_FLT,
				RTP_ASR_ASR_SAMPLE_RATE,
				&in_layout, AV_SAMPLE_FMT_FLT,
				(int) in_rate, 0, NULL);
  if (rv < 0 || !sc->swr)
    return -1;
#else
  sc->swr = swr_alloc ();
  if (!sc->swr)
    return -1;
  av_opt_set_int (sc->swr, "in_channel_layout", AV_CH_LAYOUT_MONO, 0);
  av_opt_set_int (sc->swr, "out_channel_layout", AV_CH_LAYOUT_MONO, 0);
  av_opt_set_int (sc->swr, "in_sample_rate", (int) in_rate, 0);
  av_opt_set_int (sc->swr, "out_sample_rate", RTP_ASR_ASR_SAMPLE_RATE, 0);
  av_opt_set_sample_fmt (sc->swr, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
  av_opt_set_sample_fmt (sc->swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
#endif
  if (swr_init (sc->swr) < 0)
    {
      swr_free (&sc->swr);
      return -2;
    }
  return 0;
}

/* Resample mono f32 @ in_rate → mono f32 @ 16 kHz.
 * Updates *out_samples with the actual number of output samples produced. */
int
rtp_asr_resample_to_16k (rtp_asr_session_t *s,
			 const f32 *in, u32 in_samples,
			 f32 *out, u32 *out_samples)
{
  u32 in_rate = s->clock_rate > 0 ? s->clock_rate : 8000;

  /* Fast path: input already at 16 kHz — just memcpy. */
  if (in_rate == RTP_ASR_ASR_SAMPLE_RATE)
    {
      u32 n = in_samples;
      if (n > *out_samples) n = *out_samples;
      clib_memcpy (out, in, n * sizeof (f32));
      *out_samples = n;
      return 0;
    }

  if (rtp_asr_resample_ensure_ctx (s, in_rate) != 0)
    return -1;

  swr_ctx_t *sc = s->swr_ctx;
  const u8 *in_buf[] = { (const u8 *) in };
  u8 *out_buf[] = { (u8 *) out };

  int r = swr_convert (sc->swr, out_buf, (int) *out_samples, in_buf,
		       (int) in_samples);
  if (r < 0)
    return -2;
  *out_samples = (u32) r;
  return 0;
}

void
rtp_asr_resample_teardown (rtp_asr_session_t *s)
{
  swr_ctx_t *sc = s->swr_ctx;
  if (!sc)
    return;
  if (sc->swr)
    swr_free (&sc->swr);
  clib_mem_free (sc);
  s->swr_ctx = NULL;
}
