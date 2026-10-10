/* Prove the AVC all-skip concealer is PIXEL-EXACT.
 * Composes: frame 0 = real peer pane (IDR), frames 1..N = SYNTHESISED ALL-SKIP for the peer.
 * If the concealer is correct, every later frame's peer half is byte-identical to frame 0's,
 * because P_Skip is MV(0,0) with no residual against the previous picture.
 * Usage: avc_conceal_cli <primary.264> <peer.264> <out.264> [frames] [legacy]
 *
 * DEFAULT IS THE LIVE CONFIGURATION: the peer's WHOLE ANNEX-B ACCESS UNIT as the header donor,
 * and `renumber = true`. `legacy` (any 5th argument) reverts BOTH to the shapes this harness used
 * before 2026-09-06 -- a bare NAL donor and the verbatim copy path -- for comparing against an
 * older result. **The 5th argument used to be called `audonor` and meant the OPPOSITE**; the
 * sense was inverted when the live shapes became the default, and this header said so until
 * 2026-09-06. Passing a bare NAL is why every offline validation of the concealer passed while
 * the live donor never worked once. Measured 2026-09-06 across four netcap
 * captures: byte 0 of an AU is 0x00, so nal_unit_type read 0 and nal_ref_idc forced the header
 * to 0x60, and the composite's peer slice was emitted as an UNSPECIFIED NAL that every
 * conforming decoder discards -- 69.7% of pictures in `toppad1`, with the whole 240x68 peer pane
 * error-concealed by the decoder and every counter reading healthy.
 *
 * Both modes now ASSERT ON THE OUTPUT, which is the part that was missing: this test proved the
 * pixels were exact and never checked that the slice carrying them was a legal NAL type.
 * A type-0 NAL in the output fails the run (exit 4) in EITHER mode.
 */
#include "coop_avc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t) sz); *n = fread(b, 1, (size_t) sz, f); fclose(f); return b;
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
#define MAXAU 4096
typedef struct { size_t start, end; } rng_t;
static int split(const unsigned char *d, size_t len, rng_t *r, int maxn) {
    int n = 0; size_t i = 0, off, nl, au_start = 0; int seen = 0;
    while ((i = nal_next(d, len, i, &off, &nl)) != 0) {
        int t = d[off] & 0x1F;
        size_t sc_start = (off >= 4 && !d[off-4]) ? off - 4 : off - 3;
        if ((t == 1 || t == 5) && seen && (d[off+1] & 0x80)) {
            if (n < maxn) { r[n].start = au_start; r[n].end = sc_start; n++; }
            au_start = sc_start; seen = 0;
        }
        if (t == 1 || t == 5) seen = 1;
    }
    if (n < maxn) { r[n].start = au_start; r[n].end = len; n++; }
    return n;
}
/* Every VCL NAL the composite emits must be type 1 or 5. Anything else -- above all type 0,
 * "Unspecified", which decoders MUST ignore -- means the half is not on screen. */
static int check_nal_types(const unsigned char *d, size_t len, int *n_unspec) {
    size_t i = 0, off, nl; int bad = 0;
    while ((i = nal_next(d, len, i, &off, &nl)) != 0) {
        int t = d[off] & 0x1F;
        if (t == 0 || t > 12) { bad++; fprintf(stderr, "  ILLEGAL NAL type=%d header=0x%02x\n", t, d[off]); }
    }
    *n_unspec = bad;
    return bad == 0;
}
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s prim peer out [frames] [legacy]\n", argv[0]); return 2; }
    size_t an, bn; unsigned char *A = slurp(argv[1], &an), *B = slurp(argv[2], &bn);
    if (!A || !B) return 2;
    int limit = argc > 4 ? atoi(argv[4]) : 30;
    static rng_t ra[MAXAU], rb[MAXAU];
    int na = split(A, an, ra, MAXAU), nb = split(B, bn, rb, MAXAU);
    static coop_avc_state_t st; memset(&st, 0, sizeof st);
    /* THE LIVE SHAPES ARE THE DEFAULT (2026-09-06). Both were opt-in or absent, so a default run
     * validated a configuration that does not ship -- twice over. `renumber` is set true by
     * session_video.c and routes OUR half through the rewriter instead of a verbatim memcpy, and
     * the live donor is always a whole ANNEX-B AU. Pass a 5th argument for the legacy bare-NAL /
     * verbatim path when comparing against an old result. SET BEFORE THE FIRST COMPOSE -- frame 0
     * is composed further down and would otherwise run with the memset default. */
    const int legacy = argc > 5;
    st.renumber = !legacy;
    static unsigned char out[1u << 22];
    FILE *o = fopen(argv[3], "wb");
    int emitted = 0, concealed = 0, skipped = 0;
    /* frame 0: the real thing, to establish the picture the skips will copy */
    size_t n0 = coop_avc_compose(A + ra[0].start, ra[0].end - ra[0].start,
                                 B + rb[0].start, rb[0].end - rb[0].start,
                                 out, sizeof out, &st);
    if (!n0) { fprintf(stderr, "frame 0 compose failed\n"); return 3; }
    fwrite(out, 1, n0, o); emitted++;
    /* keep the peer's last real slice as the header donor for the concealer */
    static unsigned char ref[1u << 20]; size_t reflen = 0;
    /* Donor must be a P slice -- AU0's is the IDR. Take AU1's, and compose AU1 for real so the
     * decoder has the picture the skips will copy. */
    const int au_donor = !legacy;   /* see the `legacy` note where it is read, above */
    if (au_donor) {
        /* THE LIVE SHAPE: the whole access unit, start code and all. */
        reflen = rb[1].end - rb[1].start;
        if (reflen > sizeof ref) { fprintf(stderr, "donor AU too big\n"); return 3; }
        memcpy(ref, B + rb[1].start, reflen);
    } else {
        size_t i = 0, off, nl;
        while ((i = nal_next(B + rb[1].start, rb[1].end - rb[1].start, i, &off, &nl)) != 0) {
            int t = (B + rb[1].start)[off] & 0x1F;
            if (t == 1) { memcpy(ref, B + rb[1].start + off, nl); reflen = nl; break; }
        }
    }
    { size_t n1 = coop_avc_compose(A + ra[1].start, ra[1].end - ra[1].start,
                                   B + rb[1].start, rb[1].end - rb[1].start,
                                   out, sizeof out, &st);
      if (n1) { fwrite(out, 1, n1, o); emitted++; } }
    if (!reflen) { fprintf(stderr, "no peer slice in AU0\n"); return 3; }

    for (int k = 2; k < na && emitted < limit; k++) {
        /* our AU verbatim + a SYNTHESISED all-skip peer half */
        size_t n = coop_avc_compose_concealed(A + ra[k].start, ra[k].end - ra[k].start,
                                              ref, reflen, out, sizeof out, &st);
        if (!n) {
            /* Expected, not fatal: the concealer refuses an IDR picture -- an all-skip slice is a
             * P slice and a coded picture may not mix IDR and non-IDR slices. The live path pairs
             * our IDR with the peer's sticky IDR instead. Skip and keep going. */
            skipped++;
            continue;
        }
        int unspec = 0;
        if (!check_nal_types(out, n, &unspec)) {
            fprintf(stderr, "FAIL: composite %d carries %d unspecified/illegal NAL(s) -- the peer's "
                            "half is not on screen. donor shape = %s\n",
                    emitted, unspec, au_donor ? "WHOLE AU (the live shape)" : "bare NAL");
            fclose(o);
            return 4;
        }
        fwrite(out, 1, n, o); emitted++; concealed++;
    }
    fclose(o);
    printf("emitted=%d (2 real + %d CONCEALED) skipped_idr=%d peer_first_mb=%u rows=%u "
           "donor=%s renumber=%d%s\n",
           emitted, concealed, skipped, st.peer_first_mb, st.comp_sps.height_mbs,
           au_donor ? "whole-AU" : "bare-NAL", st.renumber ? 1 : 0,
           legacy ? "  (LEGACY -- not the shipping configuration)" : "  (live configuration)");
    return 0;
}
