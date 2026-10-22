#include "coop_audio.h"
#include "coop_audio_dedupe.h"
#include "coop_audio_pgate.h"
#include "coop_audio_opus.h"
#include "coop_audio_fifo.h"
#include "coop_audio_output.h"
#include "coop_audio_pull.h"

#include "app.h"
#include "app_launch.h"
#include "backend/pcmanager.h"
#include "platform/this_pc.h"
#include "stream/coop_msg_channel.h"
#include "stream/video/coop_secondary.h"
#include "stream/video/coop_session.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <poll.h>
#include <opus_multistream.h>
#include <SDL.h>
#include <ss4s.h>

#define COOP_AUDIO_MAGIC 0x43413453u   /* "CA4S": epoch + acknowledged indexed PCM */

enum {
    COOP_AUDIO_MSG_CONFIG = 0,
    COOP_AUDIO_MSG_PCM = 1,
    COOP_AUDIO_MSG_ACK = 2,
};

typedef struct {
    uint32_t magic;
    uint16_t type;
    uint16_t frames;     /**< PCM frames (samples per channel) in the payload */
    uint32_t rate;       /**< config message only */
    uint16_t channels;   /**< config message only */
    uint16_t reserved;
    uint64_t ready_us;   /**< Same-machine monotonic time immediately after decoding. */
    uint64_t first_frame; /**< PCM start, or ACK exclusive accepted end. */
    uint64_t epoch;       /**< Stable source identity for one decoder lifetime. */
} coop_audio_hdr_t;

/* Stereo only. Surround is not mixed: two 5.1 beds summed into one is not a
 * thing anyone wants out of a TV, and session.c asks both hosts for stereo on a
 * co-op launch precisely so this path always has two matching streams. */
#define COOP_AUDIO_MAX_CHANNELS 2
/* Longest Opus frame we will handle in one call (20 ms at 48 kHz). Apollo sends
 * 5 ms, so this is 4x headroom. */
#define COOP_AUDIO_MAX_FRAMES 960

/* Jitter ring. Half a second is far more than we would ever let build up -- it
 * is sized so a burst can never wrap, while the depth cap below is what actually
 * bounds the latency. */
#define COOP_AUDIO_RING_FRAMES 24000
/* Depth we aim to hold before mixing starts, and the depth above which we throw
 * the oldest away. 40 ms absorbs the IPC hop plus the two hosts' network jitter;
 * dropping starts 80 ms above that, before the peer's audio could feel detached
 * from its picture -- drift is corrected by dropping, never by growing.
 *
 * This is the one number that can only be settled BY EAR (too small = the peer
 * crackles, too large = it lags its own pane), so it is a sentinel: put a depth
 * in milliseconds in /tmp/coop-audio-depth and reconnect. */
#define COOP_AUDIO_DEFAULT_DEPTH_MS 40

/* Produced-ahead pull (Android): 5 ms blocks the worker keeps mixed for the device, the whole of the latency this mode
 * adds. 15 ms (3) assumed a gate block costs ~3 ms at the SHIELD TV's full clock (1.2 M cycles on the bench, spikes
 * ~2.5x); on a live co-op mission it cost ~7 M (the worker held 70% of a 2.0 GHz core: the gate's offset-search FFT,
 * ingest and band matching), its spikes outran 15 ms and the device played holes of silence, the likely source of the
 * crackle the user heard (2026-10-22). 50 ms is a STOPGAP that lets the spikes average out while the gate is made
 * cheaper; whether it is acceptable is the user's call by ear. */
#define COOP_AUDIO_PULL_AHEAD_BLOCKS 10

#if TARGET_ANDROID
#include <sys/resource.h>
#include <sys/syscall.h>
/* Android: the nice value of the threads co-op audio waits on -- the primary's ingress worker (it takes the peer's
 * audio, acknowledges it, and mixes and gates both for the device) and the secondary's sender: Android's value for an
 * audio thread (THREAD_PRIORITY_AUDIO, which SDLAudioManager gives its own). SDL_SetThreadPriority cannot raise them:
 * SDL 2.32.10 leaves __LINUX__ undefined on Android, so it only re-sets SCHED_OTHER at priority 0. Measured on a
 * SHIELD TV co-op mission (2026-10-22): for 4.5 minutes the worker fell behind -- the device played silence for what
 * it had not mixed, the peer's audio waited 140-220 ms for its acknowledgement instead of ~25, and a burst of
 * background app starts cost 0.94 s of it -- while the video and the peer's network ran steady. Whether this value
 * cures that is for the next run to show: [COOP-AUD.priority] logs the value each thread got. */
#define COOP_AUDIO_ANDROID_NICE (-16)
#endif

/* Per-source gain in MIX mode. Two independent game mixes are each mastered to
 * use the whole scale, so summing them at unity would sit on the limiter and
 * pump. -2.5 dB each keeps the sum near full scale on real material and leaves
 * the limiter as a safety net rather than a permanent effect. */
#define COOP_AUDIO_MIX_GAIN 0.75f
/* Limiter ceiling, just under full scale so the clamp below never actually bites. */
#define COOP_AUDIO_LIMIT 32000.0f

/* Only the output worker renders when clocked. Decode callbacks only append. */
static SDL_Thread *output_thread;
static SDL_atomic_t output_run;
static SDL_mutex *input_lock;
static SS4S_Player *output_player;
static coop_audio_fifo_t input_fifo[2];
static bool output_clocked;
static coop_output_t output_owner;
static unsigned output_block_frames;
static bool output_pull;
static bool pull_device_opened;
static coop_audio_pull_t pull_owner;
static SDL_atomic_t pull_policy;
static atomic_ullong pull_self_accepted, pull_self_rejected;
static atomic_ullong pull_peer_accepted;
static SDL_Thread *pull_service_thread;
static SDL_atomic_t pull_link;
enum { PULL_RECORDS = 64 };
typedef struct {
    unsigned frames; /* zero denotes a callback-consumption snapshot */
    uint64_t index, dropped;
    unsigned depth[2];
    coop_audio_pull_stats_t stats;
    int16_t pcm[3][COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS];
} pull_record_t;
static pull_record_t *pull_records;
static atomic_uint pull_read, pull_write;
static uint64_t pull_dropped, pull_capture_next;
static bool pull_capture;
static coop_audio_pull_stats_t pull_last;

static bool coord_active = false;
static intptr_t coord_listen_fd = -1;
static intptr_t coord_peer_fd = -1;
static coop_msg_channel_t coord_channel = COOP_MSG_CHANNEL_INITIALIZER;
static int coord_rate = 0, coord_channels = 0;
static bool peer_format_ok = false;
static bool peer_format_logged = false;

static int16_t *ring = NULL;
static unsigned ring_head = 0, ring_tail = 0;   /* in frames */
static bool ring_primed = false;
static unsigned ring_prime_frames = 0, ring_max_frames = 0;
static uint64_t ring_ready_us[COOP_AUDIO_RING_FRAMES];
static uint64_t timing_start_us, timing_received_frames, timing_consumed_frames;
static uint64_t timing_trimmed_frames, timing_overflow_frames, timing_short_frames;
static uint64_t timing_plc_frames, timing_ipc_age_max_us, timing_queue_age_max_us;
static uint64_t timing_link_discarded_frames, timing_gap_frames, timing_next_frame;
static uint64_t timing_invalid_timestamp_frames;
static bool timing_have_sequence;
static uint64_t peer_epoch, peer_duplicates;
static bool peer_ack_pending, peer_pcm_pending;
static coop_audio_hdr_t peer_pending_hdr;
static int16_t peer_pending_pcm[COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS];

/* CLOCK_MONOTONIC is shared by local processes. The Windows compatibility
 * implementation uses QPC/QPF, not SDL's process-relative millisecond clock. */
static uint64_t audio_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
}

static void timing_log(void);

static int listen_mode = COOP_AUDIO_MIX;
/* A host that is THIS PC plays its sound on this PC's own speakers (session_worker.c asks it for host
 * audio), so its streamed copy is never mixed in -- it would double up. Decided at co-op start. */
static bool self_is_this_pc = false, peer_is_this_pc = false;
static unsigned mode_recheck = 0;
static float limiter_gain = 1.0f;
static float output_role_gain[2] = {COOP_AUDIO_MIX_GAIN, COOP_AUDIO_MIX_GAIN};
static float output_filter_wet;
static uint64_t output_queue_peak;
static uint64_t output_source_next[2], output_invalid_index_blocks[2];
static bool output_source_indexed[2];
/* Duplicate suppression (coop_audio_dedupe.h). The largest offset looked for between the two hosts' copies of a
 * sound: two separate streams plus each game's own timing; 400 ms covers the ring's worst case (prime + 80 ms) with
 * room for game-side differences. UNMEASURED for real games -- /tmp/coop-audio-dump exists to measure it. */
#define COOP_AUDIO_DEDUPE_MAX_LAG_MS 400
static coop_dedupe_t *dedupe = NULL;
static bool dedupe_on = false;
/* The PASSIVE GATE (coop_audio_pgate.h -- the user's design, built and tuned 2026-10-05, accepted by the user in a
 * Windows gameplay test the same day) is the DEFAULT filter for "play shared sounds once" since 2026-10-06. Its latency
 * is 5 ms (the dedupe filter's 10.7 ms). A/B SWITCH: /tmp/coop-audio-gate "off" or "0" returns to the single-offset
 * filter above; absent, "on" or any other token = the passive gate. Both are governed by the same setting. */
#define COOP_AUDIO_GATE_MAX_LAG_MS 1000
static coop_pgate_t *gate = NULL;
static bool gate_on = true;
/* "Play shared sounds once" quality (app_settings coop_audio_filter): Accurate = look-ahead, no predictions. Every gate
 * is made by gate_new() so a recreated gate keeps it. The pull/clocked output drives the gate block by block, which
 * cannot hold audio back, so there the gate settles to Fast by itself (coop_pgate_create_quality). */
static bool gate_accurate = true;
static int gate_ahead_ms = COOP_PGATE_ACCURATE_LOOKAHEAD_MS;   /* Accurate's look-ahead, 0..100 (coop_audio_filter_ahead_ms) */
static int clamp_ahead(int ms) { return ms < 0 ? 0 : ms > 100 ? 100 : ms; }
static coop_pgate_t *gate_new(int rate, int channels) {
    return coop_pgate_create_quality(rate, channels, COOP_AUDIO_GATE_MAX_LAG_MS,
                                     gate_accurate ? COOP_PGATE_ACCURATE : COOP_PGATE_FAST, gate_ahead_ms);
}
static bool filter_active = false;   /* the last mix call ran a duplicate filter (gates the pgate: stats line) */

static unsigned stat_received = 0, stat_sent = 0, stat_dropped = 0;
static unsigned stat_underruns = 0, stat_trims = 0, stat_feeds = 0;
/* Peak level of each source over the reporting interval. COUNTERS CANNOT TELL
 * SILENCE FROM SOUND: Opus keeps sending frames when a game is quiet, so a
 * climbing message count proves the link works and proves nothing about audio.
 * These are the audio equivalent of looking at the panel. */
static int32_t peak_self = 0, peak_peer = 0, peak_out = 0;
/* The same three over the WHOLE session, logged when it ends: a silent mix shows up as "peer=0 out=0" even when the
 * session was too short or too quiet for a periodic line. */
static int32_t sess_peak_self = 0, sess_peak_peer = 0, sess_peak_out = 0;
static unsigned sess_frames = 0;

static void coop_audio_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void coop_audio_log(const char *fmt, ...) {
    FILE *f = fopen(coop_secondary_mode() != NULL ? AURORA_SECONDARY_LOG : AURORA_DIAG_LOG, "a");
    if (f == NULL) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

#if TARGET_ANDROID
static void android_audio_priority(const char *thread) {
    const pid_t tid = (pid_t) syscall(SYS_gettid);
    errno = 0;
    const int before = getpriority(PRIO_PROCESS, (id_t) tid);
    const int rc = setpriority(PRIO_PROCESS, (id_t) tid, COOP_AUDIO_ANDROID_NICE);
    const int err = rc != 0 ? errno : 0;
    const int after = getpriority(PRIO_PROCESS, (id_t) tid);
    coop_audio_log("[COOP-AUD.priority] thread=%s tid=%d nice %d -> %d (asked %d)%s%s", thread, (int) tid, before,
                   after, COOP_AUDIO_ANDROID_NICE, err ? ": " : "", err ? strerror(err) : "");
}
#endif

/* Wall-clock stamp for the periodic diagnostic lines, so a user's notes or a recording ("it crackled at 12:41")
 * can be lined up with the counters. Local time plus epoch ms; never used for any audio decision. */
static void wall_clock(char *buf, size_t n, unsigned long long *epoch_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const time_t t = ts.tv_sec;
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    snprintf(buf, n, "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);
    *epoch_ms = (unsigned long long) ts.tv_sec * 1000ull + (unsigned long long) (ts.tv_nsec / 1000000L);
}

/* Legacy (push) coordinator, per ~5 s window, reset after each [COOP-AUD.ingest] line. The secondary stops sending
 * while its stop-and-wait ACK is outstanding and discards decoded audio once 500 ms is unacknowledged, so the
 * question for each PC2 audio hole is how long THIS side went without collecting. */
static uint64_t win_last_mix_us, win_mix_gap_max_us, win_ipc_age_max_us, win_queue_age_max_us;
static unsigned win_mix_gaps_50ms, win_mix_gaps_200ms, win_commit_refused, win_accepts, win_drain_max_msgs;
static unsigned drain_msgs_call;
static unsigned win_gate_dropouts, win_gate_admitted, win_gate_evicted, win_gate_pred_failed, win_gate_twin_splits;
static unsigned win_gate_side_switches, win_gate_sticky_holds;
static bool win_gate_seen;
/* Shared-content restore (opt-in A/B, legacy path only -- the pull path runs the gate inside the device
 * callback): /tmp/coop-gate-restore holds a dB value 0..6; absent = off; present but unreadable = keep. */
static float gate_restore_db = 0.0f;
static const void *gate_restore_target = NULL;

static bool coop_audio_fill_addr(struct sockaddr_un *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
#ifdef ECLIPSE_RUNTIME_ACTIVE
    return eclipse_runtime_socket_path(COOP_AUDIO_SOCKET, addr->sun_path, sizeof(addr->sun_path)) == 0;
#else
    strncpy(addr->sun_path, COOP_AUDIO_SOCKET, sizeof(addr->sun_path) - 1);
    return true;
#endif
}

/* --------------------------------------------------------------------------
 * Listen mode
 * ------------------------------------------------------------------------ */

/* The setting is read from the config, but a sentinel overrides it so the three
 * modes can be compared BY EAR in one session without a rebuild or even a
 * reconnect -- same pattern as /tmp/coop-fps. Re-read about once a second. */
static void coop_audio_refresh_mode(void) {
    if (mode_recheck++ % 200u != 0u) {
        return;
    }
    const int policy = SDL_AtomicGet(&pull_policy);
    int current_mode = output_pull ? policy & 3 : listen_mode;
    bool current_dedupe = output_pull ? (policy & 4) != 0 : dedupe_on;
    bool current_gate = output_pull ? (policy & 8) != 0 : gate_on;
    int mode = app_configuration != NULL ? app_configuration->coop_audio_mode : COOP_AUDIO_MIX;
    FILE *f = fopen("/tmp/coop-audio", "r");
    if (f != NULL) {
        char word[16] = {0};
        if (fscanf(f, "%15s", word) == 1) {
            if (strcmp(word, "primary") == 0 || strcmp(word, "1") == 0) {
                mode = COOP_AUDIO_PRIMARY_ONLY;
            } else if (strcmp(word, "secondary") == 0 || strcmp(word, "2") == 0) {
                mode = COOP_AUDIO_SECONDARY_ONLY;
            } else {
                mode = COOP_AUDIO_MIX;
            }
        }
        fclose(f);
    }
    const int filter = app_configuration != NULL ? app_configuration->coop_audio_filter : COOP_AUDIO_FILTER_ACCURATE;
    bool want_dedupe = filter != COOP_AUDIO_FILTER_OFF;
    bool want_accurate = filter != COOP_AUDIO_FILTER_FAST;
    int want_ahead = app_configuration != NULL ? app_configuration->coop_audio_filter_ahead_ms : COOP_PGATE_ACCURATE_LOOKAHEAD_MS;
    FILE *df = fopen("/tmp/coop-audio-dedupe", "r");
    if (df != NULL) {
        char word[16] = {0};
        if (fscanf(df, "%15s", word) == 1) {
            want_dedupe = !(strcmp(word, "off") == 0 || strcmp(word, "0") == 0);
            int ms = 0;
            if (strcmp(word, "fast") == 0) want_accurate = false;
            else if (strcmp(word, "accurate") == 0) want_accurate = true;
            else if (sscanf(word, "accurate:%d", &ms) == 1) { want_accurate = true; want_ahead = ms; }
        } else {
            want_dedupe = current_dedupe;   /* empty or mid-write: keep what we have */
            want_accurate = gate_accurate;
            want_ahead = gate_ahead_ms;
        }
        fclose(df);
    } else if (errno != ENOENT) {
        /* The file is there but could not be opened just now: KEEP the current state. Measured 10-04 on Windows:
         * the sentinel said "off", one read ~1 s later failed, and falling back to the setting switched the filter
         * back on for the rest of a live A/B -- only an absent file means "use the setting". */
        want_dedupe = current_dedupe;
        want_accurate = gate_accurate;
        want_ahead = gate_ahead_ms;
    }
    want_ahead = clamp_ahead(want_ahead);
    if (!output_pull && (want_accurate != gate_accurate || (want_accurate && want_ahead != gate_ahead_ms))) {
        /* legacy path: the gate lives on this thread */
        coop_audio_log("[COOP-AUD] shared-sound filter quality -> %s (look-ahead %d ms)",
                       want_accurate ? "accurate" : "fast", want_accurate ? want_ahead : 0);
        gate_accurate = want_accurate;
        gate_ahead_ms = want_ahead;
        if (coord_rate > 0 && !output_clocked && gate != NULL) {
            coop_pgate_destroy(gate);
            gate = gate_new(coord_rate, coord_channels);
        }
    }
    bool want_gate = current_gate;
    FILE *gf = fopen("/tmp/coop-audio-gate", "r");
    if (gf != NULL) {
        char word[16] = {0};
        if (fscanf(gf, "%15s", word) == 1) {
            want_gate = !(strcmp(word, "off") == 0 || strcmp(word, "0") == 0);
        }
        fclose(gf);
    } else if (errno == ENOENT) {
        want_gate = true;   /* no A/B switch: the default filter */
    }
    if (want_gate != current_gate) {
        coop_audio_log("[COOP-AUD] echo filter -> %s", want_gate ? "PASSIVE GATE" : (output_clocked ? "bypass (clocked path)" : "dedupe (single offset)"));
        current_gate = want_gate;
        if (coord_rate > 0 && !output_clocked) {   /* legacy filters hold output */
            if (current_gate) {
                coop_pgate_destroy(gate);
                gate = gate_new(coord_rate, coord_channels);
            } else if (dedupe != NULL) {
                coop_dedupe_destroy(dedupe);
                dedupe = coop_dedupe_create(coord_rate, coord_channels, COOP_AUDIO_DEDUPE_MAX_LAG_MS);
            }
        }
    }
    if (want_dedupe != current_dedupe) {
        coop_audio_log("[COOP-AUD] play shared sounds once -> %s", want_dedupe ? "on" : "off");
        current_dedupe = want_dedupe;
        if (!output_clocked && current_dedupe && dedupe != NULL && coord_rate > 0) {
            /* Start clean: while off the filter was not fed, so its buffers hold audio from before -- resuming them
             * would replay ~10 ms of stale sound. Same thread as the mix (this runs inside coop_audio_mix). */
            coop_dedupe_destroy(dedupe);
            dedupe = coop_dedupe_create(coord_rate, coord_channels, COOP_AUDIO_DEDUPE_MAX_LAG_MS);
            coop_pgate_destroy(gate);
            gate = gate_new(coord_rate, coord_channels);
        }
    }
    if (mode != current_mode) {
        coop_audio_log("[COOP-AUD] listen mode -> %s",
                       mode == COOP_AUDIO_PRIMARY_ONLY ? "primary only"
                       : mode == COOP_AUDIO_SECONDARY_ONLY ? "secondary only" : "mix");
        current_mode = mode;
    }
    if (output_pull) SDL_AtomicSet(&pull_policy, current_mode | (current_dedupe ? 4 : 0) | (current_gate ? 8 : 0));
    else { listen_mode=current_mode; dedupe_on=current_dedupe; gate_on=current_gate; }
    if (!output_pull) {   /* same thread as the gate here (this runs inside coop_audio_mix) */
        float want_restore = gate_restore_db;
        FILE *rf = fopen("/tmp/coop-gate-restore", "r");
        if (rf != NULL) {
            float v = 0.0f;
            if (fscanf(rf, "%f", &v) == 1) want_restore = v > 0.0f ? (v < 6.0f ? v : 6.0f) : 0.0f;
            fclose(rf);
        } else if (errno == ENOENT) {
            want_restore = 0.0f;
        }
        if (want_restore != gate_restore_db) {
            coop_audio_log("[COOP-AUD] gate restore -> %+.1f dB", want_restore);
        }
        if (gate != NULL && (want_restore != gate_restore_db || (const void *) gate != gate_restore_target)) {
            coop_pgate_set_restore_db(gate, want_restore);
            gate_restore_target = gate;
        }
        gate_restore_db = want_restore;
    }
}

/* --------------------------------------------------------------------------
 * Input capture (diagnostic, off unless /tmp/coop-audio-dump exists at start)
 * ------------------------------------------------------------------------ */

/* Writes the mixer's two inputs EXACTLY as they are summed -- self (the primary host) and peer (the second host),
 * before any gain -- to coop-audio-self.wav / coop-audio-peer.wav next to the diagnostic log. The file's content is
 * the number of seconds to capture (default 120, 5..600). WHY (2026-10-03, user): when both games play the same
 * narration, the mix plays it twice, slightly apart. Removing the duplicate needs two numbers nobody has measured --
 * how far apart the copies arrive and how alike their waveforms are -- and these two files are what measures them. */
static FILE *dump_self = NULL, *dump_peer = NULL, *dump_output = NULL;
static unsigned dump_frames_left = 0, dump_frames_done = 0;
static int dump_channels = 0;

static void dump_wav_header(FILE *f, int rate, int channels, uint32_t data_bytes) {
    uint8_t h[44];
    const uint32_t byte_rate = (uint32_t) rate * (uint32_t) channels * 2u;
    memcpy(h, "RIFF", 4);
    const uint32_t riff = 36u + data_bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmt_len = 16u;
    const uint16_t pcm_tag = 1u, ch = (uint16_t) channels, align = (uint16_t) (channels * 2), bits = 16u;
    const uint32_t r = (uint32_t) rate;
    memcpy(h + 16, &fmt_len, 4);
    memcpy(h + 20, &pcm_tag, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &r, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
}

static FILE *dump_open(const char *name, int rate, int channels) {
    char path[1024];
    snprintf(path, sizeof(path), "%s", AURORA_DIAG_LOG);
    char *slash = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }
    size_t dir_len = slash != NULL ? (size_t) (slash - path) + 1u : 0u;
    snprintf(path + dir_len, sizeof(path) - dir_len, "%s", name);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        dump_wav_header(f, rate, channels, 0u);
        coop_audio_log("[COOP-AUD] input capture -> %s", path);
    }
    return f;
}

static void dump_stop(void) {
    const uint32_t bytes = dump_frames_done * (uint32_t) dump_channels * 2u;
    FILE *files[3] = {dump_self, dump_peer, dump_output};
    for (int i = 0; i < 3; ++i) {
        if (files[i] != NULL) {
            dump_wav_header(files[i], coord_rate, dump_channels, bytes);
            fclose(files[i]);
        }
    }
    if (dump_self != NULL || dump_peer != NULL) {
        coop_audio_log("[COOP-AUD] input capture closed: %u frames (%.1f s) per file", dump_frames_done,
                       coord_rate > 0 ? (double) dump_frames_done / coord_rate : 0.0);
    }
    dump_self = dump_peer = dump_output = NULL;
    dump_frames_left = dump_frames_done = 0;
}

static void dump_start(int rate, int channels) {
    FILE *sf = fopen("/tmp/coop-audio-dump", "r");
    if (sf == NULL) {
        return;
    }
    int seconds = 120;
    if (fscanf(sf, "%d", &seconds) != 1 || seconds < 5 || seconds > 600) {
        seconds = 120;
    }
    fclose(sf);
    dump_channels = channels;
    dump_frames_done = 0;
    dump_frames_left = (unsigned) seconds * (unsigned) rate;
    dump_self = dump_open("coop-audio-self.wav", rate, channels);
    dump_peer = dump_open("coop-audio-peer.wav", rate, channels);
    dump_output = dump_open("coop-audio-output.wav", rate, channels);
    if (dump_self == NULL || dump_peer == NULL || dump_output == NULL) {
        dump_stop();
    }
}

static void dump_write(const int16_t *self_pcm, const int16_t *peer_pcm, int frames) {
    if (dump_self == NULL || dump_frames_left == 0u) {
        return;
    }
    unsigned n = (unsigned) frames < dump_frames_left ? (unsigned) frames : dump_frames_left;
    fwrite(self_pcm, (size_t) dump_channels * 2u, n, dump_self);
    fwrite(peer_pcm, (size_t) dump_channels * 2u, n, dump_peer);
}

/* Same capture block positions as the inputs, after the gate and limiter.
 * This is the submitted mix, not a recording of the speaker or device queue. */
static void dump_write_output(const int16_t *pcm, int frames) {
    if (dump_output == NULL || dump_frames_left == 0u) return;
    unsigned n = (unsigned) frames < dump_frames_left ? (unsigned) frames : dump_frames_left;
    fwrite(pcm, (size_t) dump_channels * 2u, n, dump_output);
    dump_frames_done += n;
    dump_frames_left -= n;
    if (dump_frames_left == 0u) {
        dump_stop();
    }
}

/* --------------------------------------------------------------------------
 * Coordinator side
 * ------------------------------------------------------------------------ */

extern pcmanager_t *pcmanager;

static bool coop_audio_host_is_this_pc(const char *uuid) {
    if (pcmanager == NULL || uuid == NULL || uuid[0] == '\0') {
        return false;
    }
    uuidstr_t id;
    uuidstr_fromstr(&id, uuid);
    const pclist_t *node = pcmanager_node(pcmanager, &id);
    return node != NULL && node->server != NULL && local_host_is_this_pc(node->server->serverInfo.address);
}

#define COOP_LOCAL_AUDIO_ENV "COOP_LOCAL_AUDIO"
/* The name Umbra/Apollo/Sunshine look for when no other virtual sink is configured (platform/windows/audio.cpp
 * match_steam_speakers). A host configured with a DIFFERENT virtual sink is not recognised here, which falls
 * back to the safe LOCAL mode -- never to a loop. */
#define COOP_VIRTUAL_SINK_NAME "Steam Streaming Speakers"

bool coop_audio_local_streamed(void) {
    const char *v = getenv(COOP_LOCAL_AUDIO_ENV);
    return v != NULL && strcmp(v, "stream") == 0;
}

void coop_audio_release_local_host(void) {
    if (getenv(COOP_LOCAL_AUDIO_ENV) == NULL) {
        return;
    }
    unsetenv(COOP_LOCAL_AUDIO_ENV);
    SS4S_SetAudioDevice(app_configuration != NULL ? app_configuration->audio_device : NULL);
}

void coop_audio_prepare_local_host(bool this_pc_is_a_host) {
    unsetenv(COOP_LOCAL_AUDIO_ENV);
    SS4S_SetAudioDevice(app_configuration != NULL ? app_configuration->audio_device : NULL);
#ifdef _WIN32
    if (!this_pc_is_a_host) {
        return;
    }
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        coop_audio_log("[COOP-AUD] local host: SDL audio unavailable (%s) -> this PC plays its own sound, not mixed",
                       SDL_GetError());
        setenv(COOP_LOCAL_AUDIO_ENV, "local", 1);
        return;
    }
    bool have_sink = false;
    for (int i = 0, n = SDL_GetNumAudioDevices(0); i < n; ++i) {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        if (name != NULL && strstr(name, COOP_VIRTUAL_SINK_NAME) != NULL) {
            have_sink = true;
        }
    }
    /* The device Eclipse will play to: the user's choice if they made one, else the CURRENT default. */
    const char *chosen = app_configuration != NULL ? app_configuration->audio_device : NULL;
    char *default_name = NULL;
    SDL_AudioSpec spec;
    if (chosen == NULL || chosen[0] == '\0' || strcmp(chosen, "default") == 0) {
        if (SDL_GetDefaultAudioInfo(&default_name, &spec, 0) == 0) {
            chosen = default_name;
        } else {
            chosen = NULL;
        }
    }
    const char *why = !have_sink ? "no \"" COOP_VIRTUAL_SINK_NAME "\" on this PC, so its host would capture the "
                                   "device Eclipse plays to"
                    : chosen == NULL ? "the default output device has no name"
                    : strstr(chosen, COOP_VIRTUAL_SINK_NAME) != NULL ? "the output device already IS the virtual sink "
                                                                       "(a previous session did not restore it?)"
                    : NULL;
    if (why != NULL) {
        coop_audio_log("[COOP-AUD] local host: %s -> this PC plays its own sound, NOT mixed", why);
        setenv(COOP_LOCAL_AUDIO_ENV, "local", 1);
    } else {
        SS4S_SetAudioDevice(chosen);
        setenv(COOP_LOCAL_AUDIO_ENV, "stream", 1);
        coop_audio_log("[COOP-AUD] local host: its sound is STREAMED and mixed; Eclipse output pinned to \"%s\"",
                       chosen);
    }
    SDL_free(default_name);
#else
    (void) this_pc_is_a_host;
#endif
}

void coop_audio_coord_start(int sample_rate, int channels) {
    if (coord_active || !app_launch_coop_requested() || coop_secondary_mode() != NULL) {
        return;
    }
    const coop_session_t *pair = coop_session_get();
    /* Only in the LOCAL fallback does a host that is this PC play its own sound (and so must not be mixed
     * again); STREAMED, its sound arrives like any host's (coop_audio_prepare_local_host). */
    const bool local_plays = !coop_audio_local_streamed();
    self_is_this_pc = local_plays && pair != NULL && coop_audio_host_is_this_pc(pair->primary_uuid);
    peer_is_this_pc = local_plays && pair != NULL && coop_audio_host_is_this_pc(pair->secondary_uuid);
    if (self_is_this_pc || peer_is_this_pc) {
        coop_audio_log("[COOP-AUD] %s host is this PC: its sound plays on this PC's speakers, not mixed in",
                       self_is_this_pc ? "primary" : "secondary");
    }
    if (channels > COOP_AUDIO_MAX_CHANNELS) {
        coop_audio_log("[COOP-AUD] %d channels: co-op audio is stereo only, not mixing", channels);
        return;
    }
    ring = calloc(COOP_AUDIO_RING_FRAMES * COOP_AUDIO_MAX_CHANNELS, sizeof(int16_t));
    if (ring == NULL) {
        return;
    }
    intptr_t fd = coop_msg_socket();
    if (fd < 0) {
        coop_audio_log("[COOP-AUD] message socket unavailable (errno=%d %s) -- co-op audio disabled",
                       errno, strerror(errno));
        free(ring);
        ring = NULL;
        return;
    }
    struct sockaddr_un addr;
    if (!coop_audio_fill_addr(&addr)) {
        close(fd);
        free(ring);
        ring = NULL;
        return;
    }
    unlink(COOP_AUDIO_SOCKET);   /* a stale socket file would make bind fail */
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(fd, 1) != 0 ||
        !coop_msg_set_nonblocking(fd)) {
        coop_audio_log("[COOP-AUD] listen failed errno=%d (%s)", errno, strerror(errno));
        close(fd);
        free(ring);
        ring = NULL;
        return;
    }
    int depth_ms = COOP_AUDIO_DEFAULT_DEPTH_MS;
    FILE *df = fopen("/tmp/coop-audio-depth", "r");
    if (df != NULL) {
        int v = 0;
        if (fscanf(df, "%d", &v) == 1 && v >= 5 && v <= 400) {
            depth_ms = v;
        }
        fclose(df);
    }
    ring_prime_frames = (unsigned) (sample_rate / 1000) * (unsigned) depth_ms;
    ring_max_frames = ring_prime_frames + (unsigned) (sample_rate / 1000) * 80u;
    coord_listen_fd = fd;
    coord_rate = sample_rate;
    coord_channels = channels;
    ring_head = ring_tail = 0;
    ring_primed = false;
    peer_format_ok = peer_format_logged = false;
    limiter_gain = 1.0f;
    output_role_gain[0] = output_role_gain[1] = COOP_AUDIO_MIX_GAIN;
    output_filter_wet = 0;
    output_queue_peak = 0;
    stat_received = stat_dropped = stat_underruns = stat_trims = stat_feeds = 0;
    /* Diagnostic windows start with the session: a first gap measured from the previous session's last mix call
     * would read as a huge stall. */
    win_last_mix_us = win_mix_gap_max_us = win_ipc_age_max_us = win_queue_age_max_us = 0;
    win_mix_gaps_50ms = win_mix_gaps_200ms = win_commit_refused = win_accepts = win_drain_max_msgs = 0;
    win_gate_seen = false;
    gate_restore_target = NULL;   /* a new gate object gets the current restore on the next refresh */
    timing_start_us = audio_time_us();
    timing_received_frames = timing_consumed_frames = timing_trimmed_frames = 0;
    timing_overflow_frames = timing_short_frames = timing_plc_frames = 0;
    timing_ipc_age_max_us = timing_queue_age_max_us = 0;
    timing_link_discarded_frames = timing_gap_frames = 0;
    timing_invalid_timestamp_frames = 0;
    timing_have_sequence = false;
    peer_epoch = peer_duplicates = 0;
    peer_ack_pending = peer_pcm_pending = false;
    coord_active = true;
    mode_recheck = 0;
    coop_audio_log("[COOP-AUD] coordinator listening on %s (%dHz %dch, %dms jitter buffer)",
                   COOP_AUDIO_SOCKET, sample_rate, channels, depth_ms);
    dump_start(sample_rate, channels);
    dedupe = coop_dedupe_create(sample_rate, channels, COOP_AUDIO_DEDUPE_MAX_LAG_MS);
    gate_accurate = app_configuration == NULL || app_configuration->coop_audio_filter != COOP_AUDIO_FILTER_FAST;
    gate_ahead_ms = clamp_ahead(app_configuration != NULL ? app_configuration->coop_audio_filter_ahead_ms
                                                          : COOP_PGATE_ACCURATE_LOOKAHEAD_MS);
    gate = gate_new(sample_rate, channels);
    gate_on = true;   /* the default; the first mix call reads /tmp/coop-audio-gate and logs any switch to dedupe */
    filter_active = true;   /* both filters are fresh: a session that starts filtering must not recreate them */
    dedupe_on = app_configuration == NULL || app_configuration->coop_audio_filter != COOP_AUDIO_FILTER_OFF;
    coop_audio_log("[COOP-AUD] play shared sounds once: %s (%s, look-ahead %d ms asked; the pull/clocked output runs "
                   "fast), filter PASSIVE GATE by default%s%s",
                   dedupe_on ? "on" : "off", gate_accurate ? "accurate" : "fast", gate_accurate ? gate_ahead_ms : 0, gate != NULL ? "" : " -- gate UNAVAILABLE (create failed)",
                   dedupe != NULL ? "" : " -- dedupe fallback UNAVAILABLE (create failed)");
}

void coop_audio_coord_stop(void) {
    const bool was_pull=output_pull;
    coop_audio_output_stop();
    dump_stop();
    if (coord_active) {
        if(!was_pull)timing_log(); /* pull has its own delivered-frame ledger */
        coop_audio_log("[COOP-AUD] session end: mode=%d frames mixed=%u peak self=%d peer=%d out=%d (16-bit full scale "
                       "32767) msgs=%u dropped=%u", listen_mode, sess_frames, sess_peak_self, sess_peak_peer,
                       sess_peak_out, stat_received, stat_dropped);
        sess_peak_self = sess_peak_peer = sess_peak_out = 0;
        sess_frames = 0;
    }
    if (coord_peer_fd >= 0) {
        coop_msg_channel_detach(&coord_channel);
        close(coord_peer_fd);
        coord_peer_fd = -1;
    }
    if (coord_listen_fd >= 0) {
        close(coord_listen_fd);
        coord_listen_fd = -1;
        unlink(COOP_AUDIO_SOCKET);
    }
    coop_audio_fifo_destroy(&input_fifo[0]);
    coop_audio_fifo_destroy(&input_fifo[1]);
    free(ring);
    ring = NULL;
    coop_dedupe_destroy(dedupe);
    dedupe = NULL;
    coop_pgate_destroy(gate);
    gate = NULL;
    coord_active = false;
}

bool coop_audio_coord_active(void) { return coord_active; }

static unsigned ring_count(void) {
    if (output_pull) return coop_audio_spsc_count(&pull_owner.source[1]);
    if (input_fifo[1].pcm) return input_fifo[1].count;
    return (ring_head - ring_tail + COOP_AUDIO_RING_FRAMES) % COOP_AUDIO_RING_FRAMES;
}

static void ring_write(const int16_t *pcm, unsigned frames, uint64_t ready_us) {
    for (unsigned i = 0; i < frames; ++i) {
        for (int c = 0; c < coord_channels; ++c) {
            ring[ring_head * COOP_AUDIO_MAX_CHANNELS + c] = pcm[i * coord_channels + c];
        }
        ring_ready_us[ring_head] = ready_us;
        ring_head = (ring_head + 1) % COOP_AUDIO_RING_FRAMES;
        if (ring_head == ring_tail) {
            /* Full: the oldest frame is the one to lose. Cannot happen in
             * practice (the depth cap below trims long before), but a wrapped
             * head reading as an empty ring would silence the peer for good. */
            ring_tail = (ring_tail + 1) % COOP_AUDIO_RING_FRAMES;
            timing_overflow_frames++;
        }
    }
}

static void coop_audio_accept(void) {
    if (coord_peer_fd >= 0 || coord_listen_fd < 0) {
        return;
    }
    intptr_t fd = accept(coord_listen_fd, NULL, NULL);
    if (fd < 0) {
        return;   /* EAGAIN: no secondary yet, which is the normal case for a while */
    }
    if (!coop_msg_set_nonblocking(fd) ||
        !coop_msg_channel_attach(&coord_channel, fd,
                                 sizeof(coop_audio_hdr_t) +
                                 COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS * sizeof(int16_t))) {
        close(fd);
        return;
    }
    coord_peer_fd = fd;
    win_accepts++;
    peer_format_ok = peer_format_logged = false;
    peer_ack_pending = peer_pcm_pending = false;
    if(output_pull) SDL_AtomicSet(&pull_link,1);
    else coop_audio_log("[COOP-AUD] secondary connected");
}

static void coord_disconnect(void) {
    coop_msg_channel_detach(&coord_channel);
    close(coord_peer_fd);
    coord_peer_fd = -1;
    peer_ack_pending = peer_pcm_pending = false;
}

/* One packet is retained until space exists. Sender keeps its copy until ACK,
 * so a lost ACK/reconnect neither discards nor duplicates accepted PCM. */
static bool coord_commit_pending(void) {
    if (!peer_pcm_pending) return true;
    coop_audio_hdr_t *hdr = &peer_pending_hdr;
    if (output_pull) {
        if (!coop_audio_spsc_push(&pull_owner.source[1], peer_pending_pcm, hdr->frames,
            (unsigned)coord_channels, hdr->first_frame, hdr->ready_us, hdr->epoch, hdr->reserved)) return false;
    } else if (output_clocked) {
        if ((unsigned)hdr->frames > input_fifo[1].capacity - input_fifo[1].count) return false;
        if (!coop_audio_fifo_push(&input_fifo[1], peer_pending_pcm, hdr->frames,
                                  hdr->first_frame, hdr->ready_us, hdr->reserved)) return false;
    } else {
        if ((unsigned)hdr->frames >= COOP_AUDIO_RING_FRAMES - ring_count()) { win_commit_refused++; return false; }
        ring_write(peer_pending_pcm, hdr->frames, hdr->ready_us);
    }
    if (timing_have_sequence && hdr->first_frame > timing_next_frame)
        timing_gap_frames += hdr->first_frame - timing_next_frame;
    timing_next_frame = hdr->first_frame + hdr->frames;
    timing_have_sequence = true;
    timing_received_frames += hdr->frames;
    if(output_pull)atomic_fetch_add_explicit(&pull_peer_accepted,hdr->frames,memory_order_relaxed);
    if (hdr->reserved & 1u) timing_plc_frames += hdr->frames;
    stat_received++;
    drain_msgs_call++;
    peer_pcm_pending = false;
    peer_ack_pending = true;
    return true;
}

static bool coord_flush_ack(void) {
    if (coop_msg_channel_flush(&coord_channel) == COOP_MSG_ERROR) {
        coord_disconnect(); return false;
    }
    if (peer_ack_pending) {
        const coop_audio_hdr_t ack = {.magic=COOP_AUDIO_MAGIC, .type=COOP_AUDIO_MSG_ACK,
            .first_frame=timing_next_frame, .epoch=peer_epoch};
        int result = coop_msg_channel_send(&coord_channel, &ack, sizeof(ack));
        if (result == COOP_MSG_ERROR) { coord_disconnect(); return false; }
        if (result != COOP_MSG_ACCEPTED) return false;
        peer_ack_pending = false;
    }
    return true;
}

static void coop_audio_drain(void) {
    if (coord_peer_fd < 0) {
        return;
    }
    if (!coord_commit_pending() || !coord_flush_ack()) return;
    unsigned char msg[sizeof(coop_audio_hdr_t) + COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS * sizeof(int16_t)];
    for (;;) {
        size_t n = 0;
        int result = coop_msg_channel_receive(&coord_channel, msg, sizeof(msg), &n);
        if (result == COOP_MSG_RECV_IDLE) {
            return;   /* drained, or one framed message is still arriving */
        }
        if (result == COOP_MSG_RECV_TOO_LARGE) {
            stat_dropped++;
            continue;
        }
        if (result == COOP_MSG_RECV_ERROR) {
            if(output_pull)SDL_AtomicSet(&pull_link,4);
            else coop_audio_log("[COOP-AUD] peer recv error errno=%d (%s) -- dropping link", errno, strerror(errno));
            coord_disconnect();
            return;
        }
        if (n < sizeof(coop_audio_hdr_t)) {
            stat_dropped++;
            continue;
        }
        coop_audio_hdr_t hdr;
        memcpy(&hdr, msg, sizeof(hdr));
        if (hdr.magic != COOP_AUDIO_MAGIC) {
            stat_dropped++;
            continue;
        }
        if (hdr.type == COOP_AUDIO_MSG_CONFIG) {
            /* Mixing is sample-wise, so a peer that does not match our format is
             * refused rather than resampled. session.c asks both hosts for the
             * same thing, so this firing means something upstream disagreed and
             * the log is how we would ever know. */
            peer_format_ok = hdr.epoch != 0 && ((int) hdr.rate == coord_rate && (int) hdr.channels == coord_channels);
            if (peer_format_ok && peer_epoch != hdr.epoch) {
                if (!output_pull) timing_link_discarded_frames += ring_count();
                if (output_clocked && !output_pull) {
                    coop_audio_fifo_clear(&input_fifo[1]);
                    output_source_indexed[1] = false;
                }
                ring_head = ring_tail = 0;
                ring_primed = false;
                timing_have_sequence = false;
                timing_next_frame = 0;
                const bool replacing_epoch = peer_epoch != 0;
                peer_epoch = hdr.epoch;
                if (!output_pull && replacing_epoch && gate && !coop_pgate_reset(gate)) {
                    coop_pgate_destroy(gate); gate = NULL;
                }
            }
            if (!peer_format_logged) {
                if(output_pull)SDL_AtomicSet(&pull_link,peer_format_ok?2:3);
                else coop_audio_log("[COOP-AUD] peer format %uHz %uch vs ours %dHz %dch => %s",
                               hdr.rate, hdr.channels, coord_rate, coord_channels,
                               peer_format_ok ? "mixing" : "REFUSED (format mismatch)");
                peer_format_logged = true;
            }
            continue;
        }
        if (hdr.type != COOP_AUDIO_MSG_PCM || !peer_format_ok || hdr.epoch != peer_epoch) {
            stat_dropped++;
            continue;
        }
        size_t want = (size_t) hdr.frames * (size_t) coord_channels * sizeof(int16_t);
        if (hdr.frames == 0 || hdr.frames > COOP_AUDIO_MAX_FRAMES || want + sizeof(hdr) != n ||
            hdr.first_frame > UINT64_MAX - hdr.frames) {
            stat_dropped++;
            continue;
        }
        if (timing_have_sequence && hdr.first_frame < timing_next_frame) {
            if (hdr.first_frame + hdr.frames <= timing_next_frame) {
                peer_duplicates += hdr.frames;
                peer_ack_pending = true;
                if (!coord_flush_ack()) return;
                continue;
            }
            stat_dropped++; coord_disconnect(); return; /* invalid partial overlap */
        }
        const uint64_t now_us = audio_time_us();
        if (!hdr.ready_us || now_us < hdr.ready_us) {
            timing_invalid_timestamp_frames += hdr.frames;
            hdr.ready_us = 0;
        } else if (now_us - hdr.ready_us > timing_ipc_age_max_us) {
            timing_ipc_age_max_us = now_us - hdr.ready_us;
        }
        if (hdr.ready_us && now_us - hdr.ready_us > win_ipc_age_max_us) win_ipc_age_max_us = now_us - hdr.ready_us;
        peer_pending_hdr = hdr;
        memcpy(peer_pending_pcm, msg + sizeof(hdr), want);
        peer_pcm_pending = true;
        if (!coord_commit_pending() || !coord_flush_ack()) return;

    }
}

static void timing_log(void) {
    const uint64_t now_us = audio_time_us();
    coop_audio_log("[COOP-AUD.timing] time_us=%llu elapsed_us=%llu mix_frames=%u "
                   "peer_received_frames=%llu peer_consumed_frames=%llu peer_queued_frames=%u "
                   "peer_trimmed_frames=%llu peer_overflow_frames=%llu peer_short_frames=%llu "
                   "peer_plc_frames=%llu ipc_age_max_us=%llu queue_age_max_us=%llu "
                   "peer_link_discarded_frames=%llu peer_gap_frames=%llu invalid_timestamp_frames=%llu",
                   (unsigned long long) now_us, (unsigned long long) (now_us - timing_start_us), sess_frames,
                   (unsigned long long) timing_received_frames, (unsigned long long) timing_consumed_frames,
                   ring_count(), (unsigned long long) timing_trimmed_frames,
                   (unsigned long long) timing_overflow_frames, (unsigned long long) timing_short_frames,
                   (unsigned long long) timing_plc_frames, (unsigned long long) timing_ipc_age_max_us,
                   (unsigned long long) timing_queue_age_max_us,
                   (unsigned long long) timing_link_discarded_frames, (unsigned long long) timing_gap_frames,
                   (unsigned long long) timing_invalid_timestamp_frames);
}

static void mix_blocks(int16_t *pcm, int16_t *peer_block, int frames, bool ready, unsigned valid);

void coop_audio_mix(int16_t *pcm, int frames) {
    if (!coord_active || frames <= 0 || frames > COOP_AUDIO_MAX_FRAMES) {
        return;
    }
    const uint64_t mix_now_us = audio_time_us();
    if (win_last_mix_us != 0 && mix_now_us > win_last_mix_us) {
        const uint64_t gap_us = mix_now_us - win_last_mix_us;
        if (gap_us > win_mix_gap_max_us) win_mix_gap_max_us = gap_us;
        if (gap_us >= 50000u) win_mix_gaps_50ms++;
        if (gap_us >= 200000u) win_mix_gaps_200ms++;
    }
    win_last_mix_us = mix_now_us;
    coop_audio_refresh_mode();
    coop_audio_accept();
    drain_msgs_call = 0;
    coop_audio_drain();
    if (drain_msgs_call > win_drain_max_msgs) win_drain_max_msgs = drain_msgs_call;

    /* Correct drift downwards, never upwards. Two hosts' audio clocks cannot
     * stay locked, and a peer running fast would otherwise add latency without
     * bound; dropping the oldest costs one audible tick every few minutes,
     * which is the cheaper of the two. */
    if (ring_count() > ring_max_frames) {
        unsigned excess = ring_count() - ring_prime_frames;
        ring_tail = (ring_tail + excess) % COOP_AUDIO_RING_FRAMES;
        timing_trimmed_frames += excess;
        stat_trims++;
    }
    if (!ring_primed && ring_count() >= ring_prime_frames) {
        ring_primed = true;
    }

    /* How much peer audio we actually have. Short is normal at the start of a
     * session and after a drop-out: the missing tail mixes as silence and the
     * ring re-primes, rather than stretching or stalling our own stream. */
    unsigned avail = ring_primed ? ring_count() : 0;
    unsigned take = (unsigned) frames < avail ? (unsigned) frames : avail;
    if (ring_primed && take < (unsigned) frames) {
        ring_primed = false;
        stat_underruns++;
    }
    timing_short_frames += (unsigned) frames - take;
    timing_consumed_frames += take;
    if (take && ring_ready_us[ring_tail]) {
        const uint64_t now_us = audio_time_us();
        if (now_us >= ring_ready_us[ring_tail] && now_us - ring_ready_us[ring_tail] > timing_queue_age_max_us) {
            timing_queue_age_max_us = now_us - ring_ready_us[ring_tail];
        }
        if (now_us >= ring_ready_us[ring_tail] && now_us - ring_ready_us[ring_tail] > win_queue_age_max_us) {
            win_queue_age_max_us = now_us - ring_ready_us[ring_tail];
        }
    }

    int16_t peer_block[COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS];
    for (int i = 0; i < frames; ++i) for (int c = 0; c < coord_channels; ++c) {
        unsigned idx = (ring_tail + (unsigned)i) % COOP_AUDIO_RING_FRAMES;
        peer_block[i * coord_channels + c] = (unsigned)i < take ? ring[idx * COOP_AUDIO_MAX_CHANNELS + c] : 0;
    }
    ring_tail = (ring_tail + take) % COOP_AUDIO_RING_FRAMES;
    mix_blocks(pcm, peer_block, frames, false, COOP_PGATE_VALID_BOTH);
}

static void mix_blocks(int16_t *pcm, int16_t *peer_block, int frames, bool ready, unsigned valid) {
    float g_self, g_peer;
    switch (listen_mode) {
        case COOP_AUDIO_PRIMARY_ONLY:
            g_self = 1.0f;
            g_peer = 0.0f;
            break;
        case COOP_AUDIO_SECONDARY_ONLY:
            g_self = 0.0f;
            g_peer = 1.0f;
            break;
        default:
            g_self = g_peer = COOP_AUDIO_MIX_GAIN;
            break;
    }
    if (self_is_this_pc) {
        g_self = 0.0f;
        g_peer = listen_mode == COOP_AUDIO_PRIMARY_ONLY ? 0.0f : 1.0f;
    } else if (peer_is_this_pc) {
        g_peer = 0.0f;
        g_self = listen_mode == COOP_AUDIO_SECONDARY_ONLY ? 0.0f : 1.0f;
    }

    const int ch = coord_channels;
    int32_t sum[COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS];
    int16_t self_block[COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS];
    int16_t peer_filtered[COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS];
    for (int i = 0; i < frames * ch; ++i) {
        int32_t a = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        int32_t b = peer_block[i] < 0 ? -(int32_t)peer_block[i] : peer_block[i];
        if (a > peak_self) peak_self = a;
        if (b > peak_peer) peak_peer = b;
    }
    if (!output_pull) dump_write(pcm, peer_block, frames);   /* the raw inputs, before the filter below */

    /* Play a sound both hosts make once: only when actually mixing two audible hosts. The passive gate is the default;
     * the single-offset filter runs when the A/B switch asks for it, or if the gate could not be created. */
    const int16_t *self_src = pcm;
    const int16_t *peer_src = peer_block;
    bool processed = false;
    bool use_gate = gate_on && gate != NULL;
    const bool was_active = filter_active;
    filter_active = dedupe_on && listen_mode == COOP_AUDIO_MIX && !self_is_this_pc && !peer_is_this_pc &&
                    (use_gate || dedupe != NULL);
    if (!ready && filter_active && !was_active && coord_rate > 0) {
        /* Resuming after a pause in filtering (e.g. listen mode back to mix): neither filter was fed meanwhile, so
         * start the one in use clean rather than play its held block and match against stale history. */
        if (use_gate) {
            coop_pgate_destroy(gate);
            gate = gate_new(coord_rate, coord_channels);
            use_gate = gate != NULL;
        } else {
            coop_dedupe_destroy(dedupe);
            dedupe = coop_dedupe_create(coord_rate, coord_channels, COOP_AUDIO_DEDUPE_MAX_LAG_MS);
        }
        filter_active = use_gate || dedupe != NULL;
    }
    if (ready) {
        /* Analysis follows stable input identities even while policy selects a
         * single source. Switching roles never resets source history or time. */
        memcpy(self_block, pcm, sizeof(int16_t) * (size_t)frames * (size_t)ch);
        memcpy(peer_filtered, peer_block, sizeof(int16_t) * (size_t)frames * (size_t)ch);
        processed = gate && coop_pgate_process_ready_block_valid(gate, self_block, peer_filtered, frames, valid);
        filter_active = filter_active && use_gate && processed;
        if (processed) {
            self_src = self_block;
            peer_src = peer_filtered;
        }
    } else if (filter_active) {
        memcpy(self_block, pcm, sizeof(int16_t) * (size_t) frames * (size_t) ch);
        if (use_gate) coop_pgate_process(gate, self_block, peer_block, frames);
        else coop_dedupe_process(dedupe, self_block, peer_block, frames);
        self_src = self_block;
    }

    const bool limit_mix = listen_mode == COOP_AUDIO_MIX ||
        (ready && (output_role_gain[0] != g_self || output_role_gain[1] != g_peer));
    int32_t peak = 0;
    const float wet_target = filter_active ? 1.0f : 0.0f;
    for (int i = 0; i < frames * ch; ++i) {
        const float fraction = (float)(i / ch + 1) / (float)frames;
        const float gs = ready ? output_role_gain[0] + (g_self-output_role_gain[0])*fraction : g_self;
        const float gp = ready ? output_role_gain[1] + (g_peer-output_role_gain[1])*fraction : g_peer;
        const float wet = ready && processed ? output_filter_wet + (wet_target-output_filter_wet)*fraction : 1.0f;
        const float a = ready ? pcm[i] + ((float)self_src[i]-pcm[i])*wet : self_src[i];
        const float b = ready ? peer_block[i] + ((float)peer_src[i]-peer_block[i])*wet : peer_block[i];
        int32_t v = (int32_t)(a * gs + b * gp);
        sum[i] = v;
        int32_t magnitude = v < 0 ? -v : v;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    if (ready) {
        output_role_gain[0] = g_self; output_role_gain[1] = g_peer;
        output_filter_wet = wet_target;
    }

    /* Soft limiter instead of a hard clamp. Clipping two summed game mixes is
     * audible as crackle on every loud moment; a smoothed gain reduction with a
     * fast attack and a ~1 s release ducks those moments instead.
     *
     * ONLY when actually mixing. In a single-host mode there is nothing to
     * protect against -- the source already fits -- and touching the gain there
     * would make "primary only" sound different from a normal solo stream, which
     * is exactly the comparison the mode exists to allow. */
    const float limiter_before = limiter_gain;
    if (limit_mix) {
        float wanted = peak > COOP_AUDIO_LIMIT ? COOP_AUDIO_LIMIT / (float) peak : 1.0f;
        if (ready && wanted < limiter_gain) limiter_gain = wanted;
        else limiter_gain += (wanted - limiter_gain) * (wanted < limiter_gain ? 0.5f : 0.005f);
    } else {
        limiter_gain = 1.0f;
    }
    for (int i = 0; i < frames * ch; ++i) {
        float gain = ready ? limiter_before + (limiter_gain-limiter_before) *
            ((float)(i/ch+1)/(float)frames) : limiter_gain;
        /* The full current block is available: never let an attack ramp exceed
         * its safe peak gain. Release/role changes can still ramp smoothly. */
        const float safe_gain = peak > COOP_AUDIO_LIMIT ? COOP_AUDIO_LIMIT / (float)peak : 1.0f;
        if (ready && limit_mix && gain > safe_gain) gain = safe_gain;
        int32_t v = (int32_t) ((float) sum[i] * gain);
        pcm[i] = (int16_t) (v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        int32_t o_abs = pcm[i] < 0 ? -(int32_t) pcm[i] : pcm[i];
        if (o_abs > peak_out) {
            peak_out = o_abs;
        }
    }

    if (!output_pull) dump_write_output(pcm, frames);
    sess_frames += (unsigned) frames;
    if (peak_self > sess_peak_self) sess_peak_self = peak_self;
    if (peak_peer > sess_peak_peer) sess_peak_peer = peak_peer;
    if (peak_out > sess_peak_out) sess_peak_out = peak_out;
    /* One line every ~5 s. Audio has no picture to look at, so these counters
     * are the only way to tell "the peer is silent" from "the peer is absent". */
    if ((++stat_feeds % 1000u) == 0u && !output_pull) {
        timing_log();
        coop_dedupe_stats_t dds = {0};
        if (dedupe != NULL) {
            coop_dedupe_get_stats(dedupe, &dds);
        }
        coop_audio_log("[COOP-AUD] mode=%d peer=%s msgs=%u depth=%ums under=%u trim=%u bad=%u gain=%.2f "
                       "peak self=%d peer=%d out=%d dedupe=%s lag=%+.1fms rho=%.2f amount=%.2f quieting=%s",
                       listen_mode, coord_peer_fd >= 0 ? "up" : "down", stat_received,
                       ring_count() / 48u, stat_underruns, stat_trims, stat_dropped, limiter_gain,
                       peak_self, peak_peer, peak_out, dedupe_on ? "on" : "off",
                       coord_rate > 0 ? 1000.0 * dds.lag_samples / coord_rate : 0.0, dds.rho, dds.amount,
                       dds.later == 2 ? "peer" : dds.later == 1 ? "self" : "-");
        if (filter_active && gate_on && gate != NULL) {   /* only while the gate is actually processing */
            coop_pgate_stats_t gs;
            coop_pgate_get_stats(gate, &gs);
            char bank[200];
            int len = 0;
            bank[0] = 0;
            for (int i = 0; i < gs.offsets && len < 180; ++i) {
                len += snprintf(bank + len, sizeof(bank) - (size_t) len, " %+.1f",
                                coord_rate > 0 ? 1000.0 * gs.offset_samples[i] / coord_rate : 0.0);
            }
            coop_audio_log("[COOP-AUD] pgate: offsets(ms)%s | balance peer-vs-self %+.1fdB%s | predictions ok %u failed %u | "
                           "dropouts %u env-lines %u fast-tracks %u", len ? bank : " none", gs.balance_db,
                           gs.balance_applied ? "" : " (learning)", gs.pred_confirmed, gs.pred_failed, gs.dropouts,
                           gs.env_lines, gs.fast_tracks);
        }
        {
            unsigned d_drop = 0, d_adm = 0, d_evi = 0, d_pfail = 0, d_twin = 0, d_switch = 0, d_sticky = 0;
            if (filter_active && gate_on && gate != NULL) {
                coop_pgate_stats_t gs;
                coop_pgate_get_stats(gate, &gs);
                if (win_gate_seen && gs.dropouts >= win_gate_dropouts && gs.admitted >= win_gate_admitted &&
                    gs.evicted >= win_gate_evicted && gs.pred_failed >= win_gate_pred_failed &&
                    gs.twin_splits >= win_gate_twin_splits && gs.side_switches >= win_gate_side_switches &&
                    gs.sticky_holds >= win_gate_sticky_holds) {
                    d_drop = gs.dropouts - win_gate_dropouts; d_adm = gs.admitted - win_gate_admitted;
                    d_evi = gs.evicted - win_gate_evicted; d_pfail = gs.pred_failed - win_gate_pred_failed;
                    d_twin = gs.twin_splits - win_gate_twin_splits;
                    d_switch = gs.side_switches - win_gate_side_switches; d_sticky = gs.sticky_holds - win_gate_sticky_holds;
                }
                win_gate_dropouts = gs.dropouts; win_gate_admitted = gs.admitted;
                win_gate_evicted = gs.evicted; win_gate_pred_failed = gs.pred_failed;
                win_gate_twin_splits = gs.twin_splits;
                win_gate_side_switches = gs.side_switches; win_gate_sticky_holds = gs.sticky_holds;
                win_gate_seen = true;
            } else {
                win_gate_seen = false;
            }
            char wall[32];
            unsigned long long epoch_ms = 0;
            wall_clock(wall, sizeof(wall), &epoch_ms);
            coop_audio_log("[COOP-AUD.ingest] wall=%s epoch_ms=%llu mono_us=%llu mix_gap_max_us=%llu mix_gaps_50ms=%u "
                           "mix_gaps_200ms=%u drain_max_msgs=%u commit_refused=%u accepts=%u ipc_age_max_ms=%.1f "
                           "queue_age_max_ms=%.1f gate_dropouts=%u gate_admitted=%u gate_evicted=%u gate_pred_failed=%u "
                           "gate_twin_splits=%u gate_side_switches=%u gate_sticky_holds=%u restore_db=%.1f filter=%s ahead_ms=%d",
                           wall, epoch_ms, (unsigned long long) audio_time_us(), (unsigned long long) win_mix_gap_max_us,
                           win_mix_gaps_50ms, win_mix_gaps_200ms, win_drain_max_msgs, win_commit_refused, win_accepts,
                           win_ipc_age_max_us / 1000.0, win_queue_age_max_us / 1000.0, d_drop, d_adm, d_evi, d_pfail,
                           d_twin, d_switch, d_sticky, gate_restore_db,
                           !(filter_active && gate_on && gate != NULL) ? "off"
                           : coop_pgate_quality(gate) == COOP_PGATE_ACCURATE ? "accurate" : "fast",
                           gate != NULL ? (coop_pgate_latency_frames(gate) - coop_pgate_block_frames(gate)) * 1000 /
                                              (coord_rate > 0 ? coord_rate : 48000) : 0);
            win_mix_gap_max_us = win_ipc_age_max_us = win_queue_age_max_us = 0;
            win_mix_gaps_50ms = win_mix_gaps_200ms = win_commit_refused = win_accepts = win_drain_max_msgs = 0;
        }
        peak_self = peak_peer = peak_out = 0;
    }
}

/* A missing tail or any concealed frame makes that source's analysis block
 * invalid. Its audible samples still pass through; no fabricated match evidence. */
static unsigned render_input(coop_audio_fifo_t *q, int16_t *pcm, unsigned frames, uint64_t now, unsigned bit) {
    const unsigned source = q == &input_fifo[1] ? 1u : 0u;
    const unsigned take = q->count < frames ? q->count : frames;
    bool valid = take == frames, index_ok = true;
    for (unsigned i=0; i<take; ++i) {
        const coop_audio_stamp_t *stamp = &q->stamp[(q->head+i)%q->capacity];
        if (output_source_indexed[source] && stamp->index != output_source_next[source]) index_ok=false;
        output_source_next[source] = stamp->index + 1;
        output_source_indexed[source] = true;
        if (stamp->flags) valid=false;
    }
    if (!index_ok) { valid=false; output_invalid_index_blocks[source]++; }
    coop_audio_fifo_render(q, pcm, frames, now);
    return valid ? bit : 0;
}

static void output_render(void *ctx, int16_t *self, unsigned frames) {
    (void)ctx;
    int16_t peer[COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS];
    const uint64_t now = audio_time_us();
    coop_audio_refresh_mode();
    SDL_LockMutex(input_lock);
    unsigned valid = render_input(&input_fifo[0], self, frames, now, COOP_PGATE_VALID_SELF);
    SDL_UnlockMutex(input_lock);
    uint64_t before = input_fifo[1].consumed;
    valid |= render_input(&input_fifo[1], peer, frames, now, COOP_PGATE_VALID_PEER);
    unsigned take = (unsigned)(input_fifo[1].consumed - before);
    timing_consumed_frames += take;
    timing_short_frames += frames - take;
    timing_queue_age_max_us = input_fifo[1].max_age_us;
    mix_blocks(self, peer, (int)frames, true, valid);
}

static int output_feed(void *ctx, const int16_t *pcm, unsigned frames) {
    (void)ctx;
    SS4S_AudioFeedResult r = SS4S_PlayerAudioFeed(output_player, (const unsigned char *)pcm,
        frames * (unsigned)coord_channels * sizeof(int16_t));
    return r == SS4S_AUDIO_FEED_OK ? COOP_OUTPUT_FEED_OK :
           r == SS4S_AUDIO_FEED_ERROR ? COOP_OUTPUT_FEED_ERROR : COOP_OUTPUT_FEED_WAIT;
}

static void output_log(void) {
    SDL_LockMutex(input_lock);
    const coop_audio_fifo_t self = input_fifo[0];
    SDL_UnlockMutex(input_lock);
    coop_audio_log("[COOP-AUD.output] submitted_frames=%llu retries=%llu pending_frames=%u failed=%d queue_peak_frames=%llu self_age_max_us=%llu peer_age_max_us=%llu "
                   "self_accepted=%llu self_consumed=%llu self_queued=%u self_rejected=%llu "
                   "peer_accepted=%llu peer_consumed=%llu peer_queued=%u peer_rejected=%llu",
        (unsigned long long)output_owner.submitted_frames, (unsigned long long)output_owner.retries,
        output_owner.pending ? output_block_frames : 0, output_owner.failed,
        (unsigned long long)output_queue_peak, (unsigned long long)self.max_age_us,
        (unsigned long long)input_fifo[1].max_age_us,
        (unsigned long long)self.accepted, (unsigned long long)self.consumed,
        self.count, (unsigned long long)self.rejected,
        (unsigned long long)input_fifo[1].accepted, (unsigned long long)input_fifo[1].consumed,
        input_fifo[1].count, (unsigned long long)input_fifo[1].rejected);
}

static int output_main(void *unused);

/* The callback publishes bounded diagnostic records. A slow logger/capture
 * invalidates diagnostics, never blocks playout or overwrites an unread record. */
static pull_record_t *pull_record_begin(unsigned *next) {
    unsigned w=atomic_load_explicit(&pull_write,memory_order_relaxed);
    *next=(w+1)%PULL_RECORDS;
    if(*next==atomic_load_explicit(&pull_read,memory_order_acquire)) {
        ++pull_dropped; return NULL;
    }
    return &pull_records[w];
}

static void pull_observe(void *ctx,const int16_t *self,const int16_t *peer,
                         const int16_t *out,unsigned frames,uint64_t index) {
    (void)ctx;
    if(!pull_capture)return;
    unsigned next; pull_record_t *r=pull_record_begin(&next);
    if(!r)return;
    r->frames=frames; r->index=index; r->dropped=pull_dropped;
    const size_t bytes=(size_t)frames*coord_channels*sizeof(int16_t);
    memcpy(r->pcm[0],self,bytes); memcpy(r->pcm[1],peer,bytes); memcpy(r->pcm[2],out,bytes);
    atomic_store_explicit(&pull_write,next,memory_order_release);
}

static void pull_mix(void *ctx,int16_t *self,int16_t *peer,int16_t *out,
                     unsigned frames,unsigned valid,unsigned new_epoch) {
    (void)ctx;
    const int policy=SDL_AtomicGet(&pull_policy);
    listen_mode=policy&3; dedupe_on=(policy&4)!=0; gate_on=(policy&8)!=0;
    if(new_epoch && gate)coop_pgate_reset(gate);
    /* Rendered on the device thread, a block must come back at once, so the gate runs its ready-block API, which is
     * Fast whatever was asked. Produced ahead (Android) the worker may hold audio back, so it runs the push path's
     * streaming gate instead: the quality the user chose -- Accurate, 40 ms look-ahead by default -- exactly as the TV
     * hears it, both sources delayed alike. */
    mix_blocks(self,peer,(int)frames,!pull_owner.ahead,valid);
    memcpy(out,self,(size_t)frames*coord_channels*sizeof(int16_t));
}

/* The stats record for the service thread. Published by whichever thread renders: the device callback, or in
 * produced-ahead mode the worker (the record ring has one producer). */
static void pull_publish(void) {
    unsigned next; pull_record_t *r=pull_record_begin(&next);
    if(r) {
        r->frames=0; r->stats=pull_owner.stats; r->dropped=pull_dropped;
        for(unsigned s=0;s<2;++s) {
            r->depth[s]=coop_audio_spsc_count(&pull_owner.source[s]);
            if(!pull_owner.ahead && pull_owner.real[s]>pull_owner.offset)
                r->depth[s]+=pull_owner.real[s]-pull_owner.offset;
        }
        atomic_store_explicit(&pull_write,next,memory_order_release);
    }
}

static unsigned pull_render(void *ctx,int16_t *pcm,unsigned frames,uint64_t begin_us) {
    (void)ctx;
    unsigned done=coop_audio_pull_render(&pull_owner,pcm,frames,begin_us);
    if(!pull_owner.ahead)pull_publish();
    return done;
}

static uint64_t pull_time(void *ctx) { (void)ctx; return audio_time_us(); }

/* Upper edge in ms of the bucket holding the per-mille quantile (0 = no timed frames);
 * COOP_AUDIO_AGE_BUCKETS means "at least BUCKETS-1 ms". Service thread only. */
static unsigned age_quantile_ms(const uint64_t *hist,unsigned per_mille) {
    uint64_t total=0,seen=0;
    for(unsigned b=0;b<COOP_AUDIO_AGE_BUCKETS;++b)total+=hist[b];
    if(!total)return 0;
    for(unsigned b=0;b<COOP_AUDIO_AGE_BUCKETS;++b) {
        seen+=hist[b];
        if(seen*1000u>=total*per_mille)return b+1u;
    }
    return COOP_AUDIO_AGE_BUCKETS;
}

static void pull_log(const coop_audio_pull_stats_t *st,const unsigned depth[2],uint64_t dropped,bool final) {
    coop_audio_log("[COOP-AUD.pull.age] final=%d output_frames=%llu "
        "self_age_p50_ms=%u self_age_p90_ms=%u self_age_p99_ms=%u self_age_p999_ms=%u "
        "peer_age_p50_ms=%u peer_age_p90_ms=%u peer_age_p99_ms=%u peer_age_p999_ms=%u",
        final,(unsigned long long)st->output_frames,
        age_quantile_ms(st->age_hist[0],500),age_quantile_ms(st->age_hist[0],900),
        age_quantile_ms(st->age_hist[0],990),age_quantile_ms(st->age_hist[0],999),
        age_quantile_ms(st->age_hist[1],500),age_quantile_ms(st->age_hist[1],900),
        age_quantile_ms(st->age_hist[1],990),age_quantile_ms(st->age_hist[1],999));
    coop_audio_log("[COOP-AUD.pull] final=%d output_frames=%llu self_accepted=%llu self_consumed=%llu self_queued=%u self_rejected=%llu "
        "peer_accepted=%llu peer_consumed=%llu peer_queued=%u self_missing=%llu peer_missing=%llu "
        "self_timed=%llu peer_timed=%llu self_untimed=%llu peer_untimed=%llu self_age_max_us=%llu peer_age_max_us=%llu "
        "self_plc=%llu peer_plc=%llu self_gaps=%llu peer_gaps=%llu diagnostic_dropped=%llu "
        "self_trimmed=%llu peer_trimmed=%llu device_short=%llu",
        final, (unsigned long long)st->output_frames,
        atomic_load_explicit(&pull_self_accepted,memory_order_relaxed),
        (unsigned long long)st->consumed[0],depth[0],
        atomic_load_explicit(&pull_self_rejected,memory_order_relaxed),
        atomic_load_explicit(&pull_peer_accepted,memory_order_relaxed),(unsigned long long)st->consumed[1],depth[1],
        (unsigned long long)st->missing[0],(unsigned long long)st->missing[1],
        (unsigned long long)st->timed[0],(unsigned long long)st->timed[1],
        (unsigned long long)st->untimed[0],(unsigned long long)st->untimed[1],
        (unsigned long long)st->age_max_us[0],(unsigned long long)st->age_max_us[1],
        (unsigned long long)st->concealed[0],(unsigned long long)st->concealed[1],
        (unsigned long long)st->discontinuities[0],(unsigned long long)st->discontinuities[1],
        (unsigned long long)dropped,
        (unsigned long long)st->trimmed[0],(unsigned long long)st->trimmed[1],(unsigned long long)st->device_short);
}

static void pull_drain_records(void) {
    unsigned rd=atomic_load_explicit(&pull_read,memory_order_relaxed);
    unsigned wr=atomic_load_explicit(&pull_write,memory_order_acquire);
    while(rd!=wr) {
        const pull_record_t *r=&pull_records[rd];
        if(r->frames) {
            if(r->index!=pull_capture_next && dump_self) {
                coop_audio_log("[COOP-AUD.pull] capture invalid: missing rendered blocks; stopping diagnostic WAVs");
                dump_stop();
            }
            dump_write(r->pcm[0],r->pcm[1],(int)r->frames);
            dump_write_output(r->pcm[2],(int)r->frames);
            pull_capture_next=r->index+r->frames;
        } else {
            if(r->stats.output_frames/(5u*(unsigned)coord_rate)!=pull_last.output_frames/(5u*(unsigned)coord_rate))
                pull_log(&r->stats,r->depth,r->dropped,false);
            pull_last=r->stats;
        }
        rd=(rd+1)%PULL_RECORDS;
        atomic_store_explicit(&pull_read,rd,memory_order_release);
    }
}

/* This service can wait on files/player lifecycle; neither PCM producer nor
 * the device callback depends on its progress. */
static int pull_service_main(void *unused) {
    (void)unused;
    unsigned polls=0;
    int last_link=-1;
    while(SDL_AtomicGet(&output_run)) {
        coop_audio_refresh_mode();
        pull_drain_records();
        int link=SDL_AtomicGet(&pull_link);
        if(link!=last_link) { coop_audio_log("[COOP-AUD.pull] link_state=%d",link); last_link=link; }
        if(++polls%1000u==0) {
            SS4S_AudioClock clock;
            SS4S_PlayerAudioGetClock(output_player,&clock);
        }
        SDL_Delay(1);
    }
    return 0;
}

bool coop_audio_output_open_pull(SS4S_Player *player,const SS4S_AudioInfo *info) {
    if(!coord_active || output_thread || !info || !SS4S_AudioHasPull())return false;
    output_block_frames=(unsigned)(coord_rate/200);
    if(!output_block_frames || output_block_frames>COOP_AUDIO_MAX_FRAMES ||
        (gate && output_block_frames!=(unsigned)coop_pgate_block_frames(gate)))return false;
    atomic_init(&pull_read,0); atomic_init(&pull_write,0);
    atomic_init(&pull_self_accepted,0); atomic_init(&pull_self_rejected,0);
    atomic_init(&pull_peer_accepted,0); SDL_AtomicSet(&pull_link,0);
    if(!atomic_is_lock_free(&pull_read) || !atomic_is_lock_free(&pull_write) ||
        !atomic_is_lock_free(&pull_self_accepted) || !atomic_is_lock_free(&pull_self_rejected) ||
        !atomic_is_lock_free(&pull_peer_accepted))return false;
    if(!coop_audio_pull_init(&pull_owner,(unsigned)coord_channels,output_block_frames,
        COOP_AUDIO_RING_FRAMES,pull_mix,pull_observe,NULL))return false;
    pull_owner.now_us=pull_time;
#if TARGET_ANDROID
    /* THE SHIELD TV MIXES AHEAD OF ITS DEVICE (coop_audio_pull_set_ahead). Mixed and gated inside the AAudio callback,
     * 9% of callbacks overran their own 5.3 ms (render up to 27 ms) and each overrun's silence stayed behind as
     * backlog: both queues grew ~5 ms/s to their 500 ms cap, then the primary's audio was thrown away in 5 ms pieces
     * (live, 2026-10-21). A worker now renders COOP_AUDIO_PULL_AHEAD_BLOCKS ahead and the callback only copies; a source
     * queue past ring_max_frames is cut back to ring_prime_frames, the push path's depth policy. Android only. */
    if(!coop_audio_pull_set_ahead(&pull_owner,COOP_AUDIO_PULL_AHEAD_BLOCKS*output_block_frames,
                                  ring_max_frames,ring_prime_frames)) {
        coop_audio_pull_destroy(&pull_owner); return false;
    }
#endif
    pull_records=calloc(PULL_RECORDS,sizeof(*pull_records));
    if(!pull_records) { coop_audio_pull_destroy(&pull_owner); return false; }
    pull_capture=dump_self!=NULL; pull_capture_next=pull_dropped=0;
    memset(&pull_last,0,sizeof(pull_last));
    SDL_AtomicSet(&pull_policy,listen_mode|(dedupe_on?4:0)|(gate_on?8:0));
    output_player=player; output_pull=output_clocked=true; pull_device_opened=false;
    coop_audio_refresh_mode(); /* control I/O completes before device starts */
    SDL_AtomicSet(&output_run,1);
    coop_audio_pull_produce(&pull_owner,audio_time_us());   /* produced-ahead: the device starts on a full ring */
    if(SS4S_PlayerAudioOpenPull(player,info,pull_render,NULL)!=SS4S_AUDIO_OPEN_OK) {
        coop_audio_output_stop(); return false;
    }
    pull_device_opened=true;
    output_thread=SDL_CreateThread(output_main,"coop-audio-ingress",NULL);
    if(!output_thread) { coop_audio_output_stop(); return false; }
    pull_service_thread=SDL_CreateThread(pull_service_main,"coop-audio-service",NULL);
    if(!pull_service_thread) { coop_audio_output_stop(); return false; }
    if(pull_owner.ahead)
        coop_audio_log("[COOP-AUD.pull] worker-rendered block_frames=%u ahead_frames=%u trim_above=%u trim_to=%u rate=%d channels=%d "
                       "gate=%s look-ahead=%d ms (streaming, as the push path)",
                       output_block_frames,pull_owner.ahead,pull_owner.trim_above,pull_owner.trim_to,coord_rate,coord_channels,
                       gate_accurate ? "accurate" : "fast",gate_accurate ? gate_ahead_ms : 0);
    else
        coop_audio_log("[COOP-AUD.pull] callback-owned block_frames=%u rate=%d channels=%d",output_block_frames,coord_rate,coord_channels);
    return true;
}

/* Output demand is the sole pacing authority. Never catch up against a timer. */
static int output_main(void *unused) {
    (void)unused;
    uint64_t next_report = (unsigned)coord_rate * 5u;
    /* Produced-ahead, this thread carries the mix and the gate the device is waiting for. */
#if TARGET_ANDROID
    if(output_pull && pull_owner.ahead) android_audio_priority("coop-audio-ingress");
#else
    if(output_pull && pull_owner.ahead) SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif
    while (SDL_AtomicGet(&output_run)) {
        coop_audio_accept();
        coop_audio_drain();
        if(output_pull) {
            /* One block at a time, taking in the peer's audio and acknowledging it between blocks. The secondary sends
             * nothing more until its last message is acknowledged, and the gate can hold this thread for tens of ms
             * when it renders a run of blocks: on a SHIELD TV co-op mission acknowledgements waited up to 168 ms
             * behind 50 ms runs, so the peer's audio went missing, then arrived in a burst and was trimmed, while it
             * reached the secondary steadily (live, 2026-10-22). */
            if(pull_owner.ahead) {
                unsigned rendered=0;
                while(SDL_AtomicGet(&output_run) && coop_audio_pull_produce_max(&pull_owner,audio_time_us(),1)) {
                    ++rendered;
                    coop_audio_accept();
                    coop_audio_drain();
                }
                if(rendered) pull_publish();
            }
            SDL_Delay(1);
            continue;
        }
        SS4S_AudioClock clock;
        if (!SS4S_PlayerAudioGetClock(output_player, &clock)) {
            output_owner.failed = true;
            break;
        }
        if (clock.queued_frames > output_queue_peak) output_queue_peak = clock.queued_frames;
        const coop_output_clock_t snapshot = {clock.sample_rate, clock.quantum_frames,
                                              clock.generation, clock.queued_frames};
        int result = coop_output_step(&output_owner, &snapshot, output_render, output_feed, NULL);
        if (result == COOP_OUTPUT_ERROR) break;
        if (output_owner.submitted_frames >= next_report) {
            output_log(); next_report += (unsigned)coord_rate * 5u;
        }
        if (result != COOP_OUTPUT_PROGRESS) SDL_Delay(1);
    }
    SDL_AtomicSet(&output_run, 0);
    if(!output_pull) output_log();
    return 0;
}

bool coop_audio_output_start(SS4S_Player *player) {
    if (!coord_active || output_thread) return false;
    SS4S_AudioClock clock;
    if (!SS4S_PlayerAudioGetClock(player, &clock) || clock.sample_rate != (unsigned)coord_rate ||
        !clock.quantum_frames) return false;
    output_block_frames = (unsigned)(coord_rate / 200); /* complete 5ms gate block */
    if (!output_block_frames || output_block_frames > COOP_AUDIO_MAX_FRAMES ||
        (gate && output_block_frames != (unsigned)coop_pgate_block_frames(gate))) return false;
    input_lock = SDL_CreateMutex();
    if (!input_lock || !coop_audio_fifo_init(&input_fifo[0], COOP_AUDIO_RING_FRAMES, (unsigned)coord_channels) ||
        !coop_audio_fifo_init(&input_fifo[1], COOP_AUDIO_RING_FRAMES, (unsigned)coord_channels)) {
        coop_audio_output_stop();
        coop_audio_fifo_destroy(&input_fifo[0]); coop_audio_fifo_destroy(&input_fifo[1]);
        return false;
    }
    output_player = player;
    memset(output_source_indexed, 0, sizeof(output_source_indexed));
    memset(output_source_next, 0, sizeof(output_source_next));
    memset(output_invalid_index_blocks, 0, sizeof(output_invalid_index_blocks));
    if (!coop_output_init(&output_owner, (unsigned)coord_rate, (unsigned)coord_channels,
                          output_block_frames, clock.generation)) {
        coop_audio_output_stop();
        coop_audio_fifo_destroy(&input_fifo[0]); coop_audio_fifo_destroy(&input_fifo[1]);
        return false;
    }
    output_clocked = true;
    SDL_AtomicSet(&output_run, 1);
    output_thread = SDL_CreateThread(output_main, "coop-audio-output", NULL);
    if (!output_thread) {
        coop_audio_output_stop();
        coop_audio_fifo_destroy(&input_fifo[0]); coop_audio_fifo_destroy(&input_fifo[1]);
        return false;
    }
    coop_audio_log("[COOP-AUD.output] device-paced block_frames=%u quantum_frames=%u rate=%u generation=%llu",
        output_block_frames, clock.quantum_frames, clock.sample_rate, (unsigned long long)clock.generation);
    return true;
}

bool coop_audio_output_submit(const int16_t *pcm, unsigned frames, uint64_t first_frame,
                               uint64_t ready_us, bool plc) {
    if (!output_clocked || !SDL_AtomicGet(&output_run)) return false;
    if(output_pull) {
        bool accepted=coop_audio_spsc_push(&pull_owner.source[0],pcm,frames,(unsigned)coord_channels,
            first_frame,ready_us,1,plc?1u:0u);
        atomic_fetch_add_explicit(accepted?&pull_self_accepted:&pull_self_rejected,frames,memory_order_relaxed);
        return accepted;
    }
    SDL_LockMutex(input_lock);
    bool accepted = SDL_AtomicGet(&output_run) &&
        coop_audio_fifo_push(&input_fifo[0], pcm, frames, first_frame, ready_us, plc ? 1u : 0u);
    SDL_UnlockMutex(input_lock);
    return accepted;
}

void coop_audio_output_stop(void) {
    SDL_AtomicSet(&output_run, 0);
    if (output_thread) SDL_WaitThread(output_thread, NULL);
    output_thread = NULL;
    if(output_pull) {
        if(pull_service_thread)SDL_WaitThread(pull_service_thread,NULL);
        pull_service_thread=NULL;
        if(pull_device_opened)SS4S_PlayerAudioClose(output_player); /* joins callback before any state is freed */
        pull_device_opened=false;
        pull_drain_records();
        unsigned depth[2];
        for(unsigned s=0;s<2;++s) {
            depth[s]=coop_audio_spsc_count(&pull_owner.source[s]);
            if(!pull_owner.ahead && pull_owner.real[s]>pull_owner.offset)depth[s]+=pull_owner.real[s]-pull_owner.offset;
        }
        if(pull_owner.ahead) {   /* the device's own counters, as of its last callback */
            pull_owner.stats.output_frames=atomic_load(&pull_owner.device_frames);
            pull_owner.stats.device_short=atomic_load(&pull_owner.device_short);
        }
        pull_log(&pull_owner.stats,depth,pull_dropped,true);
        timing_consumed_frames=pull_owner.stats.consumed[1];
        timing_short_frames=pull_owner.stats.missing[1];
        timing_queue_age_max_us=pull_owner.stats.age_max_us[1];
        coop_audio_pull_destroy(&pull_owner);
        free(pull_records); pull_records=NULL;
        output_pull=false;
    }
    output_clocked = false;
    output_player = NULL;
    coop_output_destroy(&output_owner);
    if (input_lock) SDL_DestroyMutex(input_lock);
    input_lock = NULL;
}

/* --------------------------------------------------------------------------
 * Secondary side
 * ------------------------------------------------------------------------ */

static intptr_t sec_fd = -1;
static coop_msg_channel_t sec_channel = COOP_MSG_CHANNEL_INITIALIZER;
static OpusMSDecoder *sec_decoder = NULL;
static int sec_channels = 0, sec_rate = 0, sec_frame_size = 0;
static uint64_t sec_decoded_frames, sec_rejected_frames, sec_plc_frames, sec_packets, sec_plc_calls;
/* Secondary diagnostics per ~5 s [COOP-AUD.secondary] window. fifo max is written by the decoder under sec_lock;
 * the ACK fields belong to the sender thread, which also writes the line (the decoder thread never logs). */
static unsigned sec_win_fifo_max, sec_win_ack_timeouts, sec_win_msg_frames_max;
static uint64_t sec_win_ack_wait_max_us, sec_win_rejected_base, sec_last_report_us;

static void secondary_decode_log(void) {
    coop_audio_log("[COOP-AUD.decode] role=secondary time_us=%llu packet_frames=%d packets=%llu "
                   "plc_calls=%llu plc_frames=%llu decoded_frames=%llu rejected_frames=%llu",
                   (unsigned long long) audio_time_us(), sec_frame_size, (unsigned long long) sec_packets,
                   (unsigned long long) sec_plc_calls, (unsigned long long) sec_plc_frames,
                   (unsigned long long) sec_decoded_frames, (unsigned long long) sec_rejected_frames);
}
static SDL_Thread *sec_thread;
static SDL_mutex *sec_lock;
static SDL_atomic_t sec_run;
static coop_audio_fifo_t sec_fifo;
static uint64_t sec_epoch;
#define COOP_AUDIO_RETRY_MS 10
#define COOP_AUDIO_ACK_TIMEOUT_MS 250

/* SAY WHY THE LINK IS DOWN. Every step below used to fail silently, and the feed retries each second, so
 * on the Windows client a co-op session ran with the second PC's sound never reaching the mixer
 * ("peer=down msgs=0") and no line anywhere saying why (VEGA + PC2, 2026-09-29). Logged once per
 * distinct cause, not once per retry. */
static void coop_audio_secondary_link_failed(const char *step) {
    static char last[64] = "";
    const int err = errno;
    char now[64];
    snprintf(now, sizeof(now), "%s errno=%d", step, err);
    if (strcmp(now, last) != 0) {
        snprintf(last, sizeof(last), "%s", now);
        coop_audio_log("audio: coordinator link failed at %s (%s) -- retrying", now, strerror(err));
    }
}

static bool coop_audio_secondary_connect(void) {
    intptr_t fd = coop_msg_socket();
    if (fd < 0) {
        coop_audio_secondary_link_failed("socket");
        return false;
    }
    struct sockaddr_un addr;
    if (!coop_audio_fill_addr(&addr)) {
        close(fd);
        return false;
    }
    if (!coop_msg_set_nonblocking(fd)) {
        coop_audio_secondary_link_failed("nonblocking"); close(fd); return false;
    }
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
            coop_audio_secondary_link_failed("connect"); close(fd); return false;
        }
        struct pollfd connecting = {.fd=fd, .events=POLLOUT};
        int error=0; socklen_t size=sizeof(error);
        if (poll(&connecting, 1, COOP_AUDIO_RETRY_MS) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0 ||
            !(connecting.revents & POLLOUT)) {
            coop_audio_secondary_link_failed("connect completion"); close(fd); return false;
        }
    }
    if (!coop_msg_channel_attach(&sec_channel, fd,
                                 sizeof(coop_audio_hdr_t) +
                                 COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS * sizeof(int16_t))) {
        coop_audio_secondary_link_failed("attach");
        close(fd);
        return false;
    }
    sec_fd = fd;
    coop_audio_hdr_t hdr = {
            .magic = COOP_AUDIO_MAGIC,
            .type = COOP_AUDIO_MSG_CONFIG,
            .frames = 0,
            .rate = (uint32_t) sec_rate,
            .channels = (uint16_t) sec_channels,
            .epoch = sec_epoch,
    };
    if (coop_msg_channel_send(&sec_channel, &hdr, sizeof(hdr)) != COOP_MSG_ACCEPTED) {
        coop_audio_secondary_link_failed("config");
        coop_msg_channel_detach(&sec_channel);
        close(sec_fd);
        sec_fd = -1;
        return false;
    }
    coop_audio_log("audio: connected to coordinator (%dHz %dch)", sec_rate, sec_channels);
    return true;
}

static void secondary_disconnect(void) {
    coop_msg_channel_detach(&sec_channel);
    if (sec_fd >= 0) close(sec_fd);
    sec_fd = -1;
}

static int secondary_send_main(void *unused) {
    (void)unused;
#if TARGET_ANDROID
    android_audio_priority("coop-audio-send");
#endif
    bool awaiting_ack = false;
    uint64_t pending_end = 0, sent_us = 0;
    unsigned pending_frames = 0;
    unsigned char message[sizeof(coop_audio_hdr_t) + COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS * sizeof(int16_t)];
    int16_t pcm[COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS];
    while (SDL_AtomicGet(&sec_run)) {
        const uint64_t loop_us = audio_time_us();
        if (loop_us - sec_last_report_us >= 5000000u) {
            /* Periodic, so a secondary killed at quit (signal 15 -> no final ledger) still leaves its counters. */
            SDL_LockMutex(sec_lock);
            const unsigned fifo_max = sec_win_fifo_max, fifo_now = sec_fifo.count;
            const unsigned msg_frames_max = sec_win_msg_frames_max;
            sec_win_fifo_max = sec_win_msg_frames_max = 0;
            SDL_UnlockMutex(sec_lock);
            const uint64_t rejected = sec_rejected_frames;   /* decoder-written; telemetry-grade read */
            char wall[32];
            unsigned long long epoch_ms = 0;
            wall_clock(wall, sizeof(wall), &epoch_ms);
            coop_audio_log("[COOP-AUD.secondary] wall=%s epoch_ms=%llu mono_us=%llu packets=%llu plc_calls=%llu "
                           "decoded_frames=%llu rejected_frames=%llu rejected_window=%llu fifo_max_frames=%u "
                           "fifo_now_frames=%u msg_frames_max=%u ack_wait_max_us=%llu ack_timeouts=%u sent=%u linked=%d",
                           wall, epoch_ms, (unsigned long long) loop_us, (unsigned long long) sec_packets,
                           (unsigned long long) sec_plc_calls, (unsigned long long) sec_decoded_frames,
                           (unsigned long long) rejected, (unsigned long long) (rejected - sec_win_rejected_base),
                           fifo_max, fifo_now, msg_frames_max, (unsigned long long) sec_win_ack_wait_max_us,
                           sec_win_ack_timeouts,
                           stat_sent, sec_fd >= 0);
            sec_win_rejected_base = rejected;
            sec_win_ack_wait_max_us = 0;
            sec_win_ack_timeouts = 0;
            sec_last_report_us = loop_us;
        }
        if (sec_fd < 0) {
            awaiting_ack = false;
            if (!coop_audio_secondary_connect()) { SDL_Delay(COOP_AUDIO_RETRY_MS); continue; }
        }
        if (coop_msg_channel_flush(&sec_channel) == COOP_MSG_ERROR) {
            secondary_disconnect(); continue;
        }
        if (awaiting_ack) {
            if (audio_time_us() - sent_us >= (uint64_t)COOP_AUDIO_ACK_TIMEOUT_MS * 1000u) {
                sec_win_ack_timeouts++;
                secondary_disconnect(); continue;
            }
            coop_audio_hdr_t ack;
            size_t n = 0;
            int result = coop_msg_channel_receive(&sec_channel, &ack, sizeof(ack), &n);
            if (result == COOP_MSG_RECV_ERROR || result == COOP_MSG_RECV_TOO_LARGE) {
                secondary_disconnect(); continue;
            }
            if (result == COOP_MSG_RECV_MESSAGE) {
                if (n != sizeof(ack) || ack.magic != COOP_AUDIO_MAGIC || ack.type != COOP_AUDIO_MSG_ACK ||
                    ack.epoch != sec_epoch || ack.first_frame != pending_end) {
                    secondary_disconnect(); continue;
                }
                const uint64_t waited_us = audio_time_us() - sent_us;
                if (waited_us > sec_win_ack_wait_max_us) sec_win_ack_wait_max_us = waited_us;
                SDL_LockMutex(sec_lock);
                coop_audio_fifo_consume(&sec_fifo, pending_frames, audio_time_us());
                SDL_UnlockMutex(sec_lock);
                stat_sent++;
                awaiting_ack = false;
                continue;
            }
        } else {
            coop_audio_stamp_t stamp;
            SDL_LockMutex(sec_lock);
            /* A whole run, not one packet: the coordinator takes one message per mix call, so one packet per
             * message could only ever keep pace, and a burst from the host stayed queued here for good (measured
             * 2026-10-09: ~195 ms for the last 17 min of a session). Delivered, a backlog reaches the ring, whose
             * trim bounds the latency. */
            unsigned frames = coop_audio_fifo_peek_run(&sec_fifo, pcm, COOP_AUDIO_MAX_FRAMES, &stamp);
            SDL_UnlockMutex(sec_lock);
            if (frames) {
                coop_audio_hdr_t hdr = {.magic=COOP_AUDIO_MAGIC, .type=COOP_AUDIO_MSG_PCM,
                    .frames=(uint16_t)frames, .rate=(uint32_t)sec_rate, .channels=(uint16_t)sec_channels,
                    .reserved=(uint16_t)stamp.flags, .ready_us=stamp.ready_us,
                    .first_frame=stamp.index, .epoch=sec_epoch};
                memcpy(message, &hdr, sizeof(hdr));
                size_t bytes = frames * (unsigned)sec_channels * sizeof(int16_t);
                memcpy(message + sizeof(hdr), pcm, bytes);
                int result = coop_msg_channel_send(&sec_channel, message, sizeof(hdr) + bytes);
                if (result == COOP_MSG_ERROR) { secondary_disconnect(); continue; }
                if (result == COOP_MSG_ACCEPTED) {
                    awaiting_ack = true;
                    pending_frames = frames;
                    pending_end = stamp.index + frames;
                    if (frames > sec_win_msg_frames_max) sec_win_msg_frames_max = frames;
                    sent_us = audio_time_us();
                }
            }
        }
        SDL_Delay(1);
    }
    secondary_disconnect();
    return 0;
}

static int coop_secondary_aud_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                                   void *context, int arFlags) {
    (void) audioConfiguration;
    (void) context;
    (void) arFlags;
    int rc = 0;
    sec_decoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount,
                                                 opusConfig->streams, opusConfig->coupledStreams,
                                                 opusConfig->mapping, &rc);
    if (rc != 0 || sec_decoder == NULL) {
        coop_audio_log("audio: opus decoder create failed rc=%d", rc);
        return rc != 0 ? rc : -1;
    }
    sec_channels = opusConfig->channelCount;
    sec_rate = opusConfig->sampleRate;
    sec_frame_size = opusConfig->samplesPerFrame;
    sec_decoded_frames = sec_rejected_frames = sec_plc_frames = sec_packets = sec_plc_calls = 0;
    sec_win_fifo_max = sec_win_ack_timeouts = sec_win_msg_frames_max = 0;
    sec_win_ack_wait_max_us = sec_win_rejected_base = sec_last_report_us = 0;
    secondary_decode_log();
    if (sec_channels > COOP_AUDIO_MAX_CHANNELS || sec_frame_size > COOP_AUDIO_MAX_FRAMES) {
        /* Do not fail the session over it: the pane feed is what this process is
         * for, and it is worth keeping even without audio. */
        coop_audio_log("audio: %dch/%d-sample frames unsupported -- not forwarding audio",
                       sec_channels, sec_frame_size);
        opus_multistream_decoder_destroy(sec_decoder);
        sec_decoder = NULL;
        return 0;
    }
    sec_epoch = audio_time_us();
    stat_sent = stat_dropped = 0;
    sec_lock = SDL_CreateMutex();
    if (!sec_lock || !coop_audio_fifo_init(&sec_fifo, COOP_AUDIO_RING_FRAMES, (unsigned)sec_channels)) {
        if (sec_lock) SDL_DestroyMutex(sec_lock);
        sec_lock = NULL;
        opus_multistream_decoder_destroy(sec_decoder); sec_decoder = NULL;
        return -1;
    }
    SDL_AtomicSet(&sec_run, 1);
    sec_thread = SDL_CreateThread(secondary_send_main, "coop-audio-send", NULL);
    if (!sec_thread) {
        coop_audio_fifo_destroy(&sec_fifo); SDL_DestroyMutex(sec_lock); sec_lock = NULL;
        opus_multistream_decoder_destroy(sec_decoder); sec_decoder = NULL;
        return -1;
    }
    return 0;
}

static void coop_secondary_aud_cleanup(void) {
    SDL_AtomicSet(&sec_run, 0);
    if (sec_thread) SDL_WaitThread(sec_thread, NULL);
    sec_thread = NULL;
    secondary_decode_log();
    coop_audio_log("[COOP-AUD.transport] epoch=%llu accepted_frames=%llu acknowledged_frames=%llu queued_frames=%u rejected_frames=%llu",
        (unsigned long long)sec_epoch, (unsigned long long)sec_fifo.accepted,
        (unsigned long long)sec_fifo.consumed, sec_fifo.count, (unsigned long long)sec_fifo.rejected);
    coop_audio_fifo_destroy(&sec_fifo);
    if (sec_lock) SDL_DestroyMutex(sec_lock);
    sec_lock = NULL;
    if (sec_fd >= 0) {
        coop_msg_channel_detach(&sec_channel);
        close(sec_fd);
        sec_fd = -1;
    }
    if (sec_decoder != NULL) {
        opus_multistream_decoder_destroy(sec_decoder);
        sec_decoder = NULL;
    }
    coop_audio_log("audio: forwarded=%u dropped=%u", stat_sent, stat_dropped);
}

static void coop_secondary_aud_feed(char *sampleData, int sampleLength) {
    if (sec_decoder == NULL) {
        return;
    }
    /* Decode even while disconnected: the decoder state and sample accounting
     * must advance across a coordinator reconnect. */
    int16_t pcm[COOP_AUDIO_MAX_FRAMES * COOP_AUDIO_MAX_CHANNELS];
    int decoded = coop_audio_decode_opus(sec_decoder, (unsigned char *) sampleData, sampleLength,
                                         pcm, COOP_AUDIO_MAX_FRAMES, sec_frame_size);
    sec_packets++;
    if (sampleData == NULL) sec_plc_calls++;
    if (decoded <= 0) {
        stat_dropped++;
        return;
    }
    const uint64_t ready_us = audio_time_us(), first_frame = sec_decoded_frames;
    sec_decoded_frames += decoded;
    if (sampleData == NULL) sec_plc_frames += decoded;
    /* Decode must publish PCM before any diagnostic file access. Complete
     * decoder accounting is reported at cleanup, outside this producer. */
    SDL_LockMutex(sec_lock);
    bool accepted = coop_audio_fifo_push(&sec_fifo, pcm, (unsigned)decoded, first_frame,
                                         ready_us, sampleData == NULL ? 1u : 0u);
    if (sec_fifo.count > sec_win_fifo_max) sec_win_fifo_max = sec_fifo.count;
    SDL_UnlockMutex(sec_lock);
    if (!accepted) {
        sec_rejected_frames += (unsigned)decoded;
        stat_dropped++;
    }

}

unsigned coop_audio_received(void) { return stat_received; }

unsigned coop_audio_sent(void) { return stat_sent; }

unsigned coop_audio_dropped(void) { return stat_dropped; }

AUDIO_RENDERER_CALLBACKS coop_secondary_aud_callbacks = {
        .init = coop_secondary_aud_init,
        .cleanup = coop_secondary_aud_cleanup,
        .decodeAndPlaySample = coop_secondary_aud_feed,
        .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
