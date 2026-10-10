#include "coop_trace.h"

#include "coop_coord.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef AURORA_TRACE_SENTINEL
#define AURORA_TRACE_SENTINEL "/tmp/aurora-agetrace"
#endif
#ifndef AURORA_TRACE_LOG
#define AURORA_TRACE_LOG "/tmp/aurora-coop-trace.csv"
#endif

/* THE BOUND. At ~115 composites/s a run of 20 minutes is ~138,000 rows; this allows an hour
 * and then stops. It exists so a forgotten sentinel cannot fill the TV's /tmp -- which is a
 * tmpfs on this box, so filling it takes the whole app down, not just the trace. */
#define COOP_TRACE_MAX_ROWS 450000u

/* HOW OFTEN THE HEARTBEAT SPEAKS. Passover 07 §3.2: "Include a periodic heartbeat so a stopped
 * feed path cannot censor its own stall." The [COOP-AGE] line cannot do this -- it prints FROM
 * the feed path, so the one event it most needs to report (the feed stopping) is exactly the
 * event that silences it. 500 ms is well under the seconds-scale symptom and costs ~2 rows/s. */
#define COOP_TRACE_HB_MS 500u

static FILE *trace_f = NULL;
static pthread_t trace_hb;
static bool trace_hb_running = false;
static volatile bool trace_stop = false;
static unsigned trace_rows = 0;
static bool trace_capped = false;
static coop_trace_sampler_t trace_sampler = NULL;
static unsigned long trace_last_pic_ms = 0;
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;

unsigned long coop_trace_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long) ts.tv_sec * 1000ul + (unsigned long) (ts.tv_nsec / 1000000ul);
}

static const char *kind_name(coop_trace_kind_t k) {
    switch (k) {
        case COOP_TRACE_FRESH:     return "fresh";
        case COOP_TRACE_HELD:      return "held";
        case COOP_TRACE_BROKEN:    return "broken";
        case COOP_TRACE_CATCHUP:   return "catchup";
        case COOP_TRACE_MIRROR:    return "mirror";
        case COOP_TRACE_BOOTSTRAP: return "boot";
        default:                   return "?";
    }
}

/* Returns false once the cap is hit, having written the reason exactly once. Call with the
 * lock held. */
static bool trace_room(void) {
    if (trace_f == NULL) { return false; }
    if (trace_rows < COOP_TRACE_MAX_ROWS) { return true; }
    if (!trace_capped) {
        trace_capped = true;
        fprintf(trace_f, "cap,%lu,rows=%u,TRACE STOPPED AT ITS ROW CAP -- "
                         "the session continued; this file is truncated by design\n",
                coop_trace_now_ms(), trace_rows);
        fflush(trace_f);
    }
    return false;
}

static void *trace_hb_thread(void *arg) {
    (void) arg;
    while (!trace_stop) {
        usleep(COOP_TRACE_HB_MS * 1000u);
        if (trace_stop) { break; }
        coop_trace_state_t st;
        memset(&st, 0, sizeof(st));
        if (trace_sampler != NULL) { trace_sampler(&st); }
        const unsigned long now = coop_trace_now_ms();
        pthread_mutex_lock(&trace_lock);
        if (trace_room()) {
            /* `since_pic` IS THE POINT OF THIS ROW. It is the ms since the last composed
             * picture was traced, measured from a thread the feed path cannot stall. A feed
             * path that has stopped shows here as since_pic climbing while every other
             * instrument in the app goes quiet. */
            fprintf(trace_f,
                    "hb,%lu,since_pic=%lu,hold_open=%lu,hold_max=%lu,broke_open=%lu,"
                    "broke_max=%lu,recov_req=%lu,recov_anchor=%lu,q=%u\n",
                    now, trace_last_pic_ms ? now - trace_last_pic_ms : 0ul,
                    st.hold_open_ms, st.hold_max_ms, st.broken_open_ms, st.broken_max_ms,
                    st.recov_req_ms, st.recov_anchor_ms, st.queued);
            trace_rows++;
            fflush(trace_f);
        }
        pthread_mutex_unlock(&trace_lock);
    }
    return NULL;
}

bool coop_trace_open(coop_trace_sampler_t sampler) {
    if (trace_f != NULL) { return true; }
    if (access(AURORA_TRACE_SENTINEL, F_OK) != 0) { return false; }
    trace_f = fopen(AURORA_TRACE_LOG, "w");
    if (trace_f == NULL) { return false; }
    trace_sampler = sampler;
    trace_stop = false;
    trace_rows = 0;
    trace_capped = false;
    trace_last_pic_ms = 0;
    /* A HEADER ROW NAMING THE CLOCK. Passover 07 §3 insists the trace and any panel
     * observation share "one monotonic timebase and matching session epoch"; a reader who
     * does not know which clock these stamps are on cannot align them to anything. */
    fprintf(trace_f, "# eclipse co-op per-picture trace\n"
                     "# clock=CLOCK_MONOTONIC ms; opened at %lu\n"
                     "# peer recv/reasm are the SECONDARY process's LiGetMillis epoch --\n"
                     "#   differences within the peer stream only, NEVER against a prim_ stamp\n"
                     "# pts are capture-derived (epoch = first captured frame of that session)\n"
                     "# flags: 1=host_ms absent/NA  2=pts zero\n",
            coop_trace_now_ms());
    fflush(trace_f);
    if (pthread_create(&trace_hb, NULL, trace_hb_thread, NULL) == 0) {
        trace_hb_running = true;
    }
    return true;
}

void coop_trace_close(void) {
    if (trace_f == NULL) { return; }
    trace_stop = true;
    if (trace_hb_running) {
        pthread_join(trace_hb, NULL);
        trace_hb_running = false;
    }
    pthread_mutex_lock(&trace_lock);
    fprintf(trace_f, "# closed at %lu after %u rows\n", coop_trace_now_ms(), trace_rows);
    fclose(trace_f);
    trace_f = NULL;
    pthread_mutex_unlock(&trace_lock);
}

bool coop_trace_active(void) {
    return trace_f != NULL && !trace_capped;
}

static uint32_t ident_prim = 0, ident_peer_acc = 0;
static bool ident_any = false;

void coop_trace_identity_set(uint32_t prim_frame, uint32_t peer_accepted_frame) {
    ident_prim = prim_frame;
    if (peer_accepted_frame != 0) { ident_peer_acc = peer_accepted_frame; }
    ident_any = true;
}

bool coop_trace_identity_get(uint32_t *prim_frame, uint32_t *peer_accepted,
                             uint32_t *peer_received, unsigned long *mono_ms) {
    if (prim_frame != NULL) { *prim_frame = ident_prim; }
    if (peer_accepted != NULL) { *peer_accepted = ident_peer_acc; }
    if (peer_received != NULL) { *peer_received = coop_coord_last_recv_frame(); }
    if (mono_ms != NULL) { *mono_ms = coop_trace_now_ms(); }
    return ident_any;
}

void coop_trace_pic(const coop_trace_pic_t *p) {
    if (p == NULL) { return; }
    const unsigned long now = coop_trace_now_ms();
    pthread_mutex_lock(&trace_lock);
    if (trace_room()) {
        fprintf(trace_f,
                "pic,%lu,%s,"
                "pf=%u,ppts=%u,precv=%u,preasm=%u,phost=%u,"
                "xf=%u,xpts=%u,xrecv=%u,xreasm=%u,xhost=%u,xflags=%u,"
                "xage=%lu,dwell=%u,sblock=%u,"
                "fe=%lu,fx=%lu,fr=%d,bytes=%u,q=%u\n",
                now, kind_name(p->kind),
                p->prim_frame, p->prim_pts_ms, p->prim_recv_ms, p->prim_reasm_ms,
                (unsigned) p->prim_host_10us,
                p->peer.frame, p->peer.pts_ms, p->peer.recv_ms, p->peer.reasm_ms,
                (unsigned) p->peer.host_10us, (unsigned) p->peer.flags,
                p->peer_arrival_age_ms, p->ipc_dwell_ms, p->send_block_ms,
                p->feed_enter_mono_ms, p->feed_exit_mono_ms, p->feed_result,
                p->bytes, p->queued_after);
        trace_rows++;
        trace_last_pic_ms = now;
        /* Flushed every row on purpose: the failure this trace exists to catch is the app
         * going quiet, and a buffered tail is lost exactly then. */
        fflush(trace_f);
    } else {
        trace_last_pic_ms = now;
    }
    pthread_mutex_unlock(&trace_lock);
}
