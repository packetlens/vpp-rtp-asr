/*
 * rtp_asr.c - VPP RTP-ASR plugin: init, config, interface enable/disable.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtp_asr/rtp_asr.h>
#include <vnet/plugin/plugin.h>
#include <vnet/vnet.h>
#include <vpp/app/version.h>
#include <stdlib.h>

rtp_asr_main_t rtp_asr_main;

static clib_error_t *
rtp_asr_config_fn (vlib_main_t *vm, unformat_input_t *input)
{
  rtp_asr_main_t *rm = &rtp_asr_main;
  u8 *tmp = 0;

  clib_warning ("rtp-asr config_fn entered");

  rm->sessions_per_worker = RTP_ASR_SESSIONS_PER_WORKER_DFLT;
  rm->session_expiry_seconds = RTP_ASR_SESSION_EXPIRY_DFLT;
  rm->rtp_port_min = 16384;
  rm->rtp_port_max = 32768;
  rm->rtp_port_well_known = 5004;
  rm->segment_seconds = 2.0f;
  rm->emitter_sink = 0; /* syslog */

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "sessions-per-worker %u", &rm->sessions_per_worker))
	;
      else if (unformat (input, "expiry-seconds %f",
			 &rm->session_expiry_seconds))
	;
      else if (unformat (input, "segment-seconds %f", &rm->segment_seconds))
	;
      else if (unformat (input, "rtp-port-range %u-%u", &rm->rtp_port_min,
			 &rm->rtp_port_max))
	;
      else if (unformat (input, "rtp-port-well-known %u",
			 &rm->rtp_port_well_known))
	;
      else if (unformat (input, "model-dir %s", &tmp))
	{
	  vec_add1 (tmp, 0);
	  rm->model_dir = (char *) tmp;
	  tmp = 0;
	}
      else if (unformat (input, "emitter-syslog"))
	rm->emitter_sink = 0;
      else if (unformat (input, "emitter-json-udp %s", &tmp))
	{
	  vec_add1 (tmp, 0);
	  rm->emitter_target = (char *) tmp;
	  rm->emitter_sink = 1;
	  tmp = 0;
	}
      else
	return clib_error_return (0, "unknown rtp-asr config: '%U'",
				  format_unformat_error, input);
    }
  return 0;
}
VLIB_CONFIG_FUNCTION (rtp_asr_config_fn, "rtp-asr");

static clib_error_t *
rtp_asr_init (vlib_main_t *vm)
{
  rtp_asr_main_t *rm = &rtp_asr_main;
  rm->vlib_main = vm;
  rm->vnet_main = vnet_get_main ();
  rm->log_class = vlib_log_register_class ("rtp-asr", 0);

  if (rm->sessions_per_worker == 0)
    rm->sessions_per_worker = RTP_ASR_SESSIONS_PER_WORKER_DFLT;
  if (rm->session_expiry_seconds == 0.0f)
    rm->session_expiry_seconds = RTP_ASR_SESSION_EXPIRY_DFLT;
  if (rm->rtp_port_min == 0)
    rm->rtp_port_min = 16384;
  if (rm->rtp_port_max == 0)
    rm->rtp_port_max = 32768;
  if (rm->rtp_port_well_known == 0)
    rm->rtp_port_well_known = 5004;

  u32 n_workers = vlib_num_workers ();
  u32 n_threads = (n_workers == 0) ? 1 : n_workers;
  vec_validate (rm->per_worker, n_threads - 1);

  for (u32 i = 0; i < n_threads; i++)
    {
      rtp_asr_worker_t *w = vec_elt_at_index (rm->per_worker, i);
      if (rtp_asr_session_table_init (w, rm->sessions_per_worker) != 0)
	return clib_error_return (0,
				  "session table init failed worker %u", i);
      w->initialized = 1;
    }

  rtp_asr_codec_init ();

  /* Sherpa-ONNX is optional — plugin still taps and counts RTP if no model
   * is configured. Non-fatal if load fails; logs a warning and transcripts
   * are skipped. */
  /* Plugins load after startup.conf is parsed, so VLIB_CONFIG_FUNCTION for
   * the "rtp-asr" stanza does not fire reliably. Accept an env-var fallback
   * so deployments can configure model/emitter without requiring the user
   * to `vppctl` after every start. The CLI (`rtp-asr set model ...`) is
   * the other supported path. */
  if (!rm->model_dir)
    {
      const char *env = getenv ("VPP_RTP_ASR_MODEL_DIR");
      if (env && *env)
	rm->model_dir = strdup (env);
    }
  if (!rm->emitter_target)
    {
      const char *env = getenv ("VPP_RTP_ASR_EMITTER_JSON_UDP");
      if (env && *env)
	{
	  rm->emitter_target = strdup (env);
	  rm->emitter_sink = 1;
	}
    }

  if (rm->model_dir && *rm->model_dir)
    {
      int rv = rtp_asr_sherpa_global_init (rm->model_dir);
      if (rv != 0)
	clib_warning (
	    "Sherpa-ONNX model load failed (%d) for dir '%s' — "
	    "transcription disabled",
	    rv, rm->model_dir);
      else
	clib_warning ("Sherpa-ONNX loaded: Moonshine model @ '%s'",
		      rm->model_dir);
    }

  if (rtp_asr_emitter_init (rm->emitter_sink, rm->emitter_target) != 0)
    vlib_log_warn (rm->log_class, "emitter init failed (sink=%u target='%s')",
		   rm->emitter_sink,
		   rm->emitter_target ? rm->emitter_target : "(null)");

  if (rtp_asr_worker_pool_start (n_threads) != 0)
    return clib_error_return (0, "asr worker pool start failed");

  vlib_log_info (rm->log_class,
		 "initialized: %u threads, %u sessions/worker, expiry=%.1fs, "
		 "rtp-ports={%u,%u-%u}",
		 n_threads, rm->sessions_per_worker,
		 rm->session_expiry_seconds, rm->rtp_port_well_known,
		 rm->rtp_port_min, rm->rtp_port_max);
  return 0;
}
VLIB_INIT_FUNCTION (rtp_asr_init);

VLIB_PLUGIN_REGISTER () = {
  .version = RTP_ASR_PLUGIN_VERSION,
  .description = "RTP tap + streaming ASR (Moonshine/Sherpa-ONNX)",
};

int
rtp_asr_interface_enable_disable (u32 sw_if_index, int enable)
{
  rtp_asr_main_t *rm = &rtp_asr_main;
  int rv;

  rv = vnet_feature_enable_disable ("ip4-unicast", "rtp-asr-tap",
				    sw_if_index, enable, 0, 0);
  if (rv)
    return rv;

  rv = vnet_feature_enable_disable ("ip6-unicast", "rtp-asr-tap",
				    sw_if_index, enable, 0, 0);
  if (rv)
    return rv;

  rm->enabled_interfaces =
      clib_bitmap_set (rm->enabled_interfaces, sw_if_index, enable ? 1 : 0);

  vlib_log_info (rm->log_class, "sw_if_index %u %s", sw_if_index,
		 enable ? "enabled" : "disabled");
  return 0;
}
