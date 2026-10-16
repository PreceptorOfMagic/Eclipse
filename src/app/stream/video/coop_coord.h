/*
 * Co-op split-screen: coordinator side of the two-process split.
 *
 * Owns the listening socket, accepts the secondary, and drains its access units
 * on a dedicated thread into a bounded queue. The video path pops panes without
 * blocking -- a late or absent secondary must never stall primary decode. Panes
 * remain in prediction order; only overflow evicts the oldest unit so an ordinary
 * arrival burst does not create a hole in the peer codec chain.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "coop_ipc.h"

/** The coordinator's listening socket. PER-VARIANT via -DCOOP_COORD_SOCKET (CMake appends the
 *  app-id suffix), so the live and beta installs never share it -- a socket left by one uid
 *  cannot be unlink()ed by the other in sticky-bit /tmp, which silently fails coop_coord_start
 *  (measured 2026-09-08). This literal is the fallback for builds/tests without the -D. */
#ifndef COOP_COORD_SOCKET
#define COOP_COORD_SOCKET "/tmp/aurora-coop.sock"
#endif

/** The co-op splice rules measured on PC hardware decoders (D3D11VA, NVDEC) during the Windows
 *  port. Desktop Linux decodes with the same class of hardware (NVDEC, VA-API) and needs the same
 *  rules: without them NVDEC mis-decoded the peer's tile (2026-09-29). webOS and Steam Link keep
 *  their own -- a keepref swap made the TV bootstrap come up BLACK (parameter register §11). */
#if defined(_WIN32) || (defined(OS_LINUX) && !defined(TARGET_WEBOS) && !defined(TARGET_STEAMLINK))
#define COOP_PC_DECODER 1
#endif
/* ANDROID: MediaCodec is a strict hardware decoder of the same class (on the NVIDIA Shield, Tegra's NVDEC), so
 * it takes the PC rules -- the TV-only exceptions above were measured on LG's decoder alone. To be confirmed on
 * a Shield with the co-op procedure in docs/coop-modes.md before any of the rules is narrowed. */
#if defined(TARGET_ANDROID)
#define COOP_PC_DECODER 1
#endif

/**
 * Start listening and drain incoming access units in the background.
 * Returns false if the socket cannot be created.
 */
bool coop_coord_start(const char *socket_path);

/** Reap abandoned children of this executable/socket, never a live coordinator's child. */
int coop_coord_reap_orphan_secondaries(const char *socket_path);

/** Stop draining and release the socket. Safe if start() failed. */
void coop_coord_stop(void);

/** True once a secondary has connected. */
bool coop_coord_connected(void);

/** Optional arrival notification. The callback only wakes the renderer; it must not feed
 * video. Clear it before stopping the owned renderer. coord_stop joins any in-flight call. */
void coop_coord_set_wakeup(void (*wake)(void));

/**
 * Copy the most recent secondary access unit into @p out.
 * Returns its length, or 0 if none is available or it has already been consumed.
 * Never blocks.
 */
size_t coop_coord_take_latest(unsigned char *out, size_t cap,
                              uint32_t *frame_out, bool *is_idr_out,
                              unsigned long *age_ms_out);

/**
 * Copy the most recent IDR access unit from the secondary into @p out, or 0 if
 * none has arrived. Not consumed -- the bootstrap retries until our own host
 * produces an IRAP to pair it with.
 */
size_t coop_coord_take_idr(unsigned char *out, size_t cap);

/**
 * Spawn this same binary as a headless secondary attached to @p socket_path.
 * Returns the child pid, or -1. The child creates no window and takes no
 * foreground -- see coop_secondary_run_headless.
 */
int coop_coord_spawn_secondary(const char *exe_path, const char *socket_path);

/** As above, also telling the child which host/app to stream and the primary's
 * resolved full-frame geometry. The headless child has no display from which to
 * derive an automatic stream size, so it must inherit the primary's value. */
int coop_coord_spawn_secondary_for(const char *exe_path, const char *socket_path,
                                   const char *host_uuid, int app_id,
                                   int stream_width, int stream_height);

/**
 * Non-blocking check on the spawned child. Returns true only after it exited and
 * writes a platform status to @p status_out (POSIX exit/1000+signal, or the raw
 * Win32 DWORD exit code). A boolean running/exited result is required because
 * Windows exception codes have their high bit set and become negative if they
 * are squeezed into the old int/-1-sentinel API.
 */
bool coop_coord_child_exited(uint32_t *status_out);

/** Reap the spawned child, if any. */
void coop_coord_reap_secondary(void);

/**
 * Ask the other host, through the secondary, for a keyframe. Used when our own
 * host emits an IRAP: that picture is ours in both slices, so the other pane is
 * overwritten and can only take over again at an IDR of its own. Non-blocking --
 * the request is posted and the drain thread sends it.
 */
void coop_coord_request_peer_idr(void);

/** Ask the peer for a keyframe but KEEP the sticky IDR we already hold. Use during the
 *  initial bootstrap, where the held IDR is the thing our next IRAP must pair with. */
void coop_coord_request_peer_idr_keep(void);

/** Ask the secondary to tear its streaming session down cleanly.  The drain
 *  thread sends the request so no other thread writes the framed video socket. */
void coop_coord_request_peer_shutdown(void);

/** Wait for the secondary to prove that its host stream delivered a frame.
 *  Returns true only for that READY signal; child exit, an explicit startup
 *  failure, and timeout all fail closed. */
bool coop_coord_wait_secondary_ready(unsigned timeout_ms);

/** The same wait, abandoned (false, with the start state left as it is) as soon as *@p cancel is set --
 *  for a wait that runs beside the stream instead of blocking its start (session_video.c, Android). */
bool coop_coord_wait_secondary_ready_cancellable(unsigned timeout_ms, const volatile int *cancel);

/** Stable user-facing explanation for the most recent readiness failure. */
const char *coop_coord_start_error_message(void);

/** Counters for diagnostics. */
unsigned coop_coord_received(void);
unsigned coop_coord_replaced(void);
/** Access units dropped for exceeding COOP_Q_SLOT_CAP, and how many of those were NOT IDRs.
 *  The drop site assumes an oversized AU is always an IDR (already held whole in the sticky
 *  slot). If that is ever wrong the pane vanishes with no eviction and no `skipped`, so the
 *  splice runs across the hole with the chain flag unset. A non-zero NONIDR count falsifies
 *  the assumption. */
unsigned coop_coord_oversize(void);
unsigned coop_coord_oversize_nonidr(void);
/** Peer-IDR requests actually put on the wire, and how many were throttled away. A large
 *  suppressed count is not a fault -- it is the rate limit doing its job -- but a large SENT
 *  count means the peer is being driven to near-all-intra, which starves the pane feed. */
unsigned coop_coord_idr_sent(void);
unsigned coop_coord_idr_suppressed(void);

/** Panes currently queued and unconsumed. Diagnostic: a queue that sits at its depth
 *  means the peer outruns us and the pane lags; one that sits at 0 means we outrun it. */
unsigned coop_coord_queued(void);

/** Discard all queued panes. Use when bootstrapping on the peer's IDR, so nothing older
 *  than that keyframe is fed afterwards. */
void coop_coord_flush(void);

/** Provenance of the pane most recently handed out by a take: the header the secondary sent
 *  (source frame, capture-derived PTS, its own receive/reassembly stamps, host latency and the
 *  provenance flags) plus the monotonic ms at which it finished arriving here.
 *  A SNAPSHOT -- read it immediately after the take it describes. Passover 07 §3.1. */
void coop_coord_last_taken_meta(coop_ipc_hdr_t *out, unsigned long *arrival_ms_out);

/** Link timing: secondary send-entry -> complete receipt here (dwell), and how long the
 *  secondary's own write_all() blocked, both last and worst. Passover 07 §2C's deciding
 *  measurement -- "long send stalls or growing receive-to-coordinator age during the symptom
 *  justify this work". Any of the four may be NULL. */
void coop_coord_ipc_timing(uint32_t *dwell_last, uint32_t *dwell_max,
                           uint32_t *send_block_last, uint32_t *send_block_max);

/** Newest peer frame number to have finished arriving in the coordinator, taken or not. */
uint32_t coop_coord_last_recv_frame(void);

/** The peer's own frame number for the sticky IDR, so a caller can anchor the expected
 *  sequence to the keyframe it bootstrapped on. */
uint32_t coop_coord_idr_frame(void);

/** Pop the oldest queued pane whose frame number is >= @p min_frame, discarding any older
 *  ones (they are stale). @p frame_out receives the frame number actually taken, which the
 *  caller MUST check against what it expected: a newer-than-expected pane means the peer's
 *  prediction chain has a hole and splicing it produces garbage. */
size_t coop_coord_take_next(unsigned char *out, size_t cap, uint32_t min_frame,
                            uint32_t *frame_out, unsigned *skipped_out);

#ifdef COOP_PC_DECODER
/** Would coop_coord_take_next(min_frame) hand out a peer IDR? Read-only: nothing is taken or
 *  discarded. Catch-up asks BEFORE taking, because it cannot use an IDR and a pane it has
 *  taken is gone (issue #27). */
bool coop_coord_next_is_idr(uint32_t min_frame);
#endif

/** As coop_coord_take_next(), plus the ms the taken pane spent in the coordinator between its
 *  COMPLETE arrival and this call.
 *
 *  WHY IT EXISTS. `coop_coord_take_latest()` has always reported that age; the AVC path uses
 *  take_next() and so could not see a pane that had been sitting. Passover 07 §2B names this as
 *  the deciding measurement for whether peer pictures wait on the primary's decode-unit
 *  callback -- the composite can only advance when the primary delivers, and nothing has ever
 *  measured how long a peer pane waits for that. */
size_t coop_coord_take_next_aged(unsigned char *out, size_t cap, uint32_t min_frame,
                                 uint32_t *frame_out, unsigned *skipped_out,
                                 unsigned long *age_ms_out);   /**< arrived before the previous was used */
