/* OFFLINE PROBE: can an NVENC slice and an AMF slice share ONE picture SIDE BY SIDE?
 *
 * The horizontal composite stacks one full-width slice per host. A left/right split cannot be
 * done that way -- an H.264 slice is a contiguous run of macroblocks in RASTER order, so it can
 * only ever be a horizontal band. The way round it is to make each host encode a HALF-WIDTH
 * pane with ONE SLICE PER MACROBLOCK ROW, and then interleave: left row 0, right row 0, left
 * row 1, right row 1 ... each slice re-addressed to its place in the wider picture.
 *
 * This driver builds exactly that, using the SHIPPING rewriter (coop_avc_rewrite_slice), so a
 * pass here is evidence about the code that would run rather than about a model of it.
 *
 * Usage: avc_sbs_cli <left.264> <right.264> <composite-params.264> <out.264> [frames]
 *   left/right          half-width panes, one slice per MB row
 *   composite-params    any stream encoded at the COMPOSITE width; its SPS/PPS are used
 */
#include "coop_avc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXSLICE 512
#define MAXAU    600

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

/* Walk Annex-B NALs. Returns the offset just past this NAL, 0 when done. */
static size_t nal_next(const unsigned char *d, size_t len, size_t from, size_t *off, size_t *nl) {
    size_t i = from;
    while (i + 3 <= len) {
        size_t sc = 0;
        if (!d[i] && !d[i + 1] && d[i + 2] == 1) { sc = 3; }
        else if (i + 4 <= len && !d[i] && !d[i + 1] && !d[i + 2] && d[i + 3] == 1) { sc = 4; }
        if (!sc) { i++; continue; }
        size_t s = i + sc, e = s;
        while (e + 3 <= len) {
            if (!d[e] && !d[e + 1] && d[e + 2] == 1) { break; }
            if (e + 4 <= len && !d[e] && !d[e + 1] && !d[e + 2] && d[e + 3] == 1) { break; }
            e++;
        }
        if (e + 3 > len) { e = len; }
        size_t L = e - s;
        while (L > 0 && d[s + L - 1] == 0) { L--; }
        *off = s;
        *nl = L;
        return e;
    }
    return 0;
}

typedef struct {
    const unsigned char *nal[MAXSLICE];
    size_t len[MAXSLICE];
    int n;
} au_t;

/* Split a stream into access units of VCL slices, and hand back the first SPS/PPS seen. */
static int load(const char *path, au_t *aus, int maxau,
                const unsigned char **sps, size_t *sps_len,
                const unsigned char **pps, size_t *pps_len, unsigned char **owned) {
    size_t n = 0;
    unsigned char *d = slurp(path, &n);
    if (d == NULL) { fprintf(stderr, "cannot read %s\n", path); return -1; }
    *owned = d;
    int au = -1;
    size_t i = 0, off, nl;
    while ((i = nal_next(d, n, i, &off, &nl)) != 0) {
        if (nl == 0) { continue; }
        const int t = d[off] & 0x1F;
        if (t == 7 && *sps == NULL) { *sps = d + off; *sps_len = nl; }
        if (t == 8 && *pps == NULL) { *pps = d + off; *pps_len = nl; }
        if (t != 1 && t != 5) { continue; }
        /* first_mb_in_slice == 0 opens a picture; its ue(v) is a leading 1 bit. */
        const bool first = (d[off + 1] & 0x80) != 0;
        if (first) {
            if (au + 1 >= maxau) { break; }
            au++;
            aus[au].n = 0;
        }
        if (au < 0) { continue; }
        if (aus[au].n < MAXSLICE) {
            aus[au].nal[aus[au].n] = d + off;
            aus[au].len[aus[au].n] = nl;
            aus[au].n++;
        }
    }
    return au + 1;
}

static void emit(FILE *f, const unsigned char *nal, size_t len) {
    static const unsigned char sc[4] = {0, 0, 0, 1};
    fwrite(sc, 1, 4, f);
    fwrite(nal, 1, len, f);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <left.264> <right.264> <composite-params.264> <out.264> [frames]\n",
                argv[0]);
        return 2;
    }
    const int want = argc > 5 ? atoi(argv[5]) : 0;

    static au_t la[MAXAU], ra[MAXAU], ca[MAXAU];
    const unsigned char *lsps = NULL, *lpps = NULL, *rsps = NULL, *rpps = NULL, *csps = NULL, *cpps = NULL;
    size_t lsps_n = 0, lpps_n = 0, rsps_n = 0, rpps_n = 0, csps_n = 0, cpps_n = 0;
    unsigned char *lown = NULL, *rown = NULL, *cown = NULL;

    const int ln = load(argv[1], la, MAXAU, &lsps, &lsps_n, &lpps, &lpps_n, &lown);
    const int rn = load(argv[2], ra, MAXAU, &rsps, &rsps_n, &rpps, &rpps_n, &rown);
    const int cn = load(argv[3], ca, MAXAU, &csps, &csps_n, &cpps, &cpps_n, &cown);
    if (ln <= 0 || rn <= 0 || cn <= 0 || !lsps || !rsps || !csps || !lpps || !rpps || !cpps) {
        fprintf(stderr, "load failed (l=%d r=%d c=%d, sps %p/%p/%p)\n", ln, rn, cn,
                (void *) lsps, (void *) rsps, (void *) csps);
        return 1;
    }

    coop_avc_sps_t LS, RS, CS;
    coop_avc_pps_t LP, RP, CP;
    if (!coop_avc_sps_parse(lsps, lsps_n, &LS) || !coop_avc_sps_parse(rsps, rsps_n, &RS) ||
        !coop_avc_sps_parse(csps, csps_n, &CS) || !coop_avc_pps_parse(lpps, lpps_n, &LP) ||
        !coop_avc_pps_parse(rpps, rpps_n, &RP) || !coop_avc_pps_parse(cpps, cpps_n, &CP)) {
        fprintf(stderr, "parameter-set parse failed\n");
        return 1;
    }
    printf("left  %ux%u mbs, right %ux%u mbs, composite %ux%u mbs\n",
           LS.width_mbs, LS.height_mbs, RS.width_mbs, RS.height_mbs, CS.width_mbs, CS.height_mbs);
    if (LS.width_mbs + RS.width_mbs != CS.width_mbs) {
        fprintf(stderr, "GEOMETRY MISMATCH: %u + %u != %u mbs wide\n",
                LS.width_mbs, RS.width_mbs, CS.width_mbs);
        return 1;
    }
    printf("compatible(left,right) = %d\n", (int) coop_avc_compatible(&LS, &LP, &RS, &RP));
    printf("  left : poc_type=%u refs=%u cabac=%d wpred=%d wbipred=%u deblock_present=%d\n",
           LS.poc_type, LS.max_num_ref_frames, (int) LP.entropy_cabac, (int) LP.weighted_pred,
           LP.weighted_bipred, (int) LP.deblock_present);
    printf("  right: poc_type=%u refs=%u cabac=%d wpred=%d wbipred=%u deblock_present=%d\n",
           RS.poc_type, RS.max_num_ref_frames, (int) RP.entropy_cabac, (int) RP.weighted_pred,
           RP.weighted_bipred, (int) RP.deblock_present);

    FILE *out = fopen(argv[4], "wb");
    if (out == NULL) { fprintf(stderr, "cannot write %s\n", argv[4]); return 1; }

    const int frames = want > 0 && want < (ln < rn ? ln : rn) ? want : (ln < rn ? ln : rn);
    unsigned char buf[1 << 20];
    int rewritten = 0, refused = 0, emitted_au = 0;

    for (int f = 0; f < frames; ++f) {
        coop_avc_slice_info_t li;
        if (!coop_avc_slice_info(la[f].nal[0], la[f].len[0], &LS, &li)) {
            fprintf(stderr, "AU %d: cannot read left slice info\n", f);
            break;
        }
        /* Parameter sets ride with every IDR, exactly as the app emits them. */
        if (li.idr) {
            emit(out, csps, csps_n);
            emit(out, cpps, cpps_n);
        }
        const int rows = (int) (LS.height_mbs < RS.height_mbs ? LS.height_mbs : RS.height_mbs);
        for (int r = 0; r < rows; ++r) {
            if (r >= la[f].n || r >= ra[f].n) { break; }
            /* LEFT pane: row r of a W_l-wide pane becomes row r, column 0 of the composite. */
            coop_avc_rehome_t rl = {
                    .first_mb = (unsigned) r * CS.width_mbs,
                    .frame_num = li.frame_num, .nal_ref_idc = li.nal_ref_idc,
                    .slice_type_form = (int) li.slice_type, .force_nonidr = false,
                    .src_sps = &LS, .src_pps = &LP, .dst_sps = &CS, .dst_pps = &CP,
            };
            size_t got = coop_avc_rewrite_slice(la[f].nal[r], la[f].len[r], buf, sizeof(buf), &rl);
            if (got == 0) { refused++; } else { emit(out, buf, got); rewritten++; }

            /* RIGHT pane: same row, offset by the left pane's width in macroblocks. */
            coop_avc_rehome_t rr = rl;
            rr.first_mb = (unsigned) r * CS.width_mbs + LS.width_mbs;
            rr.src_sps = &RS;
            rr.src_pps = &RP;
            /* AN ALL-INTRA PEER EMITS IDR ON EVERY PICTURE while the primary emits P, and
             * 7.4.1.2.4 forbids one coded picture mixing IDR and non-IDR NALs. Converting the
             * peer's NAL to non-IDR while keeping its I slice_type is legal and is what the
             * shipping composer does -- see coop_avc_rehome_t::force_nonidr. All-intra is the
             * peer's normal configuration here, because AMF has no constrained-encoding
             * property, so this is the ordinary path rather than an edge case. */
            rr.force_nonidr = ((ra[f].nal[r][0] & 0x1F) == 5) && !li.idr;
            got = coop_avc_rewrite_slice(ra[f].nal[r], ra[f].len[r], buf, sizeof(buf), &rr);
            if (got == 0) { refused++; } else { emit(out, buf, got); rewritten++; }
        }
        emitted_au++;
    }
    fclose(out);
    printf("AUs %d, slices rewritten %d, refused %d\n", emitted_au, rewritten, refused);
    for (int i = 0; i < 16; ++i) {
        const unsigned c = coop_avc_cfail_get(i);
        if (c) { printf("  cfail[%d] = %u\n", i, c); }
    }
    free(lown); free(rown); free(cown);
    return refused ? 1 : 0;
}
