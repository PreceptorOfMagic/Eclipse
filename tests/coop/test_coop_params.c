/* Verify the EXTENDED SPS/PPS parse against a real stream.
 *
 * The full slice-header walk is gated on ~15 flags spread across the SPS and PPS. A
 * single wrong one does not fail loudly -- it silently mis-locates every field after
 * it, and the first symptom is a decoder complaining about slice DATA (which is how the
 * byte-alignment bug presented). So this checks the parsed values are in their legal
 * ranges: implausible values mean the walk went off the rails, even when nothing
 * crashed.
 *
 *   gcc -O1 -o /tmp/t_params tests/coop/test_coop_params.c \
 *       src/app/stream/video/coop_poc.c -Isrc/app/stream/video
 *   /tmp/t_params tests/coop/skynet-4070-reference.h265
 */
#include <stdio.h>
#include "coop_poc.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <annexb.h265>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("cannot open %s\n", argv[1]);
        return 1;
    }
    static unsigned char buf[8 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    coop_poc_params_t p;
    if (!coop_poc_params_parse(buf, n, &p) || !p.valid) {
        printf("FAIL: parameter sets did not parse\n");
        return 1;
    }

    printf("geometry : %ux%u CTBs  addr_bits=%u  log2_max_poc_lsb=%u\n",
           p.w_ctbs, p.h_ctbs, p.slice_addr_bits, p.log2_max_poc_lsb);
    printf("PPS      : cabac_init=%d chroma_qp_off=%d wp=%d wbp=%d tiles=%d wpp=%d\n",
           p.cabac_init_present, p.slice_chroma_qp_offsets_present, p.weighted_pred,
           p.weighted_bipred, p.tiles_enabled, p.entropy_coding_sync_enabled);
    printf("         : lf_across=%d dbf_ctrl=%d dbf_override=%d dbf_disabled=%d\n",
           p.loop_filter_across_slices, p.deblocking_filter_control_present,
           p.deblocking_filter_override_enabled, p.pps_deblocking_filter_disabled);
    printf("         : lists_mod=%d hdr_ext=%d ref_idx_defaults=%u/%u extra_hdr_bits=%u\n",
           p.lists_modification_present, p.header_extension_present,
           p.num_ref_idx_l0_default, p.num_ref_idx_l1_default,
           p.num_extra_slice_header_bits);
    printf("SPS      : temporal_mvp=%d sao=%d lt_ref=%d num_st_rps=%u chroma_idc=%u\n",
           p.sps_temporal_mvp_enabled, p.sao_enabled, p.long_term_ref_pics_present,
           p.num_short_term_ref_pic_sets, p.chroma_format_idc);

    /* Range checks. These are the values that go implausible FIRST when the walk
     * desynchronises, which is why they are worth asserting rather than printing. */
    int bad = 0;
    if (p.chroma_format_idc > 3) {
        printf("BAD: chroma_format_idc=%u (legal 0-3)\n", p.chroma_format_idc);
        bad = 1;
    }
    if (p.num_short_term_ref_pic_sets > 64) {
        printf("BAD: num_short_term_ref_pic_sets=%u (legal 0-64)\n", p.num_short_term_ref_pic_sets);
        bad = 1;
    }
    if (p.num_ref_idx_l0_default > 16 || p.num_ref_idx_l1_default > 16) {
        printf("BAD: ref_idx defaults %u/%u (legal 1-16)\n",
               p.num_ref_idx_l0_default, p.num_ref_idx_l1_default);
        bad = 1;
    }
    if (p.log2_max_poc_lsb < 4 || p.log2_max_poc_lsb > 16) {
        printf("BAD: log2_max_poc_lsb=%u (legal 4-16)\n", p.log2_max_poc_lsb);
        bad = 1;
    }
    if (p.num_extra_slice_header_bits > 7) {
        printf("BAD: num_extra_slice_header_bits=%u\n", p.num_extra_slice_header_bits);
        bad = 1;
    }
    if (p.w_ctbs == 0 || p.h_ctbs == 0 || p.slice_addr_bits == 0) {
        printf("BAD: degenerate geometry\n");
        bad = 1;
    }

    printf(bad ? "\nFAIL: implausible values -- the field walk is misaligned\n"
               : "\nPASS: every parsed value is in range\n");
    return bad;
}
