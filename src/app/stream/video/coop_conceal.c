/*
 * Synthesised all-skip concealment slice for the co-op composite.
 *
 * WHY THIS EXISTS
 *   The composite is paced by OUR host, and the peer's picture rate never matches it
 *   exactly -- two encoders on two machines, free-running. When a picture has no peer
 *   slice the bottom half has to come from somewhere:
 *     - leaving it UNCODED gives an incomplete picture. Measured: 43% of pictures, and
 *       the bottom pane was unwatchable mush that poisoned every later reference.
 *     - REPEATING the peer's last slice gives a whole picture, but replays that slice's
 *       residual and motion against a different reference, so error accumulates and the
 *       pane smears. Better, still visibly wrong.
 *     - an ALL-SKIP slice is the correct answer, and this file builds one. Every CU is
 *       coded skip with merge candidate 0, which resolves to MV(0,0) against
 *       RefPicList0[0] -- the previous picture -- so the bottom half is reproduced
 *       PIXEL-EXACTLY with no residual at all.
 *
 *   Pixel-exact matters for more than looks: the peer's NEXT real slice predicts from
 *   its own previous frame, and after an exact copy the composite's bottom half IS that
 *   frame. The peer's prediction chain stays intact through the gap instead of drifting.
 *
 * THE HEADER IS OURS, NOT A NEW ONE. HEVC requires every slice of a picture to agree on
 * slice_pic_order_cnt_lsb, the short-term reference picture set and
 * slice_temporal_mvp_enabled_flag. Building a header from scratch would have to
 * reproduce all of that; taking our own slice's header and only re-addressing it makes
 * agreement structural. That is coop_slice_set_address_payload().
 *
 * ONE APPROXIMATION, DELIBERATE AND BOUNDED. The merge candidate for the FIRST CU of the
 * slice has no spatial neighbours (they are in the other slice), so if temporal MVP is
 * enabled it takes the collocated block's motion vector rather than zero. That affects a
 * single 32x32 CU at the top-left of the bottom pane, immediately under the seam. Every
 * other CU has a skip neighbour to its left or above, whose MV is (0,0), so candidate 0
 * is (0,0) for all of them. (Measured on the live stream: sps_temporal_mvp_enabled=0, so
 * this does not currently arise at all.)
 *
 * ============================ VERIFICATION STATUS ============================
 * Measured offline against a real captured composite (tests/coop/test_conceal_offline.c
 * on a 385-picture /tmp/coop-dump-out capture from the TV).
 *
 * DECODES CORRECTLY -- zero decoder errors in every configuration tried, including
 * concealing EVERY non-IRAP picture in the stream (384 of 385). Earlier notes here said
 * the slice was rejected as undecodable; that was WRONG and the fault was in the test
 * harness, which discarded any NAL following the peer's slice. Not every access unit in
 * the dump carries a VPS (the fallback path emits our own AU with no parameter sets), so
 * the AU splitter merged two pictures and the harness then deleted the second one. Exactly
 * one picture in 385 tripped it, so most test positions passed and the failure looked
 * data-dependent. Cost about an hour of chasing the encoder.
 *
 * NOT A BIT-EXACT FREEZE. With every picture concealed, consecutive pictures still differ,
 * but the delta DECAYS monotonically (10-bit luma mean 9.20, 8.22, 7.35, 6.65, 6.06, 5.54,
 * 5.11, 4.78, 4.46, 4.20, 3.96, 3.73 over twelve frames). It converges instead of
 * diverging, which is the opposite of the repeat-the-last-slice concealment it replaces --
 * that one decayed all the way to a black pane. The residual is consistent with the
 * encoder using several reference pictures, so RefPicList0[0] -- what a zero-MV skip copies
 * -- is not always the IMMEDIATELY preceding picture. Confirmed by a periodic signature:
 * concealing a run of ten, eight of them matched their predecessor to mean <0.17 while
 * every fourth jumped to mean 5.8, i.e. every fourth picture's list-0 head points further
 * back. Making it exact needs the concealment slice to name POC-1 explicitly, which means
 * overriding num_ref_idx_l0_active in the slice header rather than copying ours.
 *
 * SEEN ON THE TV 2026-08-01, AND IT FAILED. Sentinel on, live two-host: skipconceal=626,
 * repeated=0, peergap=1 -- and the bottom pane carried a band of blocky colour noise across
 * its first CTB rows. Reproduced OFFLINE from a real 384-picture capture, so the TV is not
 * the variable: conceal every picture and ffmpeg's own decode shows the same collapse into
 * green blocks, WHILE REPORTING ZERO ERRORS. The "decodes correctly" claim above was only
 * ever an error count, and an error count is not a decode ([[verify-visually-each-iteration]]).
 *
 * WHAT THE MEASUREMENT SAYS. Concealing every picture of that capture, PSNR of each
 * concealed picture against its predecessor: 49.6, 48.4, 48.6, *7.8*, 42.6, 38.5, 26.7,
 * 29.1, 32.7 dB. NOT ONE is bit-exact, and every fourth collapses -- the same periodic
 * signature recorded above, now with the cause identified rather than guessed at:
 *
 * THE REFERENCE-COUNT THEORY WAS TESTED AND IS WRONG. It said: the client sends
 * x-nv-video[0].maxNumReferenceFrames = 0 ("host picks") whenever the decoder claims RFI,
 * Apollo's set_ref_frames() defaults to 5, so RefPicList0[0] is periodically an older
 * picture and the "copy" reproduces a frame from several back. Dropping the RFI capability
 * made the client ask for exactly 1 -- confirmed on the wire -- and THE COMPOSITE CAME BACK
 * BYTE-IDENTICAL (same md5 over 17MB, same pairing counters). Apollo already pins
 * numRefL0 = NV_ENC_NUM_REF_FRAMES_1; the DPB size is only a fallback pool. Reverted.
 *
 * THE ISOLATED TEST, AND ITS TARGET NUMBER. Conceal exactly ONE picture of a real capture
 * and compare its BOTTOM HALF against the PREVIOUS picture's bottom half. A correct
 * all-skip slice makes those identical. Measured on a capture whose bottom pane is static
 * -- control frame n vs n-1 is 62.6 dB, i.e. nothing is moving:
 *
 *      concealed[n] bottom vs control[n-1] bottom  =  22.5 dB      <-- must be inf
 *
 * That is a construction fault in the slice, not a prediction or reference problem: on a
 * pane with no motion an exact copy is trivially available and we are not producing it.
 * Everything else -- header, RPS, temporal MVP, entry points -- has been ruled out: the
 * composite's PPS has tiles=0, WPP=0, dependent slices=0, sps_temporal_mvp=0,
 * log2_diff_max_min_cb=1, so split_cu_flag is coded at depth 0 and every CTB is whole.
 *
 * Run it with tests/coop/test_conceal_offline.c (conceal every Nth, N large enough to hit
 * one picture), find the frame where the streams diverge, and drive 22.5 dB to inf. No TV
 * and no PC2 needed.
 *
 * SO: /tmp/coop-skipconceal STAYS DEFAULT OFF, and the repeat path stays the default,
 * decay and all. Do not re-enable on the strength of the offline numbers alone -- that is
 * exactly the mistake this block now records.
 * ============================================================================
 */
#include "coop_conceal.h"

#include <string.h>

/* ---- CABAC arithmetic encoder (H.265 9.3.4; identical engine to H.264) -------------- */

/* Exposed under a coop_test_ alias so tests/coop/test_conceal_roundtrip.c can decode
 * with the SAME spec tables while implementing the engine independently. */
const unsigned char coop_test_range_tab_lps[64][4] = {
    {128,176,208,240},{128,167,197,227},{128,158,187,216},{123,150,178,205},
    {116,142,169,195},{111,135,160,185},{105,128,152,175},{100,122,144,166},
    { 95,116,137,158},{ 90,110,130,150},{ 85,104,123,142},{ 81, 99,117,135},
    { 77, 94,111,128},{ 73, 89,105,122},{ 69, 85,100,116},{ 66, 80, 95,110},
    { 62, 76, 90,104},{ 59, 72, 86, 99},{ 56, 69, 81, 94},{ 53, 65, 77, 89},
    { 51, 62, 73, 85},{ 48, 59, 69, 80},{ 46, 56, 66, 76},{ 43, 53, 63, 72},
    { 41, 50, 59, 69},{ 39, 48, 56, 65},{ 37, 45, 54, 62},{ 35, 43, 51, 59},
    { 33, 41, 48, 56},{ 32, 39, 46, 53},{ 30, 37, 43, 50},{ 29, 35, 41, 48},
    { 27, 33, 39, 45},{ 26, 31, 37, 43},{ 24, 30, 35, 41},{ 23, 28, 33, 39},
    { 22, 27, 32, 37},{ 21, 26, 30, 35},{ 20, 24, 29, 33},{ 19, 23, 27, 31},
    { 18, 22, 26, 30},{ 17, 21, 25, 28},{ 16, 20, 23, 27},{ 15, 19, 22, 25},
    { 14, 18, 21, 24},{ 14, 17, 20, 23},{ 13, 16, 19, 22},{ 12, 15, 18, 21},
    { 12, 14, 17, 20},{ 11, 14, 16, 19},{ 11, 13, 15, 18},{ 10, 12, 15, 17},
    { 10, 12, 14, 16},{  9, 11, 13, 15},{  9, 11, 12, 14},{  8, 10, 12, 14},
    {  8,  9, 11, 13},{  7,  9, 11, 12},{  7,  9, 10, 12},{  7,  8, 10, 11},
    {  6,  8,  9, 11},{  6,  7,  9, 10},{  6,  7,  8,  9},{  2,  2,  2,  2},
};

const unsigned char coop_test_trans_idx_lps[64] = {
     0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9,11,11,12,
    13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,
    33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63,
};

const unsigned char coop_test_trans_idx_mps[64] = {
     1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63,
};

typedef struct {
    unsigned char state;   /* pStateIdx */
    unsigned char mps;     /* valMps */
} ctx_t;

typedef struct {
    unsigned char *out;
    size_t cap, len;
    unsigned bit_pos;      /* 0..7 within out[len] */
    unsigned low, range;
    int bits_outstanding;
    int first_bit;
    bool overflow;
} cabac_t;

static void put_raw_bit(cabac_t *c, unsigned b) {
    if (c->bit_pos == 0) {
        if (c->len >= c->cap) { c->overflow = true; return; }
        c->out[c->len] = 0;
    }
    if (b) { c->out[c->len] |= (unsigned char) (0x80u >> c->bit_pos); }
    c->bit_pos++;
    if (c->bit_pos == 8) { c->bit_pos = 0; c->len++; }
}

static void put_bit(cabac_t *c, unsigned b) {
    /* The very first bit produced by the engine is a leading zero that is not part of the
     * bitstream (9.3.4.3.3); dropping it is what firstBitFlag is for. */
    if (c->first_bit) {
        c->first_bit = 0;
    } else {
        put_raw_bit(c, b);
    }
    while (c->bits_outstanding > 0) {
        put_raw_bit(c, 1u - b);
        c->bits_outstanding--;
    }
}

static void renorm(cabac_t *c) {
    while (c->range < 256u) {
        if (c->low < 256u) {
            put_bit(c, 0);
        } else if (c->low >= 512u) {
            c->low -= 512u;
            put_bit(c, 1);
        } else {
            c->low -= 256u;
            c->bits_outstanding++;
        }
        c->range <<= 1;
        c->low <<= 1;
    }
}

static void cabac_init(cabac_t *c, unsigned char *out, size_t cap) {
    memset(c, 0, sizeof(*c));
    c->out = out; c->cap = cap;
    c->low = 0; c->range = 510; c->first_bit = 1;
}

static void encode_bin(cabac_t *c, ctx_t *ctx, unsigned bin) {
    unsigned q = (c->range >> 6) & 3u;
    unsigned lps = coop_test_range_tab_lps[ctx->state][q];
    c->range -= lps;
    if (bin != ctx->mps) {
        c->low += c->range;
        c->range = lps;
        if (ctx->state == 0) { ctx->mps = (unsigned char) (1u - ctx->mps); }
        ctx->state = coop_test_trans_idx_lps[ctx->state];
    } else {
        ctx->state = coop_test_trans_idx_mps[ctx->state];
    }
    renorm(c);
}

/** end_of_slice_segment_flag and friends: the "terminate" bin, which uses no context. */
static void encode_terminate(cabac_t *c, unsigned bin) {
    c->range -= 2u;
    if (bin) {
        c->low += c->range;
        /* Flush (9.3.4.3.5). The trailing `| 1` IS rbsp_stop_one_bit, so no separate
         * trailing-bits step is needed afterwards -- only byte alignment with zeros. */
        c->range = 2u;
        renorm(c);
        put_bit(c, (c->low >> 9) & 1u);
        unsigned two = ((c->low >> 7) & 3u) | 1u;
        put_bit(c, (two >> 1) & 1u);
        put_bit(c, two & 1u);
    } else {
        renorm(c);
    }
}

static void cabac_finish(cabac_t *c) {
    while (c->bit_pos != 0) { put_raw_bit(c, 0); }   /* byte_alignment with zeros */
}

static int clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }

static void ctx_from_init(ctx_t *ctx, int init_value, int qp) {
    int slope = (init_value >> 4) * 5 - 45;
    int offset = ((init_value & 15) << 3) - 16;
    int pre = clip3(1, 126, ((slope * clip3(0, 51, qp)) >> 4) + offset);
    ctx->mps = (unsigned char) (pre <= 63 ? 0 : 1);
    ctx->state = (unsigned char) (ctx->mps ? (pre - 64) : (63 - pre));
}

/* initValue tables, H.265 Table 9-6 (cu_skip_flag), 9-7 (split_cu_flag), 9-12
 * (merge_idx), 9-8/9-9 (sao_merge_flag, sao_type_idx). Indexed by initType, which is
 * 0 for I, and for P/B is chosen by cabac_init_flag (9.3.2.2). */
static const unsigned char init_split_cu[3][3] = {
    {139, 141, 157}, {107, 139, 126}, {107, 139, 126},
};
static const unsigned char init_cu_skip[3][3] = {
    {154, 154, 154},                    /* unused: I slices have no cu_skip_flag */
    {197, 185, 201}, {197, 185, 201},
};
/* VERIFIED AGAINST THE SPEC TABLES 2026-08-02, AND TWO OF THESE WERE WRONG.
 *
 * merge_idx was {154, 137, 122} -- initType 1 and 2 TRANSPOSED -- and sao_type_idx was
 * {200, 200, 200}, copied across all three rows instead of the real per-initType values.
 * Both errors land in initType 1, which is what a P slice with cabac_init_flag = 0 uses,
 * i.e. every picture this concealer has ever built. Every CTB codes a sao_type_idx and a
 * merge_idx, so the contexts were wrong for every CTB -- matching the measured symptom
 * exactly: error UNIFORM across all 34 CTB rows, content recognisable but matching no
 * picture, and no positional structure at all.
 *
 * THE ROUND-TRIP TEST COULD NOT CATCH THIS BY CONSTRUCTION: it initialises its decoder from
 * the SAME tables, so a wrong table is perfectly self-consistent there. It needed an
 * external authority (the spec / a real decoder's tables), which is the lesson. */
static const unsigned char init_merge_idx[3] = {154, 122, 137};
static const unsigned char init_sao_merge[3] = {153, 153, 153};
static const unsigned char init_sao_type[3]  = {200, 185, 160};

size_t coop_build_skip_slice_data(const coop_poc_params_t *p, const coop_slice_info_t *si,
                                  unsigned start_ctb, unsigned char *out, size_t cap) {
    if (p == NULL || si == NULL || !p->valid || out == NULL) {
        return 0;
    }
    /* An IRAP picture has no references, so skip is meaningless there; and an I slice
     * carries no cu_skip_flag at all. Refuse rather than emit something undecodable. */
    if (si->slice_type == 2) {
        return 0;
    }
    const unsigned total = p->w_ctbs * p->h_ctbs;
    if (start_ctb >= total || p->w_ctbs == 0) {
        return 0;
    }
    /* initType: I->0; P->cabac_init_flag?2:1; B->cabac_init_flag?1:2 (9.3.2.2). */
    const unsigned init_type = (si->slice_type == 1) ? (si->cabac_init_flag ? 2u : 1u)
                                                     : (si->cabac_init_flag ? 1u : 2u);
    const int qp = si->slice_qp;

    ctx_t split[3], skip[3], merge, sao_merge, sao_type;
    for (int i = 0; i < 3; ++i) {
        ctx_from_init(&split[i], init_split_cu[init_type][i], qp);
        ctx_from_init(&skip[i], init_cu_skip[init_type][i], qp);
    }
    ctx_from_init(&merge, init_merge_idx[init_type], qp);
    ctx_from_init(&sao_merge, init_sao_merge[init_type], qp);
    ctx_from_init(&sao_type, init_sao_type[init_type], qp);

    cabac_t c;
    cabac_init(&c, out, cap);

    /* CTB size vs minimum CB size decides whether split_cu_flag exists at depth 0. With
     * the encoder in use CTB=32 and min CB=16, so it does and we code a single 0. */
    const bool has_split_flag = p->log2_diff_max_min_cb > 0;

    for (unsigned ctb = start_ctb; ctb < total; ++ctb) {
        const unsigned rx = ctb % p->w_ctbs;
        /* Availability is SLICE-scoped: the CTB to the left/above must be in THIS slice.
         * Our slice starts at a row boundary, so "above" is unavailable for its first row
         * and "left" for column 0. Every CU we emit is skip, so these flags are exactly
         * the cu_skip_flag context increment. */
        const bool avail_left = rx > 0 && ctb > start_ctb;
        const bool avail_up = ctb >= start_ctb + p->w_ctbs;

        if (si->sao_luma || si->sao_chroma) {
            /* Code "no SAO here": merge flags 0, then sao_type_idx 0 per component. */
            if (avail_left) { encode_bin(&c, &sao_merge, 0); }
            if (avail_up) { encode_bin(&c, &sao_merge, 0); }
            if (si->sao_luma) { encode_bin(&c, &sao_type, 0); }
            if (si->sao_chroma && p->chroma_format_idc != 0) { encode_bin(&c, &sao_type, 0); }
        }

        /* coding_quadtree at depth 0: not split, so the CU is the whole CTB. Every
         * neighbour is also depth 0, so the split context increment is always 0. */
        if (has_split_flag) {
            encode_bin(&c, &split[0], 0);
        }
        /* coding_unit: skip, with merge candidate 0. */
        encode_bin(&c, &skip[(avail_left ? 1u : 0u) + (avail_up ? 1u : 0u)], 1);
        if (si->max_num_merge_cand > 1) {
            encode_bin(&c, &merge, 0);   /* merge_idx = 0: first bin only, value 0 */
        }
        encode_terminate(&c, ctb + 1u == total ? 1u : 0u);
        if (c.overflow) {
            return 0;
        }
    }
    cabac_finish(&c);
    return c.overflow ? 0 : c.len;
}

size_t coop_build_skip_slice(const coop_poc_params_t *p, const unsigned char *our_slice,
                             size_t our_len, unsigned addr,
                             unsigned char *scratch, size_t scratch_cap,
                             unsigned char *out, size_t out_cap) {
    coop_slice_info_t si;
    if (!coop_slice_info(p, our_slice, our_len, &si)) {
        return 0;
    }
    size_t n = coop_build_skip_slice_data(p, &si, addr, scratch, scratch_cap);
    if (n == 0) {
        return 0;
    }
    return coop_slice_set_address_payload(p, our_slice, our_len, addr, scratch, n,
                                          out, out_cap);
}
