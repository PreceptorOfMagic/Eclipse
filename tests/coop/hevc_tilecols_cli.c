/* OFFLINE PROBE: compose two half-WIDTH HEVC panes into ONE two-tile-COLUMN picture.
 *
 * WHY THIS EXISTS. The vertical (left/right) mode composes by interleaving one slice per CTB
 * ROW, which needs as many slices as the pane has rows -- and NVENC caps a picture at 64
 * slices (measured 2026-09-12: asked 70 got 64, asked 135 got 64, both codecs). A 2160-tall
 * pane is 68 CTB rows at CTB 32, so the interleave cannot reach 4K on an NVIDIA host.
 *
 * The HORIZONTAL mode does not have that problem, because it does not interleave: it turns
 * each pane's SINGLE slice into a TILE ROW (coop_au_splice_tile_rows). Tiles work in both
 * directions, so the same trick should give left/right with one slice per pane and no cap.
 *
 * This driver builds exactly that, from the shipping pieces -- coop_nonvcl_tile_columns() for
 * the composite parameter sets and the tiled splice for the picture. The splice itself is
 * geometry-agnostic: the row-ness or column-ness lives entirely in those parameter sets and in
 * the peer's CTB address, which is why it is reused here unchanged.
 *
 * With --rows it builds the HORIZONTAL composite instead -- coop_nonvcl_tile_rows(), the first
 * pane on top and the second below -- for tests of what a decoder makes of tile rows.
 *
 * Usage: hevc_tilecols_cli [--rows] <left.265> <right.265> <out.265> [frames]
 */
#include "coop_splice.h"
#include "coop_poc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXAU 400

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (f == NULL) { return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t) sz);
    *n = fread(b, 1, (size_t) sz, f);
    fclose(f);
    return b;
}

typedef struct { size_t start, len; } au_t;

/* Split into access units. A new picture opens at a VCL NAL whose first_slice_segment_in_pic
 * flag is set -- the top bit of the byte after the two-byte HEVC NAL header. */
static int split_aus(const unsigned char *d, size_t n, au_t *out, int maxn) {
    int cnt = 0;
    size_t au_start = 0;
    bool seen_vcl = false;
    for (size_t i = 0; i + 4 < n; ++i) {
        size_t sc = 0;
        if (!d[i] && !d[i+1] && !d[i+2] && d[i+3] == 1) { sc = 4; }
        else if (!d[i] && !d[i+1] && d[i+2] == 1) { sc = 3; }
        if (!sc) { continue; }
        const size_t body = i + sc;
        if (body + 2 >= n) { break; }
        const int t = (d[body] >> 1) & 0x3F;
        const bool vcl = t <= 31;
        const bool first = vcl && (d[body + 2] & 0x80) != 0;
        if (first && seen_vcl) {
            if (cnt < maxn) { out[cnt].start = au_start; out[cnt].len = i - au_start; cnt++; }
            au_start = i;
            seen_vcl = false;
        }
        if (vcl) { seen_vcl = true; }
        i = body;                      /* skip past this header */
    }
    if (cnt < maxn && n > au_start) { out[cnt].start = au_start; out[cnt].len = n - au_start; cnt++; }
    return cnt;
}

int main(int argc, char **argv) {
    const bool rows = argc > 1 && strcmp(argv[1], "--rows") == 0;
    if (rows) {
        argv[1] = argv[0];
        argv++;
        argc--;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: %s [--rows] <left.265> <right.265> <out.265> [frames]\n", argv[0]);
        return 2;
    }
    size_t ln = 0, rn = 0;
    unsigned char *L = slurp(argv[1], &ln), *R = slurp(argv[2], &rn);
    if (L == NULL || R == NULL) { fprintf(stderr, "cannot read inputs\n"); return 1; }

    static au_t la[MAXAU], ra[MAXAU];
    const int lc = split_aus(L, ln, la, MAXAU);
    const int rc = split_aus(R, rn, ra, MAXAU);
    printf("left AUs %d, right AUs %d\n", lc, rc);

    /* The composite's parameter sets: width doubled AND two uniform tile columns. Built from
     * the LEFT pane's own sets, exactly as the app builds them from the primary's. */
    static unsigned char comp_nonvcl[65536];
    coop_au_t a0;
    if (!coop_au_parse(L + la[0].start, la[0].len, &a0) || a0.non_vcl == NULL) {
        fprintf(stderr, "left AU 0 has no parameter sets\n");
        return 1;
    }
    const size_t comp_n = rows
            ? coop_nonvcl_tile_rows(a0.non_vcl, a0.non_vcl_len, comp_nonvcl, sizeof(comp_nonvcl))
            : coop_nonvcl_tile_columns(a0.non_vcl, a0.non_vcl_len, comp_nonvcl, sizeof(comp_nonvcl));
    if (comp_n == 0) { fprintf(stderr, "composite parameter sets REFUSED\n"); return 1; }

    coop_poc_params_t a_src, b_src, comp;
    if (!coop_poc_params_parse(L + la[0].start, la[0].len, &a_src) ||
        !coop_poc_params_parse(R + ra[0].start, ra[0].len, &b_src) ||
        !coop_poc_params_parse(comp_nonvcl, comp_n, &comp)) {
        fprintf(stderr, "parameter parse failed\n");
        return 1;
    }
    printf("left pane %ux%u CTBs, right %ux%u, composite %ux%u  tiles=%d\n",
           a_src.w_ctbs, a_src.h_ctbs, b_src.w_ctbs, b_src.h_ctbs,
           comp.w_ctbs, comp.h_ctbs, (int) comp.tiles_enabled);
    if (rows ? comp.h_ctbs != a_src.h_ctbs + b_src.h_ctbs : comp.w_ctbs != a_src.w_ctbs + b_src.w_ctbs) {
        fprintf(stderr, "GEOMETRY: the panes do not add up to the composite (%ux%u CTBs)\n",
                comp.w_ctbs, comp.h_ctbs);
        return 1;
    }
    /* The peer's pane is tile column 1: its first CTB is row 0, column w/2, and
     * slice_segment_address is a RASTER address, so that is simply w_ctbs / 2. As tile row 1
     * it starts at the bottom half's first CTB. */
    const unsigned peer_addr = rows ? coop_bottom_half_addr(&comp) : comp.w_ctbs / 2;
    printf("peer tile %s starts at CTB raster address %u\n", rows ? "row" : "column", peer_addr);

    FILE *out = fopen(argv[3], "wb");
    if (out == NULL) { return 1; }
    const int want = argc > 4 ? atoi(argv[4]) : 0;
    const int frames = want > 0 && want < (lc < rc ? lc : rc) ? want : (lc < rc ? lc : rc);

    static unsigned char buf[1 << 22];
    int made = 0, refused = 0, multislice = 0;
    for (int f = 0; f < frames; ++f) {
        coop_au_t a, b;
        if (!coop_au_parse(L + la[f].start, la[f].len, &a) ||
            !coop_au_parse(R + ra[f].start, ra[f].len, &b)) { refused++; continue; }
        if (a.slice_count != 1 || b.slice_count != 1) { multislice++; continue; }
        const size_t got = coop_au_splice_tile_rows(&a, &b, &a_src, &b_src, &comp, peer_addr,
                                                    comp_nonvcl, comp_n, buf, sizeof(buf));
        if (got == 0) { refused++; continue; }
        fwrite(buf, 1, got, out);
        made++;
    }
    fclose(out);
    printf("composed %d, refused %d, wrong slice count %d\n", made, refused, multislice);
    return made == 0;
}
