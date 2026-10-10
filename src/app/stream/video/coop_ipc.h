/*
 * Co-op split-screen: access-unit transport between the two client processes.
 *
 * moonlight-common-c keeps connection state in file-scope globals, so two
 * LiStartConnection sessions cannot coexist in one process. The second session
 * therefore runs as a separate process -- but crucially it decodes NOTHING and
 * owns no surface or media pipeline. It is pure network receive plus this
 * transport, which is the one shape that never failed in any TV experiment.
 *
 * The secondary sends each reassembled access unit here; the coordinator splices
 * its slice 1 with the primary's slice 0 (coop_splice.h) and feeds the single
 * existing decoder.
 *
 * Framing is length-prefixed with a small header so a late or partial pane can
 * be detected rather than silently corrupting the spliced picture.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "CP3S" -- BUMPED FROM "CP2S" 2026-09-14 when the header grew the source-identity and
 * timing fields below. The magic is the ONLY thing that makes a version mismatch loud: the
 * two processes come from one ipk so they normally match, but a stale secondary left running
 * across an install would otherwise parse the new header as an old one and splice garbage.
 * A mismatch returns -2 (framing desync), which is already distinguished from an I/O error. */
#define COOP_IPC_MAGIC 0x43503353u   /* "CP3S" */

/*
 * The link also carries a thin CONTROL channel back to the secondary, marked by
 * a reserved frame number. It exists for one thing: when our own host emits an
 * IRAP, the composite picture becomes all-ours (a picture cannot be half-IDR),
 * so the other pane is overwritten and its prediction chain dies -- measured
 * offline as a permanent collapse of that pane. Recovering needs an IDR from the
 * OTHER host, and only the secondary process holds that connection.
 */
#define COOP_IPC_CTL_FRAME       0xFFFFFFFFu
#define COOP_IPC_CTL_REQUEST_IDR 1u
#define COOP_IPC_CTL_SHUTDOWN    2u
#define COOP_IPC_CTL_READY       3u
#define COOP_IPC_CTL_FAIL_CONFIG 4u
#define COOP_IPC_CTL_FAIL_HOST   5u
#define COOP_IPC_CTL_FAIL_QUERY  6u
#define COOP_IPC_CTL_FAIL_APP    7u
#define COOP_IPC_CTL_FAIL_SESSION 8u
#define COOP_IPC_CTL_FAIL_CONNECTION 9u
/* The secondary's launch request was refused (issue #33): BUSY = the host is already running a
 * DIFFERENT app ("Bad status 400: An app is already running on this host"), LAUNCH = any other
 * refusal. Without these the primary waited out its 30 s and blamed the connection. */
#define COOP_IPC_CTL_FAIL_BUSY   10u
#define COOP_IPC_CTL_FAIL_LAUNCH 11u

/* Absolute budget for one complete framed message (header plus payload).  A stream socket
 * that stops making progress mid-frame cannot be abandoned and reused because the next byte
 * would no longer be a header; after this deadline the caller must reset the whole link. */
#define COOP_IPC_IO_DEADLINE_MS 1000u

/* Provenance flags for the timing fields. Passover 07 §3.1: "Apollo derives RTP timestamps
 * from capture timestamps, but substitutes rate-control time for duplicate pictures; Moonlight
 * can also synthesize presentation time when RTP time is absent. Exclude/identify those
 * substitutions." We cannot see Apollo's substitution from here, but we CAN mark the two cases
 * the protocol itself makes visible, so a reader never averages a synthesized value with a
 * genuine one. */
#define COOP_IPC_F_HOST_MS_ABSENT  0x01u  /**< frameHostProcessingLatency == 0: Limelight.h:151
                                           *   says unavailable OR not applicable (repeated
                                           *   frame). The two are indistinguishable here. */
#define COOP_IPC_F_PTS_ZERO        0x02u  /**< presentationTimeMs == 0 -- either the session's
                                           *   first captured frame or an absent RTP time. */

/* THE PEER'S SOURCE IDENTITY AND TIMING, CARRIED ACROSS THE LINK -- passover 07 §3.1.
 *
 * Before this, the only thing that crossed was `frame`, so once a pane reached the coordinator
 * nothing could say when the PEER first saw it. Every age we could compute started at complete
 * arrival in the coordinator, which excludes peer receive, reassembly and the IPC hop -- the
 * exact regions passover 07 §1 says "seconds can still accumulate" in.
 *
 * CLOCK DOMAIN. `recv_ms` and `reasm_ms` are moonlight's LiGetMillis() epoch, which is
 * per-process: the secondary's values are NOT comparable to the primary's by subtraction.
 * They are carried so DIFFERENCES within the peer's own stream are visible (reassembly cost,
 * inter-arrival), never to be differenced against a primary stamp. `send_enter_mono_ms` is
 * CLOCK_MONOTONIC, which IS shared -- both processes run on the TV -- and is the one stamp the
 * coordinator may legitimately subtract from its own monotonic receipt time. */
typedef struct {
    uint32_t magic;
    uint32_t length;      /**< access-unit payload bytes following the header */
    uint32_t frame;       /**< source frame number, for pairing and drop detection */
    uint8_t  is_idr;      /**< 1 if this AU is an IRAP picture */
    uint8_t  flags;       /**< COOP_IPC_F_* provenance bits */
    uint16_t host_10us;   /**< DU frameHostProcessingLatency, 1/10 ms units, 0 = absent */
    uint32_t pts_ms;      /**< DU presentationTimeMs -- capture-derived, epoch = first frame */
    uint32_t recv_ms;     /**< DU receiveTimeMs -- first packet, SECONDARY's LiGetMillis epoch */
    uint32_t reasm_ms;    /**< DU enqueueTimeMs -- completed reassembly, same epoch */
    uint32_t send_enter_mono_ms; /**< CLOCK_MONOTONIC on entry to coop_ipc_send_au */
    uint32_t prev_send_ms;       /**< how long the PREVIOUS AU's send_au call took, ms.
                                  *   Passover 07 §2C wants entry AND exit of the send; exit
                                  *   is not known until after the header is on the wire, so
                                  *   it rides on the NEXT message rather than adding a round
                                  *   trip. A blocked writer shows here one AU late. */
} coop_ipc_hdr_t;

/** Create the listening socket (coordinator side). Returns fd or -1. */
intptr_t coop_ipc_listen(const char *path);

/** Accept one secondary connection. Returns fd or -1; honours the listener's blocking mode. */
intptr_t coop_ipc_accept(intptr_t listen_fd);

/** Connect to the coordinator (secondary side). Returns fd or -1. */
intptr_t coop_ipc_connect(const char *path);

/** Optional per-AU metadata for coop_ipc_send_au(). NULL sends zeros in every field. */
typedef struct {
    uint32_t pts_ms;
    uint32_t recv_ms;
    uint32_t reasm_ms;
    uint16_t host_10us;
    uint8_t  flags;
} coop_ipc_meta_t;

/** Send one access unit. The socket must be non-blocking. Returns true only if the whole
 *  header and payload were written before COOP_IPC_IO_DEADLINE_MS. On false, reset the link:
 *  a partial stream message cannot be resumed as a later frame. */
bool coop_ipc_send_au(intptr_t fd, uint32_t frame, bool is_idr,
                      const unsigned char *data, size_t len,
                      const coop_ipc_meta_t *meta);

/**
 * Receive one access unit into @p buf.
 * The socket must be non-blocking. Once any part of a message is consumed, the complete
 * header and payload share COOP_IPC_IO_DEADLINE_MS; expiry is a fatal link error.
 * Returns the payload length, 0 if nothing is pending (non-blocking), or
 * -1 if the peer is gone/I/O failed, or -2 for a framing-magic mismatch.
 */
long coop_ipc_recv_au(intptr_t fd, unsigned char *buf, size_t cap,
                      uint32_t *frame_out, bool *is_idr_out);

/**
 * As coop_ipc_recv_au(), but also hands back the header's source-identity and timing fields.
 * Separate entry point so the control-channel and reconnect call sites keep their old shape.
 */
long coop_ipc_recv_au_meta(intptr_t fd, unsigned char *buf, size_t cap,
                           uint32_t *frame_out, bool *is_idr_out,
                           coop_ipc_hdr_t *hdr_out);

/**
 * Send a one-byte control message (COOP_IPC_CTL_*) upstream to the secondary.
 * Framed exactly like an access unit so the peer's reader needs no special case
 * beyond checking the frame number.
 */
bool coop_ipc_send_ctl(intptr_t fd, uint8_t code);

/** Duration of the LAST completed coop_ipc_send_au() call, ms (secondary side). */
uint32_t coop_ipc_last_send_ms(void);

/** Put the socket in non-blocking mode so the video path never stalls on it. */
bool coop_ipc_set_nonblocking(intptr_t fd);

/* WHY THE LAST RECEIVE FAILED. The primary logs only "io error/eof" for every rc=-1, and
 * measured 2026-08-03 that is 21 link teardowns in one session with BOTH processes alive and
 * the secondary reporting only EPIPE in return -- i.e. each end blames the other and neither
 * says why. These record the errno and the exact return site so the cause is nameable.
 * Set on every failing path of the receive; read straight after a negative return. */
extern int coop_ipc_last_errno;
extern const char *coop_ipc_last_stage;
