/*
 * coop_slice_idr_to_trail_sps(): a peer IDR converted to TRAIL_R that POINTS AT the SPS's reference set
 * (short_term_ref_pic_set_sps_flag = 1), as the NVENC slice beside it in a co-op picture does.
 *
 * Why it exists: the keepref conversion spells out the same one-entry set instead. HEVC wants that flag
 * equal in every slice of a picture, and the SHIELD TV's decoder mis-decoded the converted slice (the
 * pane went green) until only that was changed -- measured 2026-10-21 by replaying a co-op startup.
 *
 * What a wrong header looks like is a wrong LENGTH, not a wrong value: every field after it shifts. So
 * each converted slice is parsed back with the walker the splicer itself uses, and the slice data after
 * the header must come through byte for byte. A synthetic slice is used rather than a capture: the
 * header fields are what is under test, and captures of real desktops do not belong in this repository.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "coop_poc.h"

static int fails;
static void ck(const char *what, int ok) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { fails++; }
}

/* ---- a minimal bit writer for the synthetic slice ---- */
static unsigned char bits_buf[64];
static size_t bit_at;
static void put(unsigned v, unsigned n) {
    for (unsigned i = 0; i < n; ++i, ++bit_at) {
        if ((v >> (n - 1 - i)) & 1u) {
            bits_buf[bit_at >> 3] |= (unsigned char) (0x80u >> (bit_at & 7u));
        }
    }
}
static void put_ue(unsigned v) {
    unsigned code = v + 1, n = 0;
    while ((code >> n) != 0) { n++; }
    put(0, n - 1);
    put(code, n);
}
static void put_se(int v) { put_ue(v > 0 ? (unsigned) (2 * v - 1) : (unsigned) (-2 * v)); }

/* The stream settings a co-op pane has in practice (NVENC HEVC, 3840x1088 -> 120x34 CTBs, but the
 * address width is the COMPOSITE's 13 bits as on the SHIELD): one short-term set in the SPS = {POC-1,
 * used}, 8-bit POC LSB, SAO on, 4:2:0, no tiles in the pane, nothing else optional. */
static void params(coop_poc_params_t *p, unsigned sets) {
    memset(p, 0, sizeof(*p));
    p->valid = true;
    p->log2_max_poc_lsb = 8;
    p->slice_addr_bits = 13;
    p->num_short_term_ref_pic_sets = sets;
    p->sps_rps0_first_neg_delta = 1;
    p->sps_rps0_first_neg_used = 1;
    p->sao_enabled = true;
    p->chroma_format_idc = 1;
    p->num_ref_idx_l0_default = 1;
    p->num_ref_idx_l1_default = 1;
}

static const unsigned char DATA[] = {0x5a, 0xa5, 0xc3, 0x3c, 0x81, 0x7e, 0x99};

/* IDR_W_RADL I-slice, NOT the first slice of its picture (address 4080: the bottom pane), QP delta +3. */
static size_t idr_slice(unsigned char *out) {
    memset(bits_buf, 0, sizeof(bits_buf));
    bit_at = 0;
    put(19u << 1, 8);              /* forbidden_zero_bit, nal_unit_type = 19, nuh_layer_id high bit */
    put(1, 8);                     /* nuh_layer_id low bits, nuh_temporal_id_plus1 = 1 */
    put(0, 1);                     /* first_slice_segment_in_pic_flag */
    put(0, 1);                     /* no_output_of_prior_pics_flag (IRAP) */
    put_ue(0);                     /* slice_pic_parameter_set_id */
    put(4080, 13);                 /* slice_segment_address */
    put_ue(2);                     /* slice_type = I */
    put(1, 1);                     /* slice_sao_luma_flag */
    put(1, 1);                     /* slice_sao_chroma_flag */
    put_se(3);                     /* slice_qp_delta */
    put(1, 1);                     /* byte_alignment(): one ... */
    while (bit_at & 7u) { put(0, 1); }   /* ... then zeros */
    size_t n = bit_at / 8;
    memcpy(out, bits_buf, n);
    memcpy(out + n, DATA, sizeof(DATA));
    return n + sizeof(DATA);
}

int main(void) {
    coop_poc_params_t p;
    params(&p, 1);
    unsigned char idr[64];
    const size_t idr_len = idr_slice(idr);

    static unsigned char keep[256], sps[256];
    const size_t nk = coop_slice_idr_to_trail_keepref(&p, idr, idr_len, 9, keep, sizeof(keep));
    const size_t ns = coop_slice_idr_to_trail_sps(&p, idr, idr_len, 9, sps, sizeof(sps));

    printf("1. both conversions accept the slice and give TRAIL_R\n");
    ck("keepref converted", nk > 0);
    ck("sps converted", ns > 0);
    ck("sps result is nal_unit_type 1", ns > 0 && ((sps[0] >> 1) & 0x3f) == 1);

    printf("2. parsed back with the splicer's own walker\n");
    coop_slice_info_t ik, is;
    memset(&ik, 0, sizeof(ik));
    memset(&is, 0, sizeof(is));
    ck("keepref header walks", nk > 0 && coop_slice_info(&p, keep, nk, &ik));
    ck("sps header walks", ns > 0 && coop_slice_info(&p, sps, ns, &is));
    ck("keepref spells its set out", ik.rps_from_sps == 0);
    ck("sps points at the SPS set", is.rps_from_sps == 1);
    ck("both reference POC-1, used, resolved", ik.first_neg_delta == 1 && ik.first_neg_used == 1 &&
       is.first_neg_delta == 1 && is.first_neg_used == 1 && ik.rps_unresolved == 0 && is.rps_unresolved == 0);
    ck("slice type, QP and SAO come through", is.slice_type == 2 && is.slice_qp == 29 &&
       is.sao_luma == 1 && is.sao_chroma == 1 && ik.slice_qp == 29);
    unsigned poc = 0;
    ck("POC 9 reads back", coop_poc_read(&p, sps, ns, &poc) && poc == 9);
    bool first = true;
    unsigned addr = 0;
    ck("still the bottom pane's slice", coop_slice_address(&p, sps, ns, &first, &addr) && !first &&
       addr == 4080);

    printf("3. only the RPS statement changed\n");
    const size_t ek = coop_slice_header_end_bit_public(&p, keep, nk);
    const size_t es = coop_slice_header_end_bit_public(&p, sps, ns);
    /* keepref: flag 0, inter_ref_pic_set_prediction_flag 0, '010' '1' '1', used 1 = 8 bits; sps: flag 1. */
    ck("the header is 7 bits shorter", ek > 0 && es > 0 && ek - es == 7);
    ck("the slice data is unchanged", ns > sizeof(DATA) && memcmp(sps + ns - sizeof(DATA), DATA, sizeof(DATA)) == 0 &&
       memcmp(keep + nk - sizeof(DATA), DATA, sizeof(DATA)) == 0);

    printf("4. an SPS with two sets is refused (the index would need a choice)\n");
    params(&p, 2);
    ck("refused", coop_slice_idr_to_trail_sps(&p, idr, idr_len, 9, sps, sizeof(sps)) == 0);

    printf(fails == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
