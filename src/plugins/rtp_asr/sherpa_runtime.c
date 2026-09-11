/*
 * sherpa_runtime.c - Sherpa-ONNX offline recognizer (Moonshine) lifecycle.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Design: sherpa-onnx's *online* recognizer doesn't support Moonshine in
 * the shipped C API (only Transducer / Paraformer / Zipformer2-CTC /
 * NeMo-CTC / T-One-CTC). Moonshine is supported on the *offline* path only.
 *
 * We use a chunk-based offline loop: the Linux worker accumulates up to
 * ~segment_seconds of 16 kHz PCM per session and calls
 * rtp_asr_sherpa_decode_chunk() once per chunk. Each call creates a
 * short-lived OfflineStream, pushes the chunk, decodes, reads the result,
 * and destroys the stream. This matches the MediaBridge 2-second-chunk
 * pattern (though we skip the WebSocket hop).
 *
 * A future stage can swap to an online backend (Zipformer-streaming) behind
 * the same API.
 *
 * Expected model directory layout (Moonshine-base):
 *   ${model_dir}/preprocess.onnx
 *   ${model_dir}/encode.onnx
 *   ${model_dir}/uncached_decode.onnx
 *   ${model_dir}/cached_decode.onnx
 *   ${model_dir}/tokens.txt
 */

/* Keep Sherpa-ONNX / FFmpeg headers before VPP's clib.h — avoids the
 * `always_inline` macro clash. */
#include <sherpa-onnx/c-api/c-api.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <rtp_asr/rtp_asr.h>

typedef struct
{
  const SherpaOnnxOfflineRecognizer *recognizer;
  char *model_dir;
  /* pinned so sherpa config structs can hold pointers to them */
  char  preprocess_path[512];
  char  encode_path[512];
  char  uncached_decode_path[512];
  char  cached_decode_path[512];
  char  tokens_path[512];
} rtp_asr_sherpa_state_t;

static rtp_asr_sherpa_state_t g_sherpa;

static int
file_exists (const char *p)
{
  struct stat st;
  return p && stat (p, &st) == 0;
}

/* Resolve a model file. Sherpa-ONNX publishes Moonshine with either:
 *   - `.int8.onnx` (quantized) — the default published tarballs
 *   - `.onnx`     (float)      — less common
 * Try the int8 variant first; fall back to plain. The preprocess file is
 * always `.onnx` in published bundles.  */
static int
resolve_onnx (char *out, size_t cap, const char *dir, const char *stem)
{
  char p[512];
  snprintf (p, sizeof p, "%s/%s.int8.onnx", dir, stem);
  if (file_exists (p))
    {
      snprintf (out, cap, "%s", p);
      return 0;
    }
  snprintf (p, sizeof p, "%s/%s.onnx", dir, stem);
  if (file_exists (p))
    {
      snprintf (out, cap, "%s", p);
      return 0;
    }
  return -1;
}

int
rtp_asr_sherpa_global_init (const char *model_dir)
{
  if (!model_dir || !*model_dir)
    return -1;

  snprintf (g_sherpa.preprocess_path, sizeof g_sherpa.preprocess_path,
	    "%s/preprocess.onnx", model_dir);
  snprintf (g_sherpa.tokens_path, sizeof g_sherpa.tokens_path,
	    "%s/tokens.txt", model_dir);

  if (!file_exists (g_sherpa.preprocess_path) ||
      !file_exists (g_sherpa.tokens_path))
    return -2;
  if (resolve_onnx (g_sherpa.encode_path, sizeof g_sherpa.encode_path,
		    model_dir, "encode") != 0)
    return -3;
  if (resolve_onnx (g_sherpa.uncached_decode_path,
		    sizeof g_sherpa.uncached_decode_path, model_dir,
		    "uncached_decode") != 0)
    return -4;
  if (resolve_onnx (g_sherpa.cached_decode_path,
		    sizeof g_sherpa.cached_decode_path, model_dir,
		    "cached_decode") != 0)
    return -5;

  SherpaOnnxOfflineRecognizerConfig cfg;
  memset (&cfg, 0, sizeof (cfg));
  cfg.feat_config.sample_rate = 16000;
  cfg.feat_config.feature_dim = 80;

  cfg.model_config.moonshine.preprocessor      = g_sherpa.preprocess_path;
  cfg.model_config.moonshine.encoder           = g_sherpa.encode_path;
  cfg.model_config.moonshine.uncached_decoder  = g_sherpa.uncached_decode_path;
  cfg.model_config.moonshine.cached_decoder    = g_sherpa.cached_decode_path;
  cfg.model_config.tokens                      = g_sherpa.tokens_path;
  cfg.model_config.num_threads                 = 1;
  cfg.model_config.provider                    = "cpu";
  cfg.model_config.debug                       = 0;

  cfg.decoding_method = "greedy_search";

  g_sherpa.recognizer = SherpaOnnxCreateOfflineRecognizer (&cfg);
  if (!g_sherpa.recognizer)
    return -3;

  g_sherpa.model_dir = strdup (model_dir);
  return 0;
}

int
rtp_asr_sherpa_is_loaded (void)
{
  return g_sherpa.recognizer != NULL;
}

int
rtp_asr_sherpa_decode_chunk (const f32 *pcm_16k, u32 n,
			     char *text_buf, u32 text_buf_cap)
{
  if (!g_sherpa.recognizer || !pcm_16k || n == 0 || !text_buf || text_buf_cap == 0)
    return -1;

  const SherpaOnnxOfflineStream *stream =
      SherpaOnnxCreateOfflineStream (g_sherpa.recognizer);
  if (!stream)
    return -2;

  SherpaOnnxAcceptWaveformOffline (stream, 16000, pcm_16k, (int) n);
  SherpaOnnxDecodeOfflineStream (g_sherpa.recognizer, stream);

  const SherpaOnnxOfflineRecognizerResult *r =
      SherpaOnnxGetOfflineStreamResult (stream);

  int written = 0;
  if (r && r->text && r->text[0])
    {
      size_t len = strlen (r->text);
      if (len >= text_buf_cap)
	len = text_buf_cap - 1;
      memcpy (text_buf, r->text, len);
      text_buf[len] = '\0';
      written = (int) len;
    }
  else if (text_buf_cap)
    {
      text_buf[0] = '\0';
    }

  if (r)
    SherpaOnnxDestroyOfflineRecognizerResult (r);
  SherpaOnnxDestroyOfflineStream (stream);
  return written;
}

void
rtp_asr_sherpa_global_shutdown (void)
{
  if (g_sherpa.recognizer)
    {
      SherpaOnnxDestroyOfflineRecognizer (g_sherpa.recognizer);
      g_sherpa.recognizer = NULL;
    }
  if (g_sherpa.model_dir)
    {
      free (g_sherpa.model_dir);
      g_sherpa.model_dir = NULL;
    }
}
