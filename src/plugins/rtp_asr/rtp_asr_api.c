/*
 * rtp_asr_api.c - Binary API handler stubs (generated .api → message IDs).
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * See rtp_asr.api for message definitions and FUNC_SPEC.md §9.1.
 */

#include <rtp_asr/rtp_asr.h>
#include <vlibapi/api.h>
#include <vlibmemory/api.h>

/* TODO(v1):
 *   - include auto-generated <rtp_asr/rtp_asr.api_enum.h> and
 *     <rtp_asr/rtp_asr.api_types.h>
 *   - vl_api_rtp_asr_interface_enable_disable_t_handler
 *   - vl_api_rtp_asr_config_set_t_handler
 *   - vl_api_rtp_asr_stats_get_t_handler (build reply with counters)
 *   - vl_api_rtp_asr_session_dump_t_handler (streaming reply)
 *   - vl_api_rtp_asr_transcripts_subscribe_t_handler (optional live stream)
 *   - REPLY_MSG_ID_BASE = rtp_asr_main.msg_id_base, use REPLY_MACRO helpers.
 */

clib_error_t *
rtp_asr_api_init (vlib_main_t *vm)
{
  /* TODO(v1): setup_message_id_table + register handlers */
  (void) vm;
  return 0;
}
