/*
 * Offline proof that the synthesised all-skip slice is DECODABLE and EXACT.
 *
 * Reads a real composed co-op stream (the /tmp/coop-dump-out capture from the TV),
 * and rewrites every Nth picture so its bottom slice is a synthesised skip slice
 * instead of the peer's real one. The output is meant to be run through a reference
 * decoder:
 *
 *   ./test_conceal_offline in.h265 out.h265 3
 *   ffmpeg -i out.h265 -f null -          # must report NO errors
 *
 * and, because a skip slice must reproduce the previous picture's bottom half exactly,
 * decoding out.h265 and in.h265 to frames and comparing the BOTTOM half of a concealed
 * picture against the PREVIOUS picture's bottom half must be lossless.
 *
 * Testing this offline rather than on the TV matters: a malformed slice on the TV shows
 * up as "the picture looks wrong", which is the same symptom as five other faults, and
 * costs a build-deploy-launch cycle to see. ffmpeg names the exact syntax element.
 */
#include "../../src/app/stream/video/coop_conceal.h"
#include "../../src/app/stream/video/coop_splice.h"
#include "../../src/app/stream/video/coop_poc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) { return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t) n);
    if (b == NULL || fread(b, 1, (size_t) n, f) != (size_t) n) { fclose(f); return NULL; }
    fclose(f);
    *len = (size_t) n;
    return b;
}

/* Split an annex-B buffer into access units at each parameter-set prefix. The composed
 * stream repeats VPS/SPS/PPS before every picture, so a VPS marks an AU boundary. */
static size_t next_au(const unsigned char *d, size_t len, size_t from) {
    for (size_t i = from + 4; i + 5 < len; ++i) {
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 0 && d[i+3] == 1) {
            int t = (d[i+4] >> 1) & 0x3F;
            if (t == 32) { return i; }      /* VPS starts the next AU */
        }
    }
    return len;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s in.h265 out.h265 EVERY_N [START COUNT]\n"
                        "  EVERY_N   conceal every Nth picture (0 = use START/COUNT)\n"
                        "  START     first picture to conceal (1-based)\n"
                        "  COUNT     how many CONSECUTIVE pictures to conceal\n"
                        "            -- consecutive is the real case: the peer stalls for a\n"
                        "            run of pictures, not for one in isolation.\n", argv[0]);
        return 2;
    }
    const int every = atoi(argv[3]);
    const int start = (argc > 4) ? atoi(argv[4]) : 0;
    const int count = (argc > 5) ? atoi(argv[5]) : 0;
    size_t len = 0;
    unsigned char *d = slurp(argv[1], &len);
    if (d == NULL) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

    FILE *out = fopen(argv[2], "wb");
    if (out == NULL) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }

    coop_poc_params_t params;
    memset(&params, 0, sizeof(params));
    static unsigned char scratch[1u << 20], slice[1u << 21];

    size_t pos = 0;
    int pics = 0, concealed = 0, refused = 0;
    while (pos < len) {
        size_t end = next_au(d, len, pos);
        size_t au_len = end - pos;
        coop_au_t au;
        if (!coop_au_parse(d + pos, au_len, &au) || au.slice_count < 1) {
            fwrite(d + pos, 1, au_len, out);
            pos = end;
            continue;
        }
        if (!params.valid && au.non_vcl != NULL) {
            coop_poc_params_parse(au.non_vcl, au.non_vcl_len, &params);
            if (params.valid) {
                fprintf(stderr, "params: %ux%u CTBs addr_bits=%u log2diff=%u sao=%d "
                                "init_qp_minus26=%d\n",
                        params.w_ctbs, params.h_ctbs, params.slice_addr_bits,
                        params.log2_diff_max_min_cb, (int) params.sao_enabled,
                        params.init_qp_minus26);
                fprintf(stderr, "sps_temporal_mvp_enabled=%d num_st_rps=%u chroma=%u\n",
                        (int) params.sps_temporal_mvp_enabled,
                        params.num_short_term_ref_pic_sets, params.chroma_format_idc);
            }
        }
        pics++;
        /* Replace the SECOND slice (the peer's, at the bottom-half address) with a
         * synthesised skip slice, using OUR slice for the header. */
        bool did = false;
        const bool want = (every > 0) ? ((pics % every) == 0)
                                      : (start > 0 && pics >= start && pics < start + count);
        if (params.valid && want && au.slice[1] != NULL) {
            unsigned addr = coop_bottom_half_addr(&params);
            /* COOP_CONCEAL_MODE=dup ISOLATES THE HEADER REWRITE FROM THE PAYLOAD SYNTHESIS.
             * It re-addresses OUR slice to the bottom half and keeps its own entropy data,
             * so the bottom pane must decode as a copy of the top pane's content. If even
             * that comes out wrong, the fault is in coop_slice_set_address_payload()'s
             * header rewriting rather than in the CABAC the concealer writes -- and the two
             * are otherwise indistinguishable from the picture. */
            const char *cmode = getenv("COOP_CONCEAL_MODE");
            const int dup = cmode != NULL && strcmp(cmode, "dup") == 0;
            /* COOP_CONCEAL_QP_DELTA sweeps the QP the CABAC contexts are initialised from.
             * That value is the one input the round-trip test CANNOT validate: it uses the
             * same number on both sides, so a wrong SliceQpY is self-consistent there and
             * only a real decoder disagrees. If some offset makes the concealed picture
             * bit-exact, the derivation in coop_slice_info() is off by exactly that much. */
            const char *qds = getenv("COOP_CONCEAL_QP_DELTA");
            size_t n;
            if (dup) {
                n = coop_slice_set_address(&params, au.slice[0], au.slice_len[0], addr,
                                           slice, sizeof(slice));
            } else if (qds != NULL) {
                coop_slice_info_t qsi;
                n = 0;
                if (coop_slice_info(&params, au.slice[0], au.slice_len[0], &qsi)) {
                    qsi.slice_qp += atoi(qds);
                    size_t pn = coop_build_skip_slice_data(&params, &qsi, addr,
                                                           scratch, sizeof(scratch));
                    if (pn > 0) {
                        n = coop_slice_set_address_payload(&params, au.slice[0],
                                                           au.slice_len[0], addr, scratch,
                                                           pn, slice, sizeof(slice));
                    }
                }
            } else {
                n = coop_build_skip_slice(&params, au.slice[0], au.slice_len[0], addr,
                                          scratch, sizeof(scratch), slice, sizeof(slice));
            }
            if (n > 0) {
                static const unsigned char sc[4] = {0, 0, 0, 1};
                /* SPLICE OUT ONLY THE PEER'S SLICE, KEEPING EVERYTHING ELSE.
                 *
                 * The first version wrote [AU start .. end of our slice] + the synthetic
                 * slice and stopped -- silently DISCARDING every NAL after the peer's.
                 * That matters because not every access unit in the dump carries a VPS:
                 * the fallback path emits our own AU with no parameter sets, so next_au()
                 * merges it into the previous one and the merged AU holds a third slice.
                 * Dropping it deleted a whole picture and the decoder cascaded -- which
                 * looked exactly like "the synthesised slice is malformed" and sent this
                 * investigation after the encoder for an hour. One picture in 385 tripped
                 * it (picture 170), so most test positions passed. */
                const unsigned char *ps = au.slice[1];
                size_t sc_len = (ps - (d + pos) >= 4 && ps[-4] == 0 && ps[-3] == 0 &&
                                 ps[-2] == 0 && ps[-1] == 1) ? 4u : 3u;
                size_t before = (size_t) (ps - sc_len - (d + pos));
                size_t after_off = (size_t) (ps + au.slice_len[1] - (d + pos));
                fwrite(d + pos, 1, before, out);
                fwrite(sc, 1, 4, out);
                fwrite(slice, 1, n, out);
                if (after_off < au_len) {
                    fwrite(d + pos + after_off, 1, au_len - after_off, out);
                }
                did = true;
                concealed++;
                {
                    coop_slice_info_t si;
                    coop_slice_info(&params, au.slice[0], au.slice_len[0], &si);
                    /* REPORT THE POC, NOT ONLY THIS HARNESS'S AU COUNTER. The two do not
                     * correspond: `pics` counts access units as next_au() splits them,
                     * while a decoder emits PICTURES -- and a merged AU or a
                     * parameter-set-only one puts the two permanently out of step. Selecting
                     * the concealed frame by AU index made a dup-mode run appear to diverge
                     * at frame 62 where the skip run diverged at 216, which invalidated a
                     * whole round of comparisons. With no reordering in this stream (no
                     * B-frames) output order IS POC order, so the POC printed here is the
                     * frame index to hand ffmpeg. */
                    unsigned poc = 0;
                    const bool got_poc =
                            coop_poc_read(&params, au.slice[0], au.slice_len[0], &poc);
                    fprintf(stderr, "pic %d concealed: POC=%s%u NAL=%zu type=%u qp=%d "
                                    "merge=%u sao=%u/%u cabac_init=%u ourNAL=%d ourlen=%zu\n",
                            pics, got_poc ? "" : "?", poc, n, si.slice_type, si.slice_qp,
                            si.max_num_merge_cand, si.sao_luma, si.sao_chroma,
                            si.cabac_init_flag, (au.slice[0][0] >> 1) & 0x3F,
                            au.slice_len[0]);
                }
            } else {
                refused++;
            }
        }
        if (!did) { fwrite(d + pos, 1, au_len, out); }
        pos = end;
    }
    fclose(out);
    fprintf(stderr, "pictures=%d concealed=%d refused=%d\n", pics, concealed, refused);
    return concealed > 0 ? 0 : 3;
}
