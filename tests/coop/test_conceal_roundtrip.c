/*
 * ROUND-TRIP THE SYNTHESISED SKIP SLICE THROUGH A REFERENCE CABAC DECODER.
 *
 * WHY THIS AND NOT ANOTHER PIXEL TEST. coop_conceal.c writes CABAC by hand, and every
 * check it had was end-to-end: build a stream, decode it, look at the picture. That cannot
 * separate "the bins are wrong" from "the bins are right and the slice means something
 * different than intended" -- and it let a slice that ffmpeg calls error-free, and whose
 * decoded pixels are wrong, pass as correct for weeks.
 *
 * So: encode with coop_build_skip_slice_data(), then decode the SAME bytes with an
 * independent implementation of the H.265 arithmetic decoding engine (9.3.4.3.2 DecodeBin,
 * 9.3.4.3.5 DecodeTerminate), reading the exact syntax an all-skip CTU should contain.
 * Every CTB must read back as: no SAO merge, sao_type_idx 0 per coded component, no split,
 * cu_skip_flag 1, merge_idx 0, end_of_slice_segment_flag 0 -- and 1 on the last CTB, at
 * which point the payload must be exhausted.
 *
 * THIS TEST CANNOT CATCH A WRONG initValue TABLE, and that is exactly what was wrong with
 * the concealer for weeks. It initialises its contexts from the SAME numbers the encoder
 * uses, so a mis-transcribed table is perfectly self-consistent here and the round trip
 * passes while a real decoder produces garbage. Two values were wrong -- merge_idx had
 * initType 1 and 2 transposed, and sao_type_idx was {200,200,200} instead of {200,185,160}
 * -- and only an EXTERNAL authority (the spec tables / a real decoder's) could show it.
 * If the pixels are wrong and this test passes, SUSPECT THE TABLES FIRST.
 */
#include "../../src/app/stream/video/coop_conceal.h"
#include "../../src/app/stream/video/coop_poc.h"
#include "../../src/app/stream/video/coop_splice.h"

#include <stdlib.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("  FAIL: ");                                                 \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
            failures++;                                                         \
        }                                                                       \
    } while (0)

/* ---- the reference DECODING engine (H.265 9.3.4.3) --------------------------------- */

extern const unsigned char coop_test_range_tab_lps[64][4];
extern const unsigned char coop_test_trans_idx_lps[64];
extern const unsigned char coop_test_trans_idx_mps[64];

typedef struct { unsigned char state, mps; } dctx_t;

typedef struct {
    const unsigned char *buf;
    size_t len;
    size_t bit;          /* next bit to read */
    unsigned range, offset;
    int past_end;        /* bits read beyond the payload */
} dcabac_t;

static unsigned d_bit(dcabac_t *d) {
    if (d->bit >= d->len * 8u) { d->past_end++; return 0; }
    unsigned b = (d->buf[d->bit >> 3] >> (7u - (d->bit & 7u))) & 1u;
    d->bit++;
    return b;
}

static void d_init(dcabac_t *d, const unsigned char *buf, size_t len) {
    memset(d, 0, sizeof(*d));
    d->buf = buf; d->len = len;
    d->range = 510;
    d->offset = 0;
    for (int i = 0; i < 9; ++i) { d->offset = (d->offset << 1) | d_bit(d); }
}

static void d_renorm(dcabac_t *d) {
    while (d->range < 256u) {
        d->range <<= 1;
        d->offset = (d->offset << 1) | d_bit(d);
    }
}

static unsigned d_decision(dcabac_t *d, dctx_t *ctx) {
    unsigned q = (d->range >> 6) & 3u;
    unsigned lps = coop_test_range_tab_lps[ctx->state][q];
    d->range -= lps;
    unsigned bin;
    if (d->offset >= d->range) {
        bin = 1u - ctx->mps;
        d->offset -= d->range;
        d->range = lps;
        if (ctx->state == 0) { ctx->mps = (unsigned char) (1u - ctx->mps); }
        ctx->state = coop_test_trans_idx_lps[ctx->state];
    } else {
        bin = ctx->mps;
        ctx->state = coop_test_trans_idx_mps[ctx->state];
    }
    d_renorm(d);
    return bin;
}

static unsigned d_terminate(dcabac_t *d) {
    d->range -= 2u;
    if (d->offset >= d->range) { return 1u; }
    d_renorm(d);
    return 0u;
}

/* Context initialisation, identical formula to the encoder's (9.3.2.2). */
static int clip3i(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }
static void d_ctx_init(dctx_t *c, int init_value, int qp) {
    int slope = (init_value >> 4) * 5 - 45;
    int offset = ((init_value & 15) << 3) - 16;
    int pre = clip3i(1, 126, ((slope * clip3i(0, 51, qp)) >> 4) + offset);
    c->mps = (unsigned char) (pre <= 63 ? 0 : 1);
    c->state = (unsigned char) (c->mps ? (pre - 64) : (63 - pre));
}

static const unsigned char i_split[3][3] = {
    {139, 141, 157}, {107, 139, 126}, {107, 139, 126},
};
static const unsigned char i_skip[3][3] = {
    {154, 154, 154}, {197, 185, 201}, {197, 185, 201},
};
static const unsigned char i_merge[3] = {154, 122, 137};
static const unsigned char i_sao_merge[3] = {153, 153, 153};
static const unsigned char i_sao_type[3] = {200, 185, 160};

/* ---- the test ---------------------------------------------------------------------- */

static void run(const char *name, const coop_poc_params_t *p, const coop_slice_info_t *si,
                unsigned start_ctb) {
    printf("%s: %ux%u CTBs, start=%u, type=%u qp=%d merge=%u sao=%u/%u cabac_init=%u\n",
           name, p->w_ctbs, p->h_ctbs, start_ctb, si->slice_type, si->slice_qp,
           si->max_num_merge_cand, si->sao_luma, si->sao_chroma, si->cabac_init_flag);

    static unsigned char payload[4u << 20];
    size_t n = coop_build_skip_slice_data(p, si, start_ctb, payload, sizeof(payload));
    CHECK(n > 0, "%s: builder refused", name);
    if (n == 0) { return; }

    const unsigned total = p->w_ctbs * p->h_ctbs;
    const unsigned init_type = (si->slice_type == 1) ? (si->cabac_init_flag ? 2u : 1u)
                                                     : (si->cabac_init_flag ? 1u : 2u);
    const int qp = si->slice_qp;

    dctx_t split[3], skip[3], merge, sao_merge, sao_type;
    for (int i = 0; i < 3; ++i) {
        d_ctx_init(&split[i], i_split[init_type][i], qp);
        d_ctx_init(&skip[i], i_skip[init_type][i], qp);
    }
    d_ctx_init(&merge, i_merge[init_type], qp);
    d_ctx_init(&sao_merge, i_sao_merge[init_type], qp);
    d_ctx_init(&sao_type, i_sao_type[init_type], qp);

    dcabac_t d;
    d_init(&d, payload, n);

    const int has_split = p->log2_diff_max_min_cb > 0;
    unsigned bad = 0;
    unsigned first_bad = 0;
    for (unsigned ctb = start_ctb; ctb < total; ++ctb) {
        const unsigned rx = ctb % p->w_ctbs;
        const int avail_left = rx > 0 && ctb > start_ctb;
        const int avail_up = ctb >= start_ctb + p->w_ctbs;
        unsigned wrong = 0;

        if (si->sao_luma || si->sao_chroma) {
            if (avail_left && d_decision(&d, &sao_merge) != 0) { wrong |= 1u; }
            if (avail_up && d_decision(&d, &sao_merge) != 0) { wrong |= 2u; }
            if (si->sao_luma && d_decision(&d, &sao_type) != 0) { wrong |= 4u; }
            if (si->sao_chroma && p->chroma_format_idc != 0 &&
                d_decision(&d, &sao_type) != 0) { wrong |= 8u; }
        }
        if (has_split && d_decision(&d, &split[0]) != 0) { wrong |= 16u; }
        if (d_decision(&d, &skip[(avail_left ? 1u : 0u) + (avail_up ? 1u : 0u)]) != 1u) {
            wrong |= 32u;
        }
        if (si->max_num_merge_cand > 1 && d_decision(&d, &merge) != 0) { wrong |= 64u; }
        const unsigned last = (ctb + 1u == total);
        if (d_terminate(&d) != last) { wrong |= 128u; }

        if (wrong) {
            if (!bad) { first_bad = ctb; }
            bad++;
        }
    }
    CHECK(bad == 0, "%s: %u of %u CTBs did not read back as all-skip (first at %u)",
          name, bad, total - start_ctb, first_bad);
    CHECK(d.past_end == 0, "%s: decoder read %d bits past the end of the payload",
          name, d.past_end);
    /* The engine legitimately holds up to a couple of bytes of lookahead, but a large
     * shortfall means the encoder emitted a different number of bins than the syntax has. */
    const size_t consumed = (d.bit + 7u) / 8u;
    CHECK(consumed + 2u >= n, "%s: decoder consumed %zu of %zu payload bytes -- the encoder "
          "wrote bins the syntax does not contain", name, consumed, n);
    printf("  %zu payload bytes, decoder consumed %zu, %u CTBs\n", n, consumed,
           total - start_ctb);
}

/* ---- the ASSEMBLED NAL, not just the payload ---------------------------------------
 *
 * The payload test above proves the bin sequence is self-consistent. It says nothing about
 * where that payload LANDS. coop_slice_set_address_payload() rewrites our header with a new
 * slice address, emits a FRESH byte_alignment() -- the old one was aligned for a header of a
 * different length -- and then copies the payload from the next byte boundary. If that
 * junction is off by even one bit the decoder starts its CABAC engine on the wrong bit, and
 * the result is not garbage: the engine still decodes, still fills every CTB, and produces
 * plausible-looking content with spurious residual. That is exactly the observed fault.
 */
static size_t strip_epb(const unsigned char *in, size_t len, unsigned char *out) {
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < len; ++i) {
        if (zeros >= 2 && in[i] == 0x03) { zeros = 0; continue; }
        out[o++] = in[i];
        zeros = (in[i] == 0) ? zeros + 1 : 0;
    }
    return o;
}

static void run_assembled(const char *name, const coop_poc_params_t *p,
                          const unsigned char *our_slice, size_t our_len, unsigned addr) {
    static unsigned char scratch[4u << 20];
    static unsigned char nal[8u << 20];
    static unsigned char rbsp[8u << 20];

    coop_slice_info_t si;
    CHECK(coop_slice_info(p, our_slice, our_len, &si), "%s: slice_info refused", name);

    size_t pn = coop_build_skip_slice_data(p, &si, addr, scratch, sizeof(scratch));
    CHECK(pn > 0, "%s: payload build refused", name);

    static unsigned char scratch2[4u << 20];
    size_t nn = coop_build_skip_slice(p, our_slice, our_len, addr,
                                      scratch2, sizeof(scratch2), nal, sizeof(nal));
    CHECK(nn > 0, "%s: assembly refused", name);
    if (nn == 0 || pn == 0) { return; }

    size_t rn = strip_epb(nal, nn, rbsp);

    /* Find the payload inside the assembled RBSP and check it starts on a byte boundary
     * with nothing but the header before it. */
    size_t found = (size_t) -1;
    for (size_t i = 0; i + pn <= rn; ++i) {
        if (memcmp(rbsp + i, scratch, pn < 16 ? pn : 16) == 0 &&
            memcmp(rbsp + i, scratch, pn) == 0) { found = i; break; }
    }
    CHECK(found != (size_t) -1, "%s: the payload is NOT present in the assembled NAL -- the "
          "header/payload junction rewrote or truncated it", name);
    if (found == (size_t) -1) { return; }
    CHECK(found + pn == rn, "%s: %zu bytes follow the payload in the NAL (should be 0)",
          name, rn - (found + pn));

    /* The byte before the payload must carry the header's byte_alignment: a 1 bit followed
     * by zeros. If the header ended exactly on a boundary the alignment byte is 0x80. */
    unsigned char last = rbsp[found - 1];
    int ok_align = 0;
    for (int b = 0; b < 8; ++b) {
        if (last == (unsigned char) (1u << b)) { ok_align = 1; break; }
        if ((last & ((1u << (b + 1)) - 1u)) == (unsigned char) (1u << b)) { ok_align = 1; break; }
    }
    CHECK(ok_align, "%s: byte before payload = 0x%02x, which is not a byte_alignment "
          "(one 1 bit then zeros) -- CABAC will start on the wrong bit", name, last);

    /* THE INVARIANT THAT MATTERS, and the one a memcmp cannot check: an INDEPENDENT walk of
     * the assembled header must agree on where the payload begins. The decoder does not
     * find our payload by searching for it -- it walks the header it was given and starts
     * its CABAC engine at the next byte boundary. If the assembled header decodes to a
     * different length than the assembler assumed, the engine starts on the wrong byte and
     * still decodes: every CTB gets filled, the picture looks plausible, and residual
     * appears where an all-skip slice can have none. */
    size_t hdr_end = coop_slice_header_end_bit_public(p, nal, nn);
    CHECK(hdr_end != 0, "%s: the assembled header does not walk", name);
    if (hdr_end != 0) {
        size_t data_byte = ((hdr_end + 1u) + 7u) / 8u;
        CHECK(data_byte == found,
              "%s: the assembled header walks to bit %zu -> payload at byte %zu, but the "
              "payload was written at byte %zu. The decoder starts CABAC %zd byte(s) off.",
              name, hdr_end, data_byte, found, (ssize_t) data_byte - (ssize_t) found);
    }
    printf("  assembled: %zu NAL bytes, %zu rbsp, payload at %zu (%zu header bytes), "
           "align byte 0x%02x\n", nn, rn, found, found, last);
}

static unsigned char real_stream[64u << 20];
static size_t real_stream_len = 0;
static const unsigned char *real_slice = NULL;
static size_t real_slice_len = 0;

/* Take the FIRST inter access unit's own slice out of a real capture. Its header is what
 * the concealment slice inherits, so it is the only header worth testing the junction on. */
static void load_capture(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) { printf("  (cannot open %s)\n", path); return; }
    real_stream_len = fread(real_stream, 1, sizeof(real_stream), f);
    fclose(f);
    coop_poc_params_t p;
    memset(&p, 0, sizeof(p));
    if (!coop_poc_params_parse(real_stream, real_stream_len, &p) || !p.valid) { return; }
    size_t at = 0;
    while (at + 5 < real_stream_len) {
        size_t end = at + 4;
        while (end + 5 < real_stream_len &&
               !(real_stream[end] == 0 && real_stream[end+1] == 0 &&
                 real_stream[end+2] == 0 && real_stream[end+3] == 1 &&
                 ((real_stream[end+4] >> 1) & 0x3F) == 32)) { end++; }
        if (end + 5 >= real_stream_len) { end = real_stream_len; }
        coop_au_t au;
        coop_slice_info_t si;
        if (coop_au_parse(real_stream + at, end - at, &au) && au.slice[0] != NULL &&
            coop_slice_info(&p, au.slice[0], au.slice_len[0], &si) && si.slice_type != 2) {
            real_slice = au.slice[0];
            real_slice_len = au.slice_len[0];
            return;
        }
        at = end;
    }
}

int main(int argc, char **argv) {
    if (argc > 1) { load_capture(argv[1]); }
    /* The real composite geometry: 3840x2176, CTB 32 -> 120x68, bottom half at 34*120. */
    coop_poc_params_t p;
    memset(&p, 0, sizeof(p));
    p.valid = true;
    p.w_ctbs = 120; p.h_ctbs = 68;
    p.log2_diff_max_min_cb = 1;
    p.chroma_format_idc = 1;
    p.slice_addr_bits = 13;

    coop_slice_info_t si;
    memset(&si, 0, sizeof(si));
    si.slice_type = 1;              /* P, as the live capture shows */
    si.slice_qp = 11;
    si.max_num_merge_cand = 5;
    si.sao_luma = 1; si.sao_chroma = 1;
    si.cabac_init_flag = 0;

    run("live geometry", &p, &si, 4080);

    si.sao_luma = 0; si.sao_chroma = 0;
    run("no SAO", &p, &si, 4080);

    si.sao_luma = 1; si.sao_chroma = 1;
    si.max_num_merge_cand = 1;
    run("single merge candidate", &p, &si, 4080);

    si.max_num_merge_cand = 5;
    si.slice_qp = 40;
    run("high QP", &p, &si, 4080);

    /* With a real capture, also exercise the ASSEMBLED NAL against a genuine slice header.
     * Synthetic parameters cannot test the header/payload junction -- that needs a header
     * that was actually produced by the encoder. */
    if (real_slice_len > 0) {
        coop_poc_params_t rp;
        memset(&rp, 0, sizeof(rp));
        if (coop_poc_params_parse(real_stream, real_stream_len, &rp) && rp.valid) {
            run_assembled("real capture", &rp, real_slice, real_slice_len,
                          coop_bottom_half_addr(&rp));
        } else {
            printf("  (capture given but its parameter sets did not parse)\n");
        }
    } else {
        printf("  (no capture given: pass one to also test the assembled NAL)\n");
    }

    if (failures > 0) {
        printf("FAIL: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: conceal round-trip\n");
    return 0;
}
