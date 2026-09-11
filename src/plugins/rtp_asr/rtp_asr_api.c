/*
 * rtp_asr_api.c - Binary API handlers for vpp-rtp-asr.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 */

#include <vnet/vnet.h>
#include <vlibapi/api.h>
#include <vlibmemory/api.h>
#include <rtp_asr/rtp_asr.h>

#include <rtp_asr.api_enum.h>
#include <rtp_asr.api_types.h>

#define REPLY_MSG_ID_BASE (rtp_asr_main.msg_id_base)
#include <vlibapi/api_helper_macros.h>

static void
vl_api_rtp_asr_interface_enable_disable_t_handler (
    vl_api_rtp_asr_interface_enable_disable_t *mp)
{
  vl_api_rtp_asr_interface_enable_disable_reply_t *rmp;
  int rv;

  u32 sw_if_index = clib_net_to_host_u32 (mp->sw_if_index);
  rv = rtp_asr_interface_enable_disable (sw_if_index,
					 (int) mp->enable_disable);
  REPLY_MACRO (VL_API_RTP_ASR_INTERFACE_ENABLE_DISABLE_REPLY);
}

static void
vl_api_rtp_asr_config_set_t_handler (vl_api_rtp_asr_config_set_t *mp)
{
  vl_api_rtp_asr_config_set_reply_t *rmp;
  /* Config-set is a noop stub in the minimum-compile scope.
   * TODO(v1): parse model_path / variant / vad_kind / emitter_sink into
   *           rtp_asr_main and trigger re-init paths. */
  int rv = 0;
  (void) mp;
  REPLY_MACRO (VL_API_RTP_ASR_CONFIG_SET_REPLY);
}

static void
vl_api_rtp_asr_stats_get_t_handler (vl_api_rtp_asr_stats_get_t *mp)
{
  vl_api_rtp_asr_stats_get_reply_t *rmp;
  vl_api_registration_t *reg;
  rtp_asr_main_t *rm = &rtp_asr_main;
  int rv = 0;

  u64 packets = 0, bytes = 0;
  u64 new_sessions = 0, expired = 0, malformed = 0, ring_drops = 0;

  for (u32 i = 0; i < vec_len (rm->per_worker); i++)
    {
      rtp_asr_worker_t *w = vec_elt_at_index (rm->per_worker, i);
      packets += w->packets;
      bytes += w->bytes;
      new_sessions += w->new_sessions;
      expired += w->expired_sessions;
      malformed += w->malformed_rtp;
      ring_drops += w->ring_full_drops;
    }

  reg = vl_api_client_index_to_registration (mp->client_index);
  if (!reg)
    return;

  rmp = vl_msg_api_alloc (sizeof (*rmp));
  clib_memset (rmp, 0, sizeof (*rmp));
  rmp->_vl_msg_id =
      clib_host_to_net_u16 (VL_API_RTP_ASR_STATS_GET_REPLY + REPLY_MSG_ID_BASE);
  rmp->context = mp->context;
  rmp->retval = clib_host_to_net_i32 (rv);
  rmp->packets = clib_host_to_net_u64 (packets);
  rmp->bytes = clib_host_to_net_u64 (bytes);
  rmp->new_sessions = clib_host_to_net_u64 (new_sessions);
  rmp->expired_sessions = clib_host_to_net_u64 (expired);
  rmp->malformed_rtp = clib_host_to_net_u64 (malformed);
  rmp->ring_full_drops = clib_host_to_net_u64 (ring_drops);
  rmp->transcripts_emitted = 0;
  rmp->inference_calls = 0;
  rmp->inference_latency_ms_sum = 0;

  vl_api_send_msg (reg, (u8 *) rmp);
}

static void
vl_api_rtp_asr_session_dump_t_handler (vl_api_rtp_asr_session_dump_t *mp)
{
  /* TODO(v1): walk per-worker session pools and send rtp_asr_session_details
   * messages. Deferred from the minimum-compile scope — CLI `show rtp-asr
   * sessions` already covers this for inspection. */
  (void) mp;
}

static void
vl_api_rtp_asr_transcripts_subscribe_t_handler (
    vl_api_rtp_asr_transcripts_subscribe_t *mp)
{
  vl_api_rtp_asr_transcripts_subscribe_reply_t *rmp;
  /* TODO(v1): register/unregister subscription; used only when the emitter
   * sink is set to "binary-api" (not default). */
  int rv = 0;
  (void) mp;
  REPLY_MACRO (VL_API_RTP_ASR_TRANSCRIPTS_SUBSCRIBE_REPLY);
}

#include <rtp_asr.api.c>

static clib_error_t *
rtp_asr_api_hookup (vlib_main_t *vm)
{
  rtp_asr_main.msg_id_base = setup_message_id_table ();
  return 0;
}

VLIB_API_INIT_FUNCTION (rtp_asr_api_hookup);
