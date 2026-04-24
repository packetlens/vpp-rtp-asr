/*
 * asr_worker.c - Linux worker thread pool that drives codec decode,
 *                resample, VAD, and Sherpa-ONNX inference off-path.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Each VPP worker is statically bound at plugin init to exactly one Linux
 * worker thread (round-robin). The VPP worker writes payload entries into
 * an SPSC ring; this worker dequeues and performs all heavy work.
 *
 * See FUNC_SPEC.md §4 for the main-loop contract.
 *
 * Upstream candidate: the generic "async off-path worker helper" (SPSC ring
 * + Linux thread dispatch) is proposed for fd.io's src/vppinfra/ as a
 * shared utility reusable by other heavy-work plugins. See
 * docs/upstream-split.md.
 */

#include <rtp_asr/rtp_asr.h>
#include <pthread.h>

/* TODO(v1):
 *   - rtp_asr_worker_pool_start():
 *       size = min(online_cpus - vpp_workers, 4) or user-configured
 *       spawn pthread per worker; pin with pthread_setaffinity_np
 *       each thread runs linux_worker_main()
 *   - linux_worker_main():
 *       while (!shutdown) {
 *         for each assigned ring:
 *           for i<BATCH: entry = ring_dequeue; handle_payload(entry);
 *         run_aging_sweep (every ~1s wall-clock);
 *         short_sleep_if_idle();
 *       }
 *   - handle_payload():
 *       decode → resample → VAD → sherpa accept+decode → poll → emit on edge
 *   - rtp_asr_worker_pool_stop(): set flag, join threads.
 */
