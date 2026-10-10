/* Offline driver for coop_avc_compose() -- the SAME entry point session_video.c calls.
 * Proves the shipping code path against real captures without the app.
 * Usage: avc_compose_cli <primary.264> <peer.264> <out.264> [frames]
 */
#include "coop_avc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t) sz);
    *n = fread(b, 1, (size_t) sz, f); fclose(f); return b;
}
static size_t nal_next(const unsigned char *d, size_t len, size_t from, size_t *off, size_t *nl) {
    size_t i = from;
    while (i < len) {
        size_t sc = 0;
        if (i + 3 <= len && !d[i] && !d[i+1] && d[i+2] == 1) sc = 3;
        else if (i + 4 <= len && !d[i] && !d[i+1] && !d[i+2] && d[i+3] == 1) sc = 4;
        if (!sc) { i++; continue; }
        size_t s = i + sc, e = s;
        while (e < len) {
            if (e + 3 <= len && !d[e] && !d[e+1] && d[e+2] == 1) break;
            if (e + 4 <= len && !d[e] && !d[e+1] && !d[e+2] && d[e+3] == 1) break;
            e++;
        }
        size_t L = e - s; while (L > 0 && d[s+L-1] == 0) L--;
        *off = s; *nl = L; return e;
    }
    return 0;
}
/* Byte ranges of each AU, so whole-AU buffers can be handed to compose() as the app does. */
#define MAXAU 4096
typedef struct { size_t start, end; } rng_t;
static int split(const unsigned char *d, size_t len, rng_t *r, int maxn) {
    int n = 0; size_t i = 0, off, nl, au_start = 0; int seen = 0; size_t prev_sc = 0;
    while ((i = nal_next(d, len, i, &off, &nl)) != 0) {
        int t = d[off] & 0x1F;
        size_t sc_start = off - 4; if (off >= 3 && d[off-3] == 0 && d[off-2] == 0 && d[off-1] == 1) sc_start = off - 3;
        if ((t == 1 || t == 5) && seen && (d[off+1] & 0x80)) {
            if (n < maxn) { r[n].start = au_start; r[n].end = sc_start; n++; }
            au_start = sc_start; seen = 0;
        }
        if (t == 1 || t == 5) seen = 1;
        prev_sc = sc_start; (void) prev_sc;
    }
    if (n < maxn) { r[n].start = au_start; r[n].end = len; n++; }
    return n;
}
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s prim peer out [frames] [noren]\n", argv[0]); return 2; }
    size_t an, bn; unsigned char *A = slurp(argv[1], &an), *B = slurp(argv[2], &bn);
    if (!A || !B) { fprintf(stderr, "read failed\n"); return 2; }
    int limit = argc > 4 ? atoi(argv[4]) : 40;
    static rng_t ra[MAXAU], rb[MAXAU];
    int na = split(A, an, ra, MAXAU), nb = split(B, bn, rb, MAXAU);
    static coop_avc_state_t st; memset(&st, 0, sizeof st);
    /* MATCH THE LIVE PATH. session_video.c sets `coop_avc.renumber = true`, which routes OUR
     * half through the rewriter instead of copying it verbatim -- a different code path with
     * different output. This harness ran with the memset default (false) and therefore validated
     * a configuration that does not ship; it is the same gap that let the concealer's donor-shape
     * bug through offline validation. Default is now the LIVE value; pass a 5th argument to get
     * the old verbatim path back for comparison. */
    st.renumber = (argc <= 5);
    static unsigned char out[1u << 22];
    FILE *o = fopen(argv[3], "wb");
    int emitted = 0, refused = 0;
    for (int k = 0; k < na && k < nb && emitted < limit; k++) {
        size_t n = coop_avc_compose(A + ra[k].start, ra[k].end - ra[k].start,
                                    B + rb[k].start, rb[k].end - rb[k].start,
                                    out, sizeof out, &st);
        if (n == 0) { refused++; continue; }
        fwrite(out, 1, n, o); emitted++;
    }
    fclose(o);
    printf("composed=%d refused=%d (incompatible=%u mixed_idr=%u) peer_first_mb=%u comp_rows=%u\n",
           emitted, refused, st.refused_incompatible, st.refused_mixed_idr,
           st.peer_first_mb, st.comp_sps.height_mbs);
    return 0;
}
