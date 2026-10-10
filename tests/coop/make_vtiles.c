/* VERTICAL SPLIT VIA HEVC TILE COLUMNS -- 2 slices per picture instead of 128.
 *
 * The per-CTB-row version works and splices bit-exact, but it needs one slice per row (128
 * for a composite), and this TV's decoder does not render that smoothly: measured on the
 * panel, a 2-slice composite is clean while a 128-slice one flashes and micro-stutters.
 * Slices force that, because a slice must be a contiguous run in RASTER order and so can
 * only be a horizontal band. TILES have no such rule -- a tile COLUMN is exactly a
 * left/right half.
 *
 * WHY A PANE'S BITS DROP STRAIGHT INTO A TILE. Within a tile, CTBs are scanned in raster
 * order OF THE TILE. For a 2-column composite that is 60 CTBs per row, row by row --
 * byte-for-byte the scan of a 1920-wide pane. So the pane's entropy-coded slice data is
 * already in the right order; only the headers change. And because slice_segment_address is
 * specified in TILE SCAN order (H.265 7.4.7.1), tile 1's slices simply sit at
 * (pane CTBs) + their pane address.
 *
 * Each tile is its own SLICE, deliberately: a slice ends with end_of_slice_segment_flag = 1
 * and a CABAC flush, exactly as the pane's data already does. Packing both tiles into ONE
 * slice would need that terminating bin rewritten from 1 to 0 plus an end_of_subset bit --
 * arithmetic-coder surgery on the tail, requiring the encoder state to be reconstructed.
 * Two slices avoids all of it and costs nothing.
 *
 * Usage: make_vtiles <paneA.h265> <paneB.h265> <out.h265> [max_pictures]
 *   Panes must be identical encodes. Constrained encoding is still required: it confines
 *   motion vectors to the slice region, whose width IS the pane width, so they cannot reach
 *   into the other player's tile.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_splice.h"
#include "coop_poc.h"

static const unsigned char SC[4] = {0, 0, 0, 1};
#define MAXSL 256

typedef struct {
    const unsigned char *p[MAXSL];
    size_t len[MAXSL];
    int n;
} pic_t;

static int split_pictures(const unsigned char *buf, size_t n, pic_t *pics, int max_pics) {
    int np = 0;
    size_t pos = 0;
    while (pos + 5 <= n && np <= max_pics) {
        size_t next = pos + 4;
        while (next + 4 <= n && memcmp(buf + next, SC, 4) != 0) { next++; }
        const size_t end = (next + 4 <= n) ? next : n;
        const unsigned char *nal = buf + pos + 4;
        const size_t nlen = end - pos - 4;
        if (nlen > 3 && ((nal[0] >> 1) & 0x3F) <= 31) {
            if ((nal[2] >> 7) & 1) {
                if (++np > max_pics) { break; }
                pics[np - 1].n = 0;
            }
            if (np > 0 && pics[np - 1].n < MAXSL) {
                pic_t *pc = &pics[np - 1];
                pc->p[pc->n] = nal;
                pc->len[pc->n] = nlen;
                pc->n++;
            }
        }
        pos = next;
        if (next + 4 > n) { break; }
    }
    return np;
}

int main(int argc, char **argv) {
    if (argc < 4) { printf("usage: make_vtiles <A.h265> <B.h265> <out.h265> [max]\n"); return 1; }
    const int max_pics = (argc > 4) ? atoi(argv[4]) : 200;

    static unsigned char A[192 << 20], B[192 << 20];
    FILE *fa = fopen(argv[1], "rb"), *fb = fopen(argv[2], "rb");
    if (!fa || !fb) { printf("FAIL: cannot open panes\n"); return 1; }
    size_t na = fread(A, 1, sizeof(A), fa), nb = fread(B, 1, sizeof(B), fb);
    fclose(fa); fclose(fb);

    coop_poc_params_t src;
    if (!coop_poc_params_parse(A, na, &src) || !src.valid) { printf("FAIL: pane params\n"); return 1; }
    coop_au_t head;
    if (!coop_au_parse(A, na, &head) || head.non_vcl == NULL) { printf("FAIL: no param sets\n"); return 1; }

    static unsigned char nv[64 << 10];
    size_t nvlen = coop_nonvcl_tile_columns(head.non_vcl, head.non_vcl_len, nv, sizeof(nv));
    if (!nvlen) { printf("FAIL: tile parameter-set rewrite\n"); return 1; }
    coop_poc_params_t comp;
    if (!coop_poc_params_parse(nv, nvlen, &comp) || !comp.valid) { printf("FAIL: composite params\n"); return 1; }
    const unsigned tile_ctbs = src.w_ctbs * src.h_ctbs;
    (void) tile_ctbs;
    printf("pane %ux%u CTBs -> composite %ux%u CTBs, tiles=%d, addr_bits %u -> %u, tile1 base %u\n",
           src.w_ctbs, src.h_ctbs, comp.w_ctbs, comp.h_ctbs, (int) comp.tiles_enabled,
           src.slice_addr_bits, comp.slice_addr_bits, tile_ctbs);
    if (!comp.tiles_enabled || comp.w_ctbs != src.w_ctbs * 2) {
        printf("FAIL: composite is not a 2-column tiled picture\n");
        return 1;
    }

    static pic_t pa[600], pb[600];
    int npa = split_pictures(A, na, pa, 600), npb = split_pictures(B, nb, pb, 600);
    printf("pane A: %d pictures (%d slices each), pane B: %d pictures\n",
           npa, npa ? pa[0].n : 0, npb);
    if (npa == 0 || npb == 0) { printf("FAIL: no pictures\n"); return 1; }

    FILE *o = fopen(argv[3], "wb");
    if (!o) { printf("FAIL: cannot write output\n"); return 1; }
    static unsigned char work[8 << 20], work2[8 << 20];
    int pics = 0, refused = 0;
    size_t total = 0;
    const int np = (npa < npb) ? npa : npb;
    for (int i = 0; i < np && pics < max_pics; ++i) {
        if (pa[i].n == 0 || pb[i].n == 0) { break; }
        const int ta = (pa[i].p[0][0] >> 1) & 0x3F;
        const int tb = (pb[i].p[0][0] >> 1) & 0x3F;
        const bool convert_b = (ta == 1 && (tb == 19 || tb == 20));
        if (ta != tb && !convert_b) { refused++; continue; }

        unsigned my_poc = 0;
        const bool have_poc = coop_poc_read(&src, pa[i].p[0], pa[i].len[0], &my_poc);

        fwrite(nv, 1, nvlen, o);
        total += nvlen;
        int ok = 1;
        for (int side = 0; side < 2 && ok; ++side) {
            const pic_t *pc = side ? &pb[i] : &pa[i];
            for (int k = 0; k < pc->n && ok; ++k) {
                const unsigned char *sl = pc->p[k];
                size_t sl_len = pc->len[k];
                bool first = false;
                unsigned addr = 0;
                if (!coop_slice_address(&src, sl, sl_len, &first, &addr)) { ok = 0; break; }
                /* slice_segment_address is a RASTER address, NOT a tile-scan one -- checked
                 * on the decoder, not assumed. Putting tile 1 at (tile 0's CTB count) placed
                 * its slices at raster row 34 column 0, i.e. the MIDDLE-LEFT of the picture,
                 * which is exactly how the first render came out. Map the pane's raster
                 * address through the composite's wider rows instead: a pane CTB at
                 * (col, row) sits at row*Wc + side*Wp + col. The slice is still contiguous in
                 * TILE scan, which is what the standard actually requires of a slice. */
                const unsigned prow = addr / src.w_ctbs;
                const unsigned pcol = addr % src.w_ctbs;
                const unsigned want = prow * comp.w_ctbs + (side ? src.w_ctbs : 0u) + pcol;

                if (side && convert_b) {
                    size_t cn = coop_slice_idr_to_trail(&src, sl, sl_len, 0, work, sizeof(work));
                    if (!cn) { ok = 0; break; }
                    sl = work; sl_len = cn;
                }
                if (side && have_poc) {
                    size_t pn = coop_poc_rewrite(&src, sl, sl_len, my_poc, work2, sizeof(work2));
                    if (pn > 0) { sl = work2; sl_len = pn; }
                }
                static unsigned char outsl[8 << 20];
                const unsigned char *emit = sl;
                size_t emit_len = sl_len;
                /* One conversion for every case: enabling tiles adds num_entry_point_offsets
                 * to the header, so even the composite's FIRST slice has to be rewritten. */
                (void) first;
                emit_len = coop_slice_to_tile(&src, &comp, sl, sl_len, want, outsl, sizeof(outsl));
                emit = outsl;
                if (emit_len == 0) { ok = 0; break; }
                fwrite(SC, 1, 4, o);
                fwrite(emit, 1, emit_len, o);
                total += 4 + emit_len;
            }
        }
        if (!ok) { refused++; continue; }
        pics++;
    }
    fclose(o);
    printf("wrote %s: %d pictures (%d refused), %zu bytes, %d slices each\n",
           argv[3], pics, refused, total, pics ? pa[0].n + pb[0].n : 0);
    if (!pics) { printf("FAIL: nothing composed\n"); return 1; }
    printf("PASS(built): expect %ux%u with TWO TILE COLUMNS\n", comp.w_ctbs * 32, comp.h_ctbs * 32);
    return 0;
}
