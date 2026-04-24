/*
 * sherpa_runtime.c - Sherpa-ONNX lifecycle: recognizer init, stream
 *                    create/destroy, model loading.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Default model: Moonshine v2 Base (MIT). Alternate variants:
 *   - moonshine-small-streaming.onnx (123 MB, better WER)
 *   - moonshine-medium-streaming.onnx (245 MB, best WER)
 * Alternate backend models reachable through the same Sherpa-ONNX config:
 *   - Parakeet-TDT ONNX
 *   - Zipformer-streaming
 *   - Whisper ONNX (offline/multilingual fallback)
 *
 * See FUNC_SPEC.md §7.
 */

#include <rtp_asr/rtp_asr.h>

/* TODO(v1):
 *   - rtp_asr_sherpa_global_init(model_path, variant):
 *       build SherpaOnnxOnlineRecognizerConfig with .model_config.moonshine
 *       = {preprocessor, encoder, uncached_decoder, cached_decoder}
 *       sample_rate=16000, feat_dim=80, greedy_search, enable_endpoint=1
 *       store in rtp_asr_main.recognizer (one shared across Linux workers;
 *       Sherpa-ONNX is thread-safe for stream-per-thread use).
 *   - rtp_asr_sherpa_attach_session(s):
 *       s->asr_stream = sherpa_onnx_create_online_stream(recognizer);
 *   - rtp_asr_sherpa_detach_session(s):
 *       sherpa_onnx_destroy_online_stream(s->asr_stream);
 *   - rtp_asr_sherpa_global_shutdown().
 */
