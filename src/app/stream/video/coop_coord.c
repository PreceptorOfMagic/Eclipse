#include "coop_coord.h"
#include "coop_ipc.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* PROCESS MANAGEMENT IS THE ONLY PART OF THIS FILE THAT IS NOT PORTABLE, and it is
 * confined to two guarded regions below (orphan sweeping, and spawn/reap of the
 * secondary). Everything else -- the pane queue, the worker, the IPC -- compiles as
 * written on both platforms. The Windows bodies for the guarded functions live in
 * coop_proc_win.c; nothing here is compiled on Windows, and none of this changes what
 * the webOS build produces. */
#ifndef _WIN32
#include <dirent.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#ifdef __ANDROID__
#include "util/path.h"
#endif
#include <signal.h>
#include <sys/time.h>
#endif

#define COOP_AU_CAP (12u * 1024u * 1024u)  /* a 4K HDR IDR at 85Mbps exceeds 4MB */

static intptr_t coord_listen_fd = -1;
static char *coord_socket_path;

static void coord_close_listener(void) {
    if (coord_listen_fd >= 0) {
        close(coord_listen_fd);
        coord_listen_fd = -1;
    }
    if (coord_socket_path != NULL) {
        unlink(coord_socket_path);
        free(coord_socket_path);
        coord_socket_path = NULL;
    }
}
/* The worker owns accept() and every peer close.  stop() may only shutdown the current
 * peer while holding this lock; it never invalidates a descriptor underneath worker I/O. */
static pthread_mutex_t coord_fd_lock = PTHREAD_MUTEX_INITIALIZER;
static intptr_t coord_peer_fd = -1;
static pthread_t coord_thread;
/* GUARDS THE JOIN. pthread_create leaves `coord_thread` UNINITIALISED on failure, so joining it
 * unconditionally in stop() is undefined behaviour on exactly the path where start() failed. */
static bool coord_thread_started = false;
static atomic_bool coord_run = ATOMIC_VAR_INIT(false);
static atomic_bool coord_connected = ATOMIC_VAR_INIT(false);
enum coop_coord_start_state {
    COOP_COORD_PENDING = 0,
    COOP_COORD_READY,
    COOP_COORD_FAILED,
};
/* Guarded by coord_lock. The worker writes these while the video setup thread
 * waits; volatile would not make that cross-thread access defined or ordered. */
static enum coop_coord_start_state coord_start_state = COOP_COORD_PENDING;
static uint8_t coord_start_error = 0;

/* PANE QUEUE, guarded by a mutex. Held briefly: memcpy only, never I/O.
 *
 * THIS WAS A SINGLE SLOT AND THAT WAS THE LAST BIG SOURCE OF GHOSTING. The video thread
 * takes one pane per picture it composes; a single slot means any pane that arrives while
 * the previous one is still unconsumed is DISCARDED, and any picture that arrives in a gap
 * finds the slot empty. Measured over one run: recv=7209, replaced=2847, and 1192 pictures
 * still had to repeat a stale pane. The peer was delivering 65% MORE panes than we needed
 * and we were throwing them away, then concealing -- pure jitter, mistaken for the peer
 * being too slow, and every repeat re-applies its residual and smears the pane.
 *
 * A short queue turns burst-arrival into 1:1 pairing without waiting for anything, so it
 * costs no latency at the pace the peer is already running; the depth cap bounds how far
 * the pane can lag if the peer ever runs genuinely faster, and overflow drops the OLDEST
 * so what we show stays current. Depth is tunable at runtime via /tmp/coop-queue because
 * the right value depends on the jitter of two hosts, which is not knowable from source.
 */
/* DEEP ENOUGH TO ABSORB A BURST, because a dropped pane is far more expensive than a
 * buffered one. An overflow drops a pane, which breaks the peer's prediction chain until its
 * next keyframe -- and the only clean recovery (hold the pane frozen) is VISIBLE as a hitch.
 * Buffering instead costs latency ONLY while the queue is actually occupied: the consumer
 * takes one pane per composed picture, so with the two hosts' rates matched the queue sits
 * near empty and grows only during a burst, which is exactly when dropping would hurt.
 * Raised 4 -> 16 on 2026-08-02 after peergap=127/46796 (~one every 6s) proved to be the last
 * visible artefact source. */
#define COOP_Q_SLOTS 16
#define COOP_Q_SLOT_CAP (4u * 1024u * 1024u)
static pthread_mutex_t coord_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*coord_wakeup)(void) = NULL;
static unsigned char *coord_q[COOP_Q_SLOTS];
static size_t coord_q_len[COOP_Q_SLOTS];
static uint32_t coord_q_frame[COOP_Q_SLOTS];
static bool coord_q_idr[COOP_Q_SLOTS];
static unsigned long coord_q_ms[COOP_Q_SLOTS];
/* THE PEER'S OWN PROVENANCE, PARKED ALONGSIDE EACH QUEUED PANE -- passover 07 §3.1.
 * coord_q_ms has always recorded when a pane finished arriving HERE; these record what the
 * secondary knew about the same picture before it crossed the link, so an age can finally be
 * decomposed into peer-side, link-side and compositor-side parts instead of starting at the
 * coordinator's door. */
static coop_ipc_hdr_t coord_q_meta[COOP_Q_SLOTS];
static uint32_t coord_last_recv_frame = 0;     /* newest peer frame to finish arriving here */
static coop_ipc_hdr_t coord_taken_meta;        /* provenance of the last pane handed out */
static unsigned long coord_taken_arrival_ms = 0;
static uint32_t coord_ipc_dwell_last = 0, coord_ipc_dwell_max = 0;
static uint32_t coord_send_block_last = 0, coord_send_block_max = 0;
static unsigned coord_q_head = 0, coord_q_count = 0;
/* DEPTH 4, RAISED FROM 3 ON A LIVE A/B 2026-07-30, and the difference is not marginal.
 * Sequence enforcement (coop_seq.c) turns an overflow drop into a HOLE, and a hole costs a
 * full re-anchor -- suppress, both hosts IDR, resume -- not merely one late pane. So the
 * shallow queue was not trading freshness for lag, it was destroying the pane feed.
 * Same build, same session, only this value changed:
 *      depth 3:  replaced=373  paired=612/fallback=7788 (7%)  peergap=898 climbing  boot=0
 *      depth 4:  replaced=  0  paired=3556/fallback=1244(74%) peergap=548           boot=1
 * paneAge stayed 0ms on every sample, so the extra slot cost no measurable latency
 * ([[latency-minimization]]) -- the queue simply was not the thing making the pane late.
 * Predicted offline first (tests/coop/test_coop_seq.c models this exact cliff), then
 * confirmed live. /tmp/coop-queue still overrides, and COOP_Q_SLOTS caps it. */
/* 16, NOT 12, since 2026-09-05 — and the queue has always had 16 SLOTS, so this costs no memory.
 *
 * WHY: at a 120 fps composite against a peer near 87 fps the peer arrives in BURSTS, and a burst
 * bigger than the queue evicts. Measured: cu[gate=87 plan=0 take=0 build=0] with catchup=87, i.e.
 * catch-up fired 87 times and never once failed, yet qmax reached 12 (the full depth) and
 * evicted=9. Catch-up drains at most COOP_VT_CATCHUP_MAX (6) panes per picture, so a burst
 * arriving between two pictures can outrun it.
 *
 * WHY EVICTION MATTERS SO MUCH MORE THAN ITS COUNT SUGGESTS: an eviction latches
 * coop_avc_chain_broken, which FREEZES the peer's pane ("freeze rather than paint damage") and
 * requests a peer keyframe EVERY picture until one lands. With gop_length=300 on the peer that
 * wait is up to ~5 s, so THREE evictions produced ~15 s of frozen peer pane -- measured on the
 * panel as det codes 235 steps apart and read, wrongly at first, as corruption.
 *
 * At 60 fps the queue never fills and evicted=0, which is why this never showed until 120. */
static unsigned coord_q_depth = 16;

/* The most recent IDR pane, kept SEPARATELY and never overwritten by later
 * inter panes. Bootstrapping the second pane needs an IDR from both hosts in the
 * SAME picture, and the two hosts IDR independently -- by the time our own host
 * produces one, the other PC's IDR is long gone from the latest-pane slot. */
static unsigned char *coord_idr_au = NULL;
static size_t coord_idr_len = 0;
static uint32_t coord_idr_frame = 0;   /* the peer's own frame number for that IDR */

static unsigned coord_recv_count = 0;
static unsigned coord_replaced = 0;
static unsigned coord_oversize = 0, coord_oversize_p = 0;
/* Posted by the video thread, sent by the drain thread so the 165fps path never writes.
 * The worker owns socket I/O/close; stop() only issues shutdown under coord_fd_lock. */
static atomic_bool coord_idr_req = ATOMIC_VAR_INIT(false);
static atomic_bool coord_shutdown_req = ATOMIC_VAR_INIT(false);

/* PEER IDR REQUESTS MUST BE RATE LIMITED, AND NOT FOR TIDINESS.
 *
 * Measured live 2026-08-01: with the pane anchor stuck, every picture asked the peer for a
 * keyframe and PC2 received 550 requests in 78 seconds. It honours each one, so a 3840x1080
 * stream became near-all-intra; the bitrate that produces (78-142 Mbps for one 1080p pane,
 * measured in docs/pane-budget-results.txt) swamped the link, and the secondary ended up
 * forwarding FIVE access units a second while the peer's own frame counter advanced at 48/s.
 * Asking harder made fewer panes arrive.
 *
 * Nothing downstream can benefit from more than a couple per second anyway: the request has
 * to cross to the other host, be encoded and come back. 500ms is well inside that and still
 * far below the rate at which the request becomes the fault. */
/* 500ms was chosen to stop an IDR STORM when the pane anchor was stuck and every picture
 * asked for a keyframe. That fault is fixed, gaps are now rare, and the throttle instead
 * lengthens the one thing the user can see: after a gap the pane is HELD until the peer's
 * keyframe lands, so every millisecond of delay here is visible hold time. 120ms still caps
 * the peer at ~8 keyframes/sec in the worst case while cutting the hold substantially.
 *
 * HISTORY, BECAUSE THIS VALUE HAS BEEN WRONG IN BOTH DIRECTIONS. This and the queue depth
 * were retuned for the tile path AFTER horizontal was declared working (0c2c12ff), and both
 * live here, shared -- so the retune silently moved horizontal off its proven state and its
 * peer pane shredded (depth 12: peergap=621 idrreq=175/455; depth 4: 20 and 18/13). The fix
 * at the time was to split them per mode. THAT SPLIT IS NOW REMOVED: the real defect was that
 * horizontal could not emit catch-up pictures, so a deep queue could only fill and evict, and
 * every eviction cut the peer's chain. Horizontal owns its composite POC now and emits
 * catch-up like the tile path, so the deep queue drains instead of overflowing -- and the
 * shallow queue had become the BLOCKER, because COOP_VT_CATCHUP_START is 4 and a 4-slot queue
 * can never show the backlog catch-up triggers on. One set of values, both modes.
 */
#define COOP_IDR_REQ_MIN_MS_VT 120ul
static unsigned long coord_idr_req_min_ms = COOP_IDR_REQ_MIN_MS_VT;
static unsigned long coord_idr_req_ms = 0;
static unsigned coord_idr_sent = 0, coord_idr_suppressed = 0;

static unsigned long now_ms(void) {
    /* MONOTONIC, NOT WALL CLOCK. Every age computed from this is a DURATION, and a wall clock
     * can step -- NTP correction, a DST change, the TV's own time sync -- which would show up
     * as a peer pane that arrived in the future or aged by an hour. Raised by Astra in passover
     * 07 §3 while specifying the per-picture trace; the ages this stamp feeds are the whole
     * point of that trace, so the clock has to be one that only moves forward. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long) ts.tv_sec * 1000ul + (unsigned long) (ts.tv_nsec / 1000000ul);
}

/** True if a peer-IDR request may go out now; records that it did. */
static bool coord_idr_req_allowed(void) {
    const unsigned long now = now_ms();
    pthread_mutex_lock(&coord_lock);
    if (coord_idr_req_ms != 0 && now - coord_idr_req_ms < coord_idr_req_min_ms) {
        coord_idr_suppressed++;
        pthread_mutex_unlock(&coord_lock);
        return false;
    }
    coord_idr_req_ms = now;
    coord_idr_sent++;
    pthread_mutex_unlock(&coord_lock);
    return true;
}

static intptr_t coord_peer_snapshot(void) {
    pthread_mutex_lock(&coord_fd_lock);
    const intptr_t fd = coord_peer_fd;
    pthread_mutex_unlock(&coord_fd_lock);
    return fd;
}

/* Called only by coord_worker().  It detaches before close so stop() either sees a live
 * descriptor it may shutdown, or -1; it can never shutdown a newly reused descriptor. */
static void coord_worker_drop_peer(intptr_t expected_fd) {
    bool close_peer = false;
    pthread_mutex_lock(&coord_fd_lock);
    if (coord_peer_fd == expected_fd && expected_fd >= 0) {
        coord_peer_fd = -1;
        atomic_store(&coord_connected, false);
        close_peer = true;
    }
    pthread_mutex_unlock(&coord_fd_lock);
    if (close_peer) {
        close(expected_fd);
    }
}

static void *coord_worker(void *arg) {
    (void) arg;
    unsigned char *scratch = malloc(COOP_AU_CAP);
    if (scratch == NULL) {
        pthread_mutex_lock(&coord_lock);
        coord_start_error = COOP_IPC_CTL_FAIL_CONNECTION;
        coord_start_state = COOP_COORD_FAILED;
        pthread_mutex_unlock(&coord_lock);
        return NULL;
    }
    while (atomic_load(&coord_run)) {
        intptr_t peer_fd = coord_peer_snapshot();
        if (peer_fd < 0) {
            intptr_t fd = coop_ipc_accept(coord_listen_fd);
            if (fd < 0) {
                if (!atomic_load(&coord_run)) { break; }
                usleep(50000);
                continue;
            }
            if (!coop_ipc_set_nonblocking(fd) || !atomic_load(&coord_run)) {
                close(fd);
                continue;
            }
            pthread_mutex_lock(&coord_fd_lock);
            coord_peer_fd = fd;
            atomic_store(&coord_connected, true);
            pthread_mutex_unlock(&coord_fd_lock);
            continue;
        }
        if (atomic_exchange(&coord_idr_req, false) &&
            !coop_ipc_send_ctl(peer_fd, COOP_IPC_CTL_REQUEST_IDR)) {
            coord_worker_drop_peer(peer_fd);
            continue;
        }
        if (atomic_exchange(&coord_shutdown_req, false) &&
            !coop_ipc_send_ctl(peer_fd, COOP_IPC_CTL_SHUTDOWN)) {
            coord_worker_drop_peer(peer_fd);
            continue;
        }
        uint32_t frame = 0;
        bool idr = false;
        coop_ipc_hdr_t rhdr;
        memset(&rhdr, 0, sizeof(rhdr));
        long n = coop_ipc_recv_au_meta(peer_fd, scratch, COOP_AU_CAP, &frame, &idr, &rhdr);
        if (n < 0) {
            /* "The secondary stopped" was indistinguishable from a framing bug;
             * record which one so the next failure does not need another run. */
            FILE *f = fopen(AURORA_DIAG_LOG, "a");
            if (f != NULL) {
                pthread_mutex_lock(&coord_lock);
                const unsigned recv_count = coord_recv_count;
                pthread_mutex_unlock(&coord_lock);
                fprintf(f, "[COOP-COORD] peer dropped rc=%ld (%s) after recv=%u "
                           "stage=%s errno=%d (%s)\n",
                        n, n == -2 ? "FRAMING DESYNC" : "io error/eof", recv_count,
                        coop_ipc_last_stage, coop_ipc_last_errno,
                        coop_ipc_last_errno > 0 ? strerror(coop_ipc_last_errno) : "-");
                fclose(f);
            }
            coord_worker_drop_peer(peer_fd);
            continue;   /* secondary went away: wait for it to reconnect */
        }
        if (n == 0) {
            usleep(1000);
            continue;
        }
        if (frame == COOP_IPC_CTL_FRAME) {
            const uint8_t code = scratch[0];
            pthread_mutex_lock(&coord_lock);
            if (coord_start_state == COOP_COORD_PENDING && n == 1 &&
                code == COOP_IPC_CTL_READY) {
                coord_start_state = COOP_COORD_READY;
            } else if (coord_start_state == COOP_COORD_PENDING && n == 1 &&
                       code >= COOP_IPC_CTL_FAIL_CONFIG &&
                       code <= COOP_IPC_CTL_FAIL_LAUNCH) {
                coord_start_error = code;
                coord_start_state = COOP_COORD_FAILED;
            }
            pthread_mutex_unlock(&coord_lock);
            continue;
        }
        /* THE LINK'S OWN DWELL -- passover 07 §2C's deciding measurement, both halves of it.
         *   dwell        = secondary's send ENTRY -> coordinator's complete RECEIPT. Both stamps
         *                  are CLOCK_MONOTONIC and both processes run on the TV, so this
         *                  subtraction is legitimate where the LiGetMillis stamps are not.
         *   send_block   = how long the secondary's write_all() actually blocked, reported one
         *                  AU late (see coop_ipc.c). This is the "delayed writer" §2C names.
         * A wrapped 32-bit monotonic or a stale zero would show as a nonsense dwell, so a
         * negative/absurd difference is discarded rather than recorded. */
        pthread_mutex_lock(&coord_lock);
        {
            const uint32_t now32 = (uint32_t) now_ms();
            const uint32_t d = now32 - rhdr.send_enter_mono_ms;
            if (rhdr.send_enter_mono_ms != 0u && d < 60000u) {
                coord_ipc_dwell_last = d;
                if (d > coord_ipc_dwell_max) { coord_ipc_dwell_max = d; }
            }
            coord_send_block_last = rhdr.prev_send_ms;
            if (rhdr.prev_send_ms > coord_send_block_max && rhdr.prev_send_ms < 60000u) {
                coord_send_block_max = rhdr.prev_send_ms;
            }
        }
        if (idr && coord_idr_au != NULL && (size_t) n <= COOP_AU_CAP) {
            memcpy(coord_idr_au, scratch, (size_t) n);
            coord_idr_len = (size_t) n;
            coord_idr_frame = frame;
        }
        /* An access unit too big for a queue slot is an IDR; it is already kept whole in
         * the sticky slot above, and forcing it into the queue would mean either a 12MB
         * slot each or a truncated pane. Skip it here rather than corrupt the queue.
         *
         * THE ASSUMPTION "IT IS AN IDR" IS NOT CHECKED, and if it is ever wrong the pane
         * VANISHES SILENTLY: coord_replaced does not move, so no consumer sees an eviction, and
         * take_next later reports skipped == 0, so the splice runs straight across the hole with
         * the chain flag never set. Counted (not changed) so the assumption can be tested rather
         * than trusted -- `qoversize` non-zero with `qoversizep` non-zero means a NON-IDR pane
         * was dropped and the premise is false. */
        if ((size_t) n > COOP_Q_SLOT_CAP) {
            coord_oversize++;
            if (!idr) { coord_oversize_p++; }
        }
        if ((size_t) n <= COOP_Q_SLOT_CAP) {
            if (coord_q_count >= coord_q_depth) {
                /* Full: drop the OLDEST so the pane we show tracks the present. */
                coord_q_head = (coord_q_head + 1u) % COOP_Q_SLOTS;
                coord_q_count--;
                coord_replaced++;
            }
            unsigned slot = (coord_q_head + coord_q_count) % COOP_Q_SLOTS;
            if (coord_q[slot] != NULL) {
                memcpy(coord_q[slot], scratch, (size_t) n);
                coord_q_len[slot] = (size_t) n;
                coord_q_frame[slot] = frame;
                coord_q_idr[slot] = idr;
                coord_q_ms[slot] = now_ms();
                coord_q_meta[slot] = rhdr;
                coord_q_count++;
            }
        }
        coord_recv_count++;
        coord_last_recv_frame = frame;
        void (*wake)(void) = coord_wakeup;
        pthread_mutex_unlock(&coord_lock);
        if (wake != NULL) { wake(); }
    }
    coord_worker_drop_peer(coord_peer_snapshot());
    atomic_store(&coord_connected, false);
    free(scratch);
    return NULL;
}

#ifndef _WIN32
static bool coord_executable_path(const char *link, char path[PATH_MAX]) {
    ssize_t n = readlink(link, path, PATH_MAX - 1);
    if (n <= 0 || n == PATH_MAX - 1) { return false; }
    path[n] = '\0';
    /* An install can replace the file while an old secondary still holds it open. */
    const char suffix[] = " (deleted)";
    if ((size_t)n >= sizeof(suffix) - 1 &&
            strcmp(path + n - (sizeof(suffix) - 1), suffix) == 0) {
        path[n - (sizeof(suffix) - 1)] = '\0';
    }
    return true;
}

/* A disconnected secondary can reconnect to a later coordinator by socket path.
 * Reap only a process reparented to init, from THIS executable and THIS socket.
 * A secondary belonging to a live primary or another install is not ours to kill.
 * Whether old orphans caused the September 8 launch failure remains unproven. */
int coop_coord_reap_orphan_secondaries(const char *socket_path) {
    char self_exe[PATH_MAX];
#if defined(__ANDROID__)
    /* /proc/self/exe is the zygote's app_process on Android; a secondary is the packaged peer executable,
     * which path_self_executable() names (platform/android/path_android.c). */
    if (!path_self_executable(self_exe, sizeof(self_exe))) { return 0; }
#else
    if (!coord_executable_path("/proc/self/exe", self_exe)) { return 0; }
#endif
    DIR *proc = opendir("/proc");
    if (proc == NULL) {
        return 0;
    }
    const pid_t self = getpid();
    int killed = 0;
    struct dirent *e;
    while ((e = readdir(proc)) != NULL) {
        char *end = NULL;
        long pid = strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end != '\0' || pid <= 0 || (pid_t) pid == self) {
            continue;
        }
        char path[64];
        char peer_exe[PATH_MAX];
        snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
        if (!coord_executable_path(path, peer_exe) || strcmp(self_exe, peer_exe) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%ld/status", pid);
        FILE *status = fopen(path, "r");
        if (status == NULL) { continue; }
        char line[256];
        long parent = -1;
        unsigned real_uid = UINT_MAX, effective_uid = UINT_MAX;
        while (fgets(line, sizeof(line), status) != NULL) {
            if (strncmp(line, "PPid:", 5) == 0) { sscanf(line + 5, "%ld", &parent); }
            if (strncmp(line, "Uid:", 4) == 0) {
                sscanf(line + 4, "%u %u", &real_uid, &effective_uid);
            }
        }
        fclose(status);
        if (parent != 1 || effective_uid != (unsigned)geteuid()) { continue; }
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            continue;  /* the process exited between readdir and here, or is another uid */
        }
        char buf[512];
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (got == 0 || got == sizeof(buf) - 1 || buf[got - 1] != '\0') {
            continue;
        }
        buf[got] = '\0';
        /* /proc/<pid>/cmdline is NUL-separated; scan each argument for the secondary flag. */
        bool is_secondary = false;
        for (size_t i = 0; i < got;) {
            const char *arg = buf + i;
            if (strcmp(arg, "--coop-secondary") == 0 &&
                    i + strlen(arg) + 1 < got) {
                is_secondary = strcmp(arg + strlen(arg) + 1, socket_path) == 0;
                break;
            }
            i += strlen(arg) + 1;
        }
        if (is_secondary && kill((pid_t) pid, SIGKILL) == 0) {
            killed++;
        }
    }
    closedir(proc);
    return killed;
}
#endif /* !_WIN32 */

static void coord_release_queue(void) {
    pthread_mutex_lock(&coord_lock);
    for (unsigned i = 0; i < COOP_Q_SLOTS; ++i) {
        free(coord_q[i]);
        coord_q[i] = NULL;
        coord_q_len[i] = 0;
    }
    coord_q_head = coord_q_count = 0;
    /* The sticky allocation is process-lifetime storage, but its contents belong to one
     * session.  Retaining the buffer also avoids handing the worker a dangling pointer. */
    coord_idr_len = 0;
    coord_idr_frame = 0;
    pthread_mutex_unlock(&coord_lock);
}

bool coop_coord_start(const char *socket_path) {
    if (socket_path == NULL || coord_thread_started || coord_listen_fd >= 0) {
        return false;
    }
    /* A previous start may have failed after only some queue slots were allocated. */
    coord_release_queue();
    /* Sweep orphaned secondaries from earlier sessions before anything else -- see the note on
     * coop_coord_reap_orphan_secondaries(). Do it first so the fresh bind below cannot be joined
     * by a stale peer the moment the socket appears. */
    {
        const int reaped = coop_coord_reap_orphan_secondaries(socket_path);
        if (reaped > 0) {
            FILE *f = fopen(AURORA_DIAG_LOG, "a");
            if (f != NULL) {
                fprintf(f, "[COOP-COORD] reaped %d orphaned secondary process(es) before bind\n",
                        reaped);
                fclose(f);
            }
        }
    }
    /* THE PER-MODE SPLIT IS GONE, AND DELIBERATELY SO. It existed because horizontal could not
     * emit catch-up pictures, so a deep queue could only fill and evict, and every eviction cut
     * the peer's chain (measured: depth 12 -> peergap=621 idrreq=175/455; depth 4 -> 20 and
     * 18/13). Horizontal now owns its composite POC and CAN emit catch-up, which is what makes
     * a deep queue drain instead of overflow -- the same reason the tile path wants one. Worse,
     * the shallow queue then BLOCKED the fix: COOP_VT_CATCHUP_START is 4, so a 4-slot queue can
     * never present the backlog catch-up triggers on, and catchup stayed 0 with the POC work
     * already in place. Both modes take the tile path's proven values. */
    /* DEFAULT FIRST, THEN THE SENTINEL, AND THE ORDER IS THE WHOLE POINT (fixed 2026-09-06).
     * The per-session reset added below on 2026-09-06 (`coord_q_depth = COOP_Q_SLOTS`) was
     * placed AFTER this read, so it overwrote whatever /tmp/coop-queue had just supplied and
     * the depth was pinned at COOP_Q_SLOTS on every session. That killed a registered sentinel
     * and, with it, the user-facing `coop_queue_depth` setting that coop_tunables.c:56 writes
     * into that file -- silently, because a dead tunable and a tunable set to its default look
     * identical in every log. Assigning the default here and reading the file afterwards keeps
     * BOTH properties: nothing is inherited from a previous session, and the sentinel still
     * decides. */
    coord_q_depth = COOP_Q_SLOTS;
    /* Depth is read once at start: the video thread must never stat() the filesystem, and
     * a depth that changed mid-session would reorder panes. */
    FILE *qf = fopen("/tmp/coop-queue", "r");
    if (qf != NULL) {
        int v = 0;
        if (fscanf(qf, "%d", &v) == 1 && v >= 1 && v <= COOP_Q_SLOTS) {
            coord_q_depth = (unsigned) v;
        }
        fclose(qf);
    }
    pthread_mutex_lock(&coord_lock);
    coord_q_head = coord_q_count = 0;
    /* PER-SESSION COUNTERS. `coord_replaced` is what every eviction watcher in session_video.c
     * diffs against its own last-seen value, so carrying a previous session's total into a new
     * one makes the first sample see a jump and latch chain_broken -- which freezes the peer's
     * pane until an IDR arrives. Nothing reset these until 2026-09-05. */
    coord_recv_count = coord_replaced = 0;
    coord_oversize = coord_oversize_p = 0;
    coord_idr_sent = coord_idr_suppressed = 0;
    coord_idr_req_ms = 0;
    coord_last_recv_frame = 0;
    memset(&coord_taken_meta, 0, sizeof(coord_taken_meta));
    coord_taken_arrival_ms = 0;
    coord_ipc_dwell_last = coord_ipc_dwell_max = 0;
    coord_send_block_last = coord_send_block_max = 0;
    coord_start_state = COOP_COORD_PENDING;
    coord_start_error = 0;
    pthread_mutex_unlock(&coord_lock);
    /* LATCHED ACROSS SESSIONS AND MISSED UNTIL 2026-09-06. `coord_q_depth` is only ASSIGNED
     * when /tmp/coop-queue parses, so a session that set 4 leaves 4 in force for the next one
     * that has no file -- and teardown.sh now DELETES that file, which makes the stale case the
     * common one rather than the rare one. Its reset is now at the TOP of this function, ahead
     * of the sentinel read, because performing it here clobbered the sentinel.
     * `coord_idr_req` is a pending request from a session that has ended. Same class as the
     * sticky IDR below: process-global state that start() must own, not inherit. */
    atomic_store(&coord_idr_req, false);
    atomic_store(&coord_shutdown_req, false);
    atomic_store(&coord_connected, false);
    for (unsigned i = 0; i < COOP_Q_SLOTS; ++i) {
        coord_q[i] = malloc(COOP_Q_SLOT_CAP);
        coord_q_len[i] = 0;
        if (coord_q[i] == NULL) {
            coord_release_queue();
            return false;
        }
    }
    /* REUSE the sticky-IDR buffer across sessions -- stop() retains it (see the note there) so
     * the worker can never be handed a dangling pointer. Allocate only the first time. */
    if (coord_idr_au == NULL) {
        coord_idr_au = malloc(COOP_AU_CAP);
    }
    if (coord_idr_au == NULL) {
        coord_release_queue();
        return false;
    }
    pthread_mutex_lock(&coord_lock);
    coord_idr_len = 0;
    coord_idr_frame = 0;
    pthread_mutex_unlock(&coord_lock);
    coord_listen_fd = coop_ipc_listen(socket_path);
    if (coord_listen_fd < 0) {
        coord_release_queue();
        return false;
    }
    coord_socket_path = strdup(socket_path);
    if (coord_socket_path == NULL) {
        close(coord_listen_fd);
        coord_listen_fd = -1;
        unlink(socket_path);
        coord_release_queue();
        return false;
    }
    if (!coop_ipc_set_nonblocking(coord_listen_fd)) {
        coord_close_listener();
        coord_release_queue();
        return false;
    }
    pthread_mutex_lock(&coord_fd_lock);
    coord_peer_fd = -1;
    pthread_mutex_unlock(&coord_fd_lock);
    atomic_store(&coord_run, true);
    coord_thread_started = false;
    if (pthread_create(&coord_thread, NULL, coord_worker, NULL) != 0) {
        atomic_store(&coord_run, false);
        coord_close_listener();
        coord_release_queue();
        return false;
    }
    coord_thread_started = true;
    return true;
}

void coop_coord_stop(void) {
    atomic_store(&coord_run, false);
    /* Do not close here.  shutdown wakes a worker that is inside a partial framed read while
     * preserving the descriptor value until that worker has stopped using it. */
    pthread_mutex_lock(&coord_fd_lock);
    if (coord_peer_fd >= 0) {
        shutdown(coord_peer_fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&coord_fd_lock);

    if (coord_thread_started) {
        pthread_join(coord_thread, NULL);
        coord_thread_started = false;
    }
    /* The worker normally detached and closed the peer on its way out.  This fallback runs only
     * if it returned before adopting one; detaching under the lock still guarantees one close. */
    intptr_t leftover_peer = -1;
    pthread_mutex_lock(&coord_fd_lock);
    if (coord_peer_fd >= 0) {
        leftover_peer = coord_peer_fd;
        coord_peer_fd = -1;
    }
    atomic_store(&coord_connected, false);
    pthread_mutex_unlock(&coord_fd_lock);
    if (leftover_peer >= 0) {
        close(leftover_peer);
    }
    coord_close_listener();
    atomic_store(&coord_idr_req, false);
    atomic_store(&coord_shutdown_req, false);
    coord_release_queue();
}

#ifndef _WIN32
static pid_t coord_child = -1;

int coop_coord_spawn_secondary_for(const char *exe_path, const char *socket_path,
                                   const char *host_uuid, int app_id,
                                   int stream_width, int stream_height) {
    /* The child needs to know WHICH host and app to stream. Passing it through
     * the environment keeps the argv contract to the single --coop-secondary
     * flag and avoids quoting a JSON blob through exec. */
    unsetenv("COOP_HOST_UUID");
    unsetenv("COOP_APP_ID");
    unsetenv("COOP_STREAM_WIDTH");
    unsetenv("COOP_STREAM_HEIGHT");
    if (stream_width <= 0 || stream_height <= 0) {
        return -1;
    }
    if (host_uuid != NULL && setenv("COOP_HOST_UUID", host_uuid, 1) != 0) {
        return -1;
    }
    if (app_id > 0) {
        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "%d", app_id);
        if (setenv("COOP_APP_ID", idbuf, 1) != 0) {
            unsetenv("COOP_HOST_UUID");
            return -1;
        }
    }
    char width_buf[32];
    char height_buf[32];
    snprintf(width_buf, sizeof(width_buf), "%d", stream_width);
    snprintf(height_buf, sizeof(height_buf), "%d", stream_height);
    if (setenv("COOP_STREAM_WIDTH", width_buf, 1) != 0 ||
        setenv("COOP_STREAM_HEIGHT", height_buf, 1) != 0) {
        unsetenv("COOP_HOST_UUID");
        unsetenv("COOP_APP_ID");
        unsetenv("COOP_STREAM_WIDTH");
        unsetenv("COOP_STREAM_HEIGHT");
        return -1;
    }
    const int pid = coop_coord_spawn_secondary(exe_path, socket_path);
    /* These are launch arguments, not primary-process state.  Clear them even after a failed
     * spawn so a later launch with missing configuration cannot inherit this one's target. */
    unsetenv("COOP_HOST_UUID");
    unsetenv("COOP_APP_ID");
    unsetenv("COOP_STREAM_WIDTH");
    unsetenv("COOP_STREAM_HEIGHT");
    return pid;
}

int coop_coord_spawn_secondary(const char *exe_path, const char *socket_path) {
#ifdef __linux__
    const pid_t parent_pid = getpid();
#endif
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
#ifdef __linux__
        /* Also cover abnormal primary exit, when normal session cleanup cannot run.
         * SIGKILL cannot be blocked or replaced by the secondary's diagnostics. */
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent_pid) _exit(127);
#endif
        /* UNBLOCK EVERYTHING FIRST. THIS IS WHAT HUNG EVERY TEARDOWN.
         *
         * fork() inherits the calling thread's signal mask and exec() PRESERVES it --
         * exec resets HANDLERS to default but never touches the blocked mask. We fork
         * from the video setup path, which runs on an SDL-spawned thread, and SDL
         * blocks a broad set of signals on its threads. The child therefore came up
         * with SIGTERM blocked, so coop_coord_reap_secondary()'s kill() landed as a
         * PENDING signal that could never be delivered: the child kept running, and
         * the waitpid() below it never returned.
         *
         * Measured on the TV 2026-08-07 on a live hung session (primary 10417,
         * secondary 4130), straight out of /proc/4130/status:
         *     ShdPnd: 0000000000004000   <- bit 14, SIGTERM sent and pending
         *     SigBlk: 000000000e017007   <- bit 14 also set: SIGTERM BLOCKED
         * The child had been forwarding panes normally for 13 minutes while the
         * primary sat in waitpid() with the session stuck at "Disconnecting...".
         * It is why /tmp/coop_secondary.log has never once recorded the handler's
         * "!!! secondary FATAL signal=15" line, and why all 7 teardowns in
         * /tmp/aurora-diag.log stop dead at "Stopping video stream..." and never
         * reach the control stream.
         *
         * The mask is the child's own business, and a freshly exec'd secondary wants
         * none of the parent's: clear it before exec so every signal we later send it
         * is actually deliverable. */
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_UNBLOCK, &all, NULL);
        /* Child: exec ourselves in secondary mode. exec rather than continuing
         * the fork so no inherited SDL/GL state can touch the compositor. */
        execl(exe_path, exe_path, "--coop-secondary", socket_path, (char *) NULL);
        _exit(127);
    }
    coord_child = pid;
    return pid;
}

/* Poll for the child for up to timeout_ms. True if it was reaped. */
static bool coord_wait_child(unsigned timeout_ms) {
    for (unsigned waited = 0; waited < timeout_ms; waited += 10) {
        int status = 0;
        pid_t r = waitpid(coord_child, &status, WNOHANG);
        if (r == coord_child || (r < 0 && errno != EINTR)) {
            return true;   /* exited, or no such child left to wait for */
        }
        usleep(10000);
    }
    return false;
}

void coop_coord_reap_secondary(void) {
    if (coord_child <= 0) {
        return;
    }
    /* BOUNDED, AND ESCALATING. This is the first blocking call in
     * vdec_delegate_cleanup(), so anything that stalls here stalls the whole
     * teardown with the UI showing "Disconnecting..." -- which is exactly what a
     * plain waitpid(..., 0) did when the child came up with SIGTERM blocked (see
     * the mask note in coop_coord_spawn_secondary above). That specific cause is
     * fixed, but "the child did not die" must never again be able to hang the
     * close: a secondary wedged in a driver call would do it just as well.
     *
     * session_video posts COOP_IPC_CTL_SHUTDOWN before calling this function. Give that
     * cooperative path a bounded chance to destroy the Moonlight session first; SIGTERM
     * and then SIGKILL are fallbacks for a broken control link or wedged child. */
    if (coord_wait_child(2000)) {
        coord_child = -1;
        return;
    }
    kill(coord_child, SIGTERM);
    if (coord_wait_child(2000)) {
        coord_child = -1;
        return;
    }
    kill(coord_child, SIGKILL);
    if (!coord_wait_child(2000)) {
        /* Nothing left to try, and blocking here is the failure we are removing.
         * Drop the reference and let init reap it -- we are on our way out of the
         * process anyway, and a zombie costs the user nothing while a hung
         * teardown costs them the whole TV. */
        FILE *f = fopen(AURORA_DIAG_LOG, "a");
        if (f != NULL) {
            fprintf(f, "[COOP-COORD] secondary pid=%d survived SIGKILL; abandoning it\n",
                    (int) coord_child);
            fclose(f);
        }
    }
    coord_child = -1;
}

bool coop_coord_child_exited(uint32_t *status_out) {
    /* Non-blocking: the child dying is invisible from the coordinator otherwise,
     * and "no frames arriving" looks identical to a crash, a clean exit and a
     * child that simply has nothing to send. */
    if (coord_child <= 0) {
        return false;
    }
    int status = 0;
    pid_t r = waitpid(coord_child, &status, WNOHANG);
    if (r != coord_child) {
        return false;              /* still running */
    }
    coord_child = -1;
    if (WIFSIGNALED(status)) {
        if (status_out != NULL) {
            *status_out = (uint32_t) (1000 + WTERMSIG(status));
        }
        return true;
    }
    if (status_out != NULL) {
        *status_out = (uint32_t) (WIFEXITED(status) ? WEXITSTATUS(status) : 999);
    }
    return true;
}
#endif /* !_WIN32 */

size_t coop_coord_take_idr(unsigned char *out, size_t cap) {
    size_t len = 0;
    pthread_mutex_lock(&coord_lock);
    if (coord_idr_len > 0 && coord_idr_len <= cap) {
        memcpy(out, coord_idr_au, coord_idr_len);
        len = coord_idr_len;
    }
    pthread_mutex_unlock(&coord_lock);
    return len;   /* NOT consumed: the bootstrap may need several attempts */
}

void coop_coord_request_peer_idr_keep(void) {
    /* Request WITHOUT dropping the sticky IDR. The clearing variant below exists so a
     * re-bootstrap does not splice an IDR from seconds ago; during the INITIAL bootstrap
     * the opposite is true -- any peer IDR we are holding is exactly what our next IRAP
     * needs to pair with, and clearing it on every request meant the two never coincided.
     * Measured: with the clearing version the half-height path sat at boot=0 for 721
     * pictures with idrunpaired=21, having thrown away each IDR shortly before it was
     * wanted. */
    if (coord_idr_req_allowed()) {
        atomic_store(&coord_idr_req, true);
    }
}

void coop_coord_request_peer_idr(void) {
    /* Drop the sticky IDR at the same time. It is only kept so a pane IDR can
     * wait for one of ours to pair with, and the one we hold is now stale by
     * however long the pane has been running -- splicing it would hand the other
     * host's inter slices a reference picture from seconds ago. Clearing it makes
     * the bootstrap wait for the keyframe we are asking for. */
    /* Throttled as ONE unit: clearing the sticky IDR without sending the request meant to
     * replace it would leave the bootstrap with nothing to pair against. */
    if (!coord_idr_req_allowed()) {
        return;
    }
    pthread_mutex_lock(&coord_lock);
    coord_idr_len = 0;
    pthread_mutex_unlock(&coord_lock);
    atomic_store(&coord_idr_req, true);
}

void coop_coord_request_peer_shutdown(void) {
    /* As with IDR requests, only coord_worker writes the framed socket.  A
     * direct write from the teardown thread could interleave with that worker
     * and corrupt the next message. */
    atomic_store(&coord_shutdown_req, true);
}

bool coop_coord_wait_secondary_ready(unsigned timeout_ms) {
    for (unsigned waited = 0; waited < timeout_ms; waited += 10) {
        pthread_mutex_lock(&coord_lock);
        const enum coop_coord_start_state state = coord_start_state;
        pthread_mutex_unlock(&coord_lock);
        if (state == COOP_COORD_READY) {
            return true;
        }
        if (state == COOP_COORD_FAILED) {
            return false;
        }
        uint32_t child_status = 0;
        if (coop_coord_child_exited(&child_status)) {
            pthread_mutex_lock(&coord_lock);
            if (coord_start_error == 0) {
                coord_start_error = COOP_IPC_CTL_FAIL_CONNECTION;
            }
            coord_start_state = COOP_COORD_FAILED;
            pthread_mutex_unlock(&coord_lock);
            return false;
        }
        usleep(10000);
    }
    pthread_mutex_lock(&coord_lock);
    coord_start_error = COOP_IPC_CTL_FAIL_CONNECTION;
    coord_start_state = COOP_COORD_FAILED;
    pthread_mutex_unlock(&coord_lock);
    return false;
}

const char *coop_coord_start_error_message(void) {
    pthread_mutex_lock(&coord_lock);
    const uint8_t error = coord_start_error;
    pthread_mutex_unlock(&coord_lock);
    switch (error) {
        case COOP_IPC_CTL_FAIL_CONFIG:
            return "The co-op helper did not receive a valid host and app configuration.";
        case COOP_IPC_CTL_FAIL_HOST:
            return "The second co-op PC is no longer paired with this client.";
        case COOP_IPC_CTL_FAIL_QUERY:
            return "The second co-op PC did not answer the startup check.";
        case COOP_IPC_CTL_FAIL_APP:
            return "The selected co-op app is not installed on the second PC.";
        case COOP_IPC_CTL_FAIL_SESSION:
            return "The second co-op PC could not create its streaming session.";
        case COOP_IPC_CTL_FAIL_BUSY:
            return "The second co-op PC is already running another app. Close that stream or app "
                   "on it, then start co-op again.";
        case COOP_IPC_CTL_FAIL_LAUNCH:
            return "The second co-op PC refused to start the co-op app.";
        case COOP_IPC_CTL_FAIL_CONNECTION:
        default:
            return "The second co-op PC did not begin streaming in time.";
    }
}

bool coop_coord_connected(void) { return atomic_load(&coord_connected); }
void coop_coord_set_wakeup(void (*wake)(void)) {
    pthread_mutex_lock(&coord_lock);
    coord_wakeup = wake;
    pthread_mutex_unlock(&coord_lock);
}
static unsigned coord_counter_snapshot(const unsigned *counter) {
    pthread_mutex_lock(&coord_lock);
    const unsigned value = *counter;
    pthread_mutex_unlock(&coord_lock);
    return value;
}

unsigned coop_coord_received(void) { return coord_counter_snapshot(&coord_recv_count); }
unsigned coop_coord_replaced(void) { return coord_counter_snapshot(&coord_replaced); }
unsigned coop_coord_oversize(void) { return coord_counter_snapshot(&coord_oversize); }
unsigned coop_coord_oversize_nonidr(void) { return coord_counter_snapshot(&coord_oversize_p); }
unsigned coop_coord_idr_sent(void) { return coord_counter_snapshot(&coord_idr_sent); }
unsigned coop_coord_idr_suppressed(void) { return coord_counter_snapshot(&coord_idr_suppressed); }

size_t coop_coord_take_latest(unsigned char *out, size_t cap,
                              uint32_t *frame_out, bool *is_idr_out,
                              unsigned long *age_ms_out) {
    /* Name kept for its callers; it now pops the OLDEST queued pane rather than the
     * newest-and-only one. Oldest is the right end to take: consecutive pictures then get
     * consecutive panes, which is what the peer's own prediction chain assumes. Taking
     * the newest would skip panes and reintroduce the desync the queue exists to remove. */
    size_t n = 0;
    pthread_mutex_lock(&coord_lock);
    if (coord_q_count > 0) {
        unsigned slot = coord_q_head;
        if (coord_q_len[slot] > 0 && coord_q_len[slot] <= cap) {
            memcpy(out, coord_q[slot], coord_q_len[slot]);
            n = coord_q_len[slot];
            if (frame_out != NULL) { *frame_out = coord_q_frame[slot]; }
            if (is_idr_out != NULL) { *is_idr_out = coord_q_idr[slot]; }
            if (age_ms_out != NULL) { *age_ms_out = now_ms() - coord_q_ms[slot]; }
        }
        coord_q_head = (coord_q_head + 1u) % COOP_Q_SLOTS;
        coord_q_count--;
    }
    pthread_mutex_unlock(&coord_lock);
    return n;
}

unsigned coop_coord_queued(void) {
    return coord_counter_snapshot(&coord_q_count);
}

/* The newest peer frame number to have FINISHED ARRIVING, whether or not anything has taken it.
 * Passover 07 §3.4 wants "latest received and accepted identities" side by side on the panel:
 * received is this, accepted is what the compositor last spliced. When the two diverge and stay
 * diverged, the peer half on the glass is behind the peer half in the building. */
uint32_t coop_coord_last_recv_frame(void) {
    pthread_mutex_lock(&coord_lock);
    const uint32_t frame = coord_last_recv_frame;
    pthread_mutex_unlock(&coord_lock);
    return frame;
}

uint32_t coop_coord_idr_frame(void) {
    pthread_mutex_lock(&coord_lock);
    const uint32_t frame = coord_idr_frame;
    pthread_mutex_unlock(&coord_lock);
    return frame;
}

size_t coop_coord_take_next(unsigned char *out, size_t cap, uint32_t min_frame,
                            uint32_t *frame_out, unsigned *skipped_out) {
    return coop_coord_take_next_aged(out, cap, min_frame, frame_out, skipped_out, NULL);
}

#ifdef COOP_PC_DECODER
bool coop_coord_next_is_idr(uint32_t min_frame) {
    bool idr = false;
    pthread_mutex_lock(&coord_lock);
    for (unsigned i = 0; i < coord_q_count; ++i) {
        const unsigned slot = (coord_q_head + i) % COOP_Q_SLOTS;
        if (coord_q_frame[slot] < min_frame) {
            continue;   /* take_next would discard it as stale */
        }
        idr = coord_q_idr[slot];
        break;
    }
    pthread_mutex_unlock(&coord_lock);
    return idr;
}
#endif

/* THE AGE OF THE PANE WE JUST TOOK -- ms between its COMPLETE arrival in the coordinator and
 * this take. Passover 07 §2B names exactly this as the deciding measurement for whether peer
 * pictures wait on the primary's callback: "measure peer complete-arrival -> accepted composite
 * age, including periods with no primary callback". The arrival stamp already existed
 * (coord_q_ms); nothing returned it, so no caller could see a pane that had been sitting. */
size_t coop_coord_take_next_aged(unsigned char *out, size_t cap, uint32_t min_frame,
                                 uint32_t *frame_out, unsigned *skipped_out,
                                 unsigned long *age_ms_out) {
    /* THE PEER'S FRAMES MUST BE FED IN AN UNBROKEN RUN, and the queue alone cannot
     * guarantee it. Each of the peer's inter pictures predicts from the peer's PREVIOUS
     * picture, so if we hand the decoder frame k+3 straight after frame k, its reference
     * is a picture the composite never contained. Measured 2026-07-30: the bottom pane was
     * PERFECT on the bootstrap IRAP (frame 3 of the dump) and collapsed to residual-only
     * by frame 30 -- not concealment drift (repeated=0) and not the TV (the same collapse
     * is in the bitstream, decoded offline).
     *
     * Panes older than what we expect are stale leftovers: discard them and keep looking.
     * A pane NEWER than expected is a real gap, and the caller has to treat it as such
     * rather than splice it, because nothing can reconstruct the frames in between. */
    size_t n = 0;
    unsigned skipped = 0;
    pthread_mutex_lock(&coord_lock);
    while (coord_q_count > 0) {
        unsigned slot = coord_q_head;
        uint32_t f = coord_q_frame[slot];
        if (f < min_frame) {
            coord_q_head = (coord_q_head + 1u) % COOP_Q_SLOTS;
            coord_q_count--;
            skipped++;
            continue;
        }
        if (coord_q_len[slot] > 0 && coord_q_len[slot] <= cap) {
            memcpy(out, coord_q[slot], coord_q_len[slot]);
            /* How long this pane sat in the coordinator before anything took it -- passover 07 §2B. */
            if (age_ms_out != NULL) {
                const unsigned long arrived = coord_q_ms[slot];
                const unsigned long t = now_ms();
                *age_ms_out = t > arrived ? t - arrived : 0ul;
            }
            n = coord_q_len[slot];
            if (frame_out != NULL) { *frame_out = f; }
            /* Provenance of the pane actually accepted, for the per-picture trace. Recorded
             * here rather than returned through another out-parameter so the four existing
             * call sites keep their signatures; it is read immediately after the take. */
            coord_taken_meta = coord_q_meta[slot];
            coord_taken_arrival_ms = coord_q_ms[slot];
        }
        coord_q_head = (coord_q_head + 1u) % COOP_Q_SLOTS;
        coord_q_count--;
        break;
    }
    pthread_mutex_unlock(&coord_lock);
    if (skipped_out != NULL) { *skipped_out = skipped; }
    return n;
}

/* Provenance and link timing for the trace. All four are snapshots, not a stream: read them
 * straight after the take they describe. Passover 07 §3.1 (identity/timestamps) and §2C
 * (IPC dwell). */
void coop_coord_last_taken_meta(coop_ipc_hdr_t *out, unsigned long *arrival_ms_out) {
    pthread_mutex_lock(&coord_lock);
    if (out != NULL) { *out = coord_taken_meta; }
    if (arrival_ms_out != NULL) { *arrival_ms_out = coord_taken_arrival_ms; }
    pthread_mutex_unlock(&coord_lock);
}

void coop_coord_ipc_timing(uint32_t *dwell_last, uint32_t *dwell_max,
                           uint32_t *send_block_last, uint32_t *send_block_max) {
    pthread_mutex_lock(&coord_lock);
    if (dwell_last != NULL) { *dwell_last = coord_ipc_dwell_last; }
    if (dwell_max != NULL) { *dwell_max = coord_ipc_dwell_max; }
    if (send_block_last != NULL) { *send_block_last = coord_send_block_last; }
    if (send_block_max != NULL) { *send_block_max = coord_send_block_max; }
    pthread_mutex_unlock(&coord_lock);
}

void coop_coord_flush(void) {
    /* Drop every queued pane. Called when the composite bootstraps on the peer's IDR:
     * the queue can still hold panes from BEFORE that IDR, and feeding those afterwards
     * hands the decoder slices whose references precede the keyframe -- the bottom pane
     * then never fills in, it only accumulates residual, which shows as a black pane
     * with coloured edges. */
    pthread_mutex_lock(&coord_lock);
    coord_q_head = coord_q_count = 0;
    pthread_mutex_unlock(&coord_lock);
}
