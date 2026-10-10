/* TWO-PANE composite: splice pane A (top) and pane B (bottom) through Aurora's REAL splicer.
 *
 * make_composite.c uses one pane for both halves and is deliberately all-intra, because
 * (per its header) inter slices re-addressed into the bottom half read into the OTHER pane
 * unless the encoder constrained its motion vectors. THAT is exactly what this tool exists
 * to test: give it INTER panes from an encoder that can constrain MVs (kvazaar
 * --mv-constraint) and the bleed should disappear.
 *
 * Usage: make_composite2 <top.h265> <bottom.h265> <out.h265> [max_pictures]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_splice.h"
#include "coop_poc.h"

static const unsigned char SC[4] = {0, 0, 0, 1};
static unsigned char bufA[192 << 20];
static unsigned char bufB[192 << 20];
static unsigned char nv[64 << 10];
static unsigned char out[32 << 20];

static size_t slurp(const char *p, unsigned char *dst, size_t cap) {
    FILE *f = fopen(p, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", p); return 0; }
    size_t n = fread(dst, 1, cap, f);
    fclose(f);
    return n;
}

/* Advance to the next VCL NAL start offset at or after *pos; returns 0 when exhausted. */
size_t g_vcl_start = 0;

/* Handles BOTH 3-byte and 4-byte start codes -- kvazaar mixes them (4-byte for
 * VPS/SPS/PPS and TRAIL, 3-byte for SEI and the IDR), which is legal and which the
 * production parser already copes with via next_start_code(). */
static size_t sc_at(const unsigned char *b, size_t n, size_t i, size_t *sclen) {
    if (i + 3 <= n && b[i] == 0 && b[i+1] == 0 && b[i+2] == 1) { *sclen = 3; return 1; }
    if (i + 4 <= n && b[i] == 0 && b[i+1] == 0 && b[i+2] == 0 && b[i+3] == 1) { *sclen = 4; return 1; }
    return 0;
}
static int next_vcl(const unsigned char *b, size_t n, size_t *pos, size_t *end) {
    size_t sl;
    while (*pos < n) {
        if (!sc_at(b, n, *pos, &sl)) { (*pos)++; continue; }
        size_t payload = *pos + sl;
        if (payload >= n) { return 0; }
        int t = (int) ((b[payload] >> 1) & 0x3F);
        size_t j = payload, jl;
        while (j < n && !sc_at(b, n, j, &jl)) { j++; }
        size_t start = *pos;
        *pos = j;
        if (t <= 31) { g_vcl_start = start; *end = j; return 1; }
        if (j >= n) { return 0; }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { printf("usage: make_composite2 <top.h265> <bottom.h265> <out.h265> [max]\n"); return 1; }
    const int max_pics = (argc > 4) ? atoi(argv[4]) : 32;

    size_t nA = slurp(argv[1], bufA, sizeof(bufA));
    size_t nB = slurp(argv[2], bufB, sizeof(bufB));
    if (!nA || !nB) { return 1; }

    coop_poc_params_t sA, sB;
    if (!coop_poc_params_parse(bufA, nA, &sA) || !sA.valid) { printf("FAIL: top params\n"); return 1; }
    if (!coop_poc_params_parse(bufB, nB, &sB) || !sB.valid) { printf("FAIL: bottom params\n"); return 1; }

    coop_au_t headA;
    if (!coop_au_parse(bufA, nA, &headA) || headA.non_vcl == NULL) { printf("FAIL: no param sets\n"); return 1; }
    size_t nvlen = coop_nonvcl_double_height(headA.non_vcl, headA.non_vcl_len, nv, sizeof(nv));
    if (!nvlen) { printf("FAIL: non-vcl rewrite\n"); return 1; }

    coop_poc_params_t comp;
    if (!coop_poc_params_parse(nv, nvlen, &comp) || !comp.valid) { printf("FAIL: composite params\n"); return 1; }
    const unsigned bottom = coop_bottom_half_addr(&comp);
    printf("panes %ux%u CTBs -> composite %ux%u CTBs (%ux%u px), bottom addr %u\n",
           sA.w_ctbs, sA.h_ctbs, comp.w_ctbs, comp.h_ctbs,
           comp.w_ctbs * 32, comp.h_ctbs * 32, bottom);

    FILE *o = fopen(argv[3], "wb");
    if (!o) { printf("FAIL: cannot write %s\n", argv[3]); return 1; }

    size_t pa = 0, pb = 0, ea = 0, eb = 0, total = 0;
    int pics = 0, skipped = 0;
    while (pics < max_pics) {
        if (!next_vcl(bufA, nA, &pa, &ea)) { break; }
        size_t sa = g_vcl_start;
        if (!next_vcl(bufB, nB, &pb, &eb)) { break; }
        size_t sb = g_vcl_start;

        coop_au_t auA, auB;
        if (!coop_au_parse(bufA + sa, ea - sa, &auA) || auA.slice[0] == NULL) { skipped++; continue; }
        if (!coop_au_parse(bufB + sb, eb - sb, &auB) || auB.slice[0] == NULL) { skipped++; continue; }

        if (pics < 3) { printf("  pic %d: sliceA nal=%d len=%zu , sliceB nal=%d len=%zu\n", pics+1, (auA.slice[0][0]>>1)&0x3F, auA.slice_len[0], (auB.slice[0][0]>>1)&0x3F, auB.slice_len[0]); }
        size_t got = coop_au_splice_halves(&auA, &auB, &comp, bottom, nv, nvlen, out, sizeof(out));
        if (got > 0) {
            fwrite(out, 1, got, o);
            total += got;
            pics++;
        } else {
            skipped++;
            if (pics == 0 && skipped > 4) { printf("FAIL: compose returned 0 repeatedly\n"); fclose(o); return 1; }
        }
    }
    fclose(o);
    printf("wrote %s: %d composite pictures, %zu bytes (skipped %d)\n", argv[3], pics, total, skipped);
    return pics ? 0 : 1;
}
