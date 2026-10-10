/* DOES A VERTICAL (LEFT/RIGHT) CO-OP COMPOSITE DECODE, AND DOES THE SEAM HOLD?
 *
 * Memory recorded side-by-side as impossible. The reasoning -- HEVC slices are contiguous
 * CTUs in RASTER order, so a slice is always a horizontal band -- rules out ONE SLICE PER
 * PANE, not a vertical split. With one slice per CTB ROW the addresses work out:
 *
 *     composite row r  =  A's row r at address r*Wc          (Wp CTBs)
 *                      +  B's row r at address r*Wc + Wp     (Wp CTBs)
 *
 * where Wp is the pane width in CTBs and Wc = 2*Wp the composite's. Both are contiguous
 * runs, so both are legal slices. This builds exactly that and hands it to a decoder.
 *
 * THE REAL QUESTION IS THE SEAM, NOT THE ADDRESSING. Inter prediction can still FETCH
 * across the pane boundary: a block near the right edge of A read edge PADDING in A's own
 * narrow encode, but in the composite those samples are PLAYER B. The horizontal design
 * killed the equivalent problem with NVENC constrained encoding, which works there because
 * slice == pane; here slice == one CTB row, so constraining would confine motion search to
 * a 32px band. So the two panes are deliberately given VISUALLY DIFFERENT content: if A
 * bleeds into B, it shows up as A's picture smeared down B's left edge, and it will grow
 * with each inter frame rather than appear all at once.
 *
 * Usage: make_vsplit <paneA.h265> <paneB.h265> <out.h265> [max_pictures]
 *   Both panes must be encoded identically (same size, qp, GOP) with ONE SLICE PER CTB ROW.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_splice.h"
#include "coop_poc.h"

static const unsigned char SC[4] = {0, 0, 0, 1};

#define MAX_SLICES 128

typedef struct {
    const unsigned char *p[MAX_SLICES];
    size_t len[MAX_SLICES];
    int n;
} pic_t;

/* Split a stream into pictures. A new picture starts at a VCL NAL whose
 * first_slice_segment_in_pic_flag is set -- the top bit of the byte after the 2-byte NAL
 * header, which needs no bit-reader. */
static int split_pictures(const unsigned char *buf, size_t n, pic_t *pics, int max_pics) {
    int np = 0;
    size_t pos = 0;
    while (pos + 5 <= n && np < max_pics) {
        size_t next = pos + 4;
        while (next + 4 <= n && memcmp(buf + next, SC, 4) != 0) { next++; }
        const size_t end = (next + 4 <= n) ? next : n;
        const unsigned char *nal = buf + pos + 4;
        const size_t nlen = end - pos - 4;
        const int t = (nal[0] >> 1) & 0x3F;
        if (t <= 31 && nlen > 3) {
            const int first = (nal[2] >> 7) & 1;
            if (first || np == 0) {
                if (first) { np++; }
                if (np > max_pics) { break; }
                pics[np - 1].n = 0;
            }
            pic_t *pc = &pics[np - 1];
            if (pc->n < MAX_SLICES) {
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

static unsigned char *slurp(const char *path, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) { return NULL; }
    static unsigned char *bufs[2];
    static int which = 0;
    unsigned char *b = malloc(192u << 20);
    if (!b) { fclose(f); return NULL; }
    *out_n = fread(b, 1, 192u << 20, f);
    fclose(f);
    bufs[which++ & 1] = b;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 4) { printf("usage: make_vsplit <A.h265> <B.h265> <out.h265> [max]\n"); return 1; }
    const int max_pics = (argc > 4) ? atoi(argv[4]) : 30;

    size_t na = 0, nb = 0;
    unsigned char *A = slurp(argv[1], &na);
    unsigned char *B = slurp(argv[2], &nb);
    if (!A || !B) { printf("FAIL: cannot read panes\n"); return 1; }

    coop_poc_params_t src;
    if (!coop_poc_params_parse(A, na, &src) || !src.valid) { printf("FAIL: pane params\n"); return 1; }

    coop_au_t head;
    if (!coop_au_parse(A, na, &head) || head.non_vcl == NULL) { printf("FAIL: no param sets\n"); return 1; }
    static unsigned char nv[64 << 10];
    size_t nvlen = coop_nonvcl_double_width(head.non_vcl, head.non_vcl_len, nv, sizeof(nv));
    if (!nvlen) { printf("FAIL: non-vcl width rewrite\n"); return 1; }

    coop_poc_params_t comp;
    if (!coop_poc_params_parse(nv, nvlen, &comp) || !comp.valid) { printf("FAIL: composite params\n"); return 1; }
    printf("pane %ux%u CTBs -> composite %ux%u CTBs (addr_bits %u -> %u)\n",
           src.w_ctbs, src.h_ctbs, comp.w_ctbs, comp.h_ctbs,
           src.slice_addr_bits, comp.slice_addr_bits);
    if (comp.w_ctbs != src.w_ctbs * 2 || comp.h_ctbs != src.h_ctbs) {
        printf("FAIL: composite geometry wrong (expected width doubled, height unchanged)\n");
        return 1;
    }

    /* 600 to match the client's play-from-file cap: a 64-picture loop is 0.39s at 165fps
     * and its per-cycle IDR makes the whole picture flash. */
    static pic_t pa[600], pb[600];
    int npa = split_pictures(A, na, pa, 600);
    int npb = split_pictures(B, nb, pb, 600);
    printf("pane A: %d pictures (%d slices in #1), pane B: %d pictures (%d slices in #1)\n",
           npa, npa ? pa[0].n : 0, npb, npb ? pb[0].n : 0);
    if (npa == 0 || npb == 0) { printf("FAIL: no pictures\n"); return 1; }
    if (pa[0].n != (int) src.h_ctbs) {
        printf("FAIL: need EXACTLY one slice per CTB row -- got %d slices for %u rows.\n"
               "      A slice spanning two rows is not contiguous in the composite.\n",
               pa[0].n, src.h_ctbs);
        return 1;
    }

    FILE *o = fopen(argv[3], "wb");
    if (!o) { printf("FAIL: cannot write %s\n", argv[3]); return 1; }
    static unsigned char reA[8 << 20], reB[8 << 20];
    int pics = 0, refused = 0;
    size_t total = 0;
    const int np = (npa < npb) ? npa : npb;
    for (int i = 0; i < np && pics < max_pics; ++i) {
        if (pa[i].n != pb[i].n || pa[i].n != (int) src.h_ctbs) { break; }
        coop_pane_pic_t A2, B2;
        A2.n = (unsigned) pa[i].n;
        B2.n = (unsigned) pb[i].n;
        for (int r = 0; r < pa[i].n; ++r) {
            A2.slice[r] = pa[i].p[r]; A2.slice_len[r] = pa[i].len[r];
            B2.slice[r] = pb[i].p[r]; B2.slice_len[r] = pb[i].len[r];
        }
        /* The composition itself lives in coop_splice.c so the client and this tool cannot
         * drift apart -- the horizontal path's bugs were all in code that existed twice. */
        static unsigned char au[32 << 20];
        size_t got = coop_au_splice_columns(&A2, &B2, &src, &comp, nv, nvlen, au, sizeof(au));
        if (got == 0) {
            printf("  picture %d: splice refused (types A=%d B=%d)\n", i,
                   (pa[i].p[0][0] >> 1) & 0x3F, (pb[i].p[0][0] >> 1) & 0x3F);
            refused++;
            continue;
        }
        fwrite(au, 1, got, o);
        total += got;
        pics++;
    }
    fclose(o);
    printf("wrote %s: %d composite pictures (%d refused), %zu bytes, %d slices each\n",
           argv[3], pics, refused, total, pics ? pa[0].n * 2 : 0);
    if (pics == 0) { printf("FAIL: nothing composed\n"); return 1; }
    printf("PASS(built): decode with ffmpeg to judge -- expect %ux%u.\n"
           "  LOOK AT THE SEAM under motion; addressing being legal does not mean inter\n"
           "  prediction stayed inside its own pane.\n",
           comp.w_ctbs * 32, comp.h_ctbs * 32);
    return 0;
}
