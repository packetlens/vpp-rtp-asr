/*
 * rtp_asr.c - VPP RTP-ASR plugin: init, config, plugin registration.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * TODO(v1): implement plugin_init, config parser, VLIB_PLUGIN_REGISTER.
 *           See FUNC_SPEC.md §9.
 */

#include <rtp_asr/rtp_asr.h>
#include <vnet/plugin/plugin.h>
#include <vpp/app/version.h>

rtp_asr_main_t rtp_asr_main;

/* TODO(v1):
 *   - config_fn parsing "sessions-per-worker", "expiry-seconds", "model",
 *     "model-variant", "vad", "emitter", "workers"
 *   - plugin_init: allocate per_worker vector sized to vlib_get_thread_main()->n_vlib_mains,
 *     init bihashes + pools, call rtp_asr_codec_init(),
 *     rtp_asr_sherpa_global_init(), rtp_asr_worker_pool_start(),
 *     rtp_asr_emitter_init(), rtp_asr_cli_init(), rtp_asr_api_init()
 *   - VLIB_PLUGIN_REGISTER with description "RTP-ASR tap (Moonshine/Sherpa-ONNX)"
 */
