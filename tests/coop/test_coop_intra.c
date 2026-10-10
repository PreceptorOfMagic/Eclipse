/*
 * DOES A TRAIL_R PICTURE CONTAINING OUR P-SLICE AND THE PEER'S I-SLICE DECODE?
 *
 * That single question decides whether the co-op composite can stop being frame-locked.
 * Today the peer's inter slices predict from the peer's previous picture, so a dropped
 * pane is a HOLE, and recovering from it costs a re-anchor that needs both machines to
 * emit a keyframe simultaneously. If the peer's slice references nothing, any pane can be
 * spliced against any picture of ours, in any order, with gaps -- and the entire
 * sequencing apparatus (coop_seq, the sticky IDR, the pane queue) becomes unnecessary.
 *
 * HEVC permits it because slice_type is PER SLICE while nal_unit_type is per picture. The
 * test builds the mixed picture for real and hands it to ffmpeg, because our own parser
 * agreeing with itself would prove nothing -- the verdict has to come from a decoder.
 *
 * Usage: test_coop_intra <capture.h265>   (a real Apollo capture containing an IDR)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_splice.h"
#include "coop_poc.h"

static const unsigned char SC[4] = {0, 0, 0, 1};

static int fail(const char *msg) {
    printf("FAIL: %s\n", msg);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { return fail("usage: test_coop_intra <capture.h265>"); }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { return fail("cannot open capture"); }
    static unsigned char buf[16 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    coop_poc_params_t src;
    if (!coop_poc_params_parse(buf, n, &src) || !src.valid) { return fail("params"); }

    /* Find an IDR access unit (the self-contained pane) and a later inter one (ours). */
    coop_au_t idr, inter;
    memset(&idr, 0, sizeof(idr));
    memset(&inter, 0, sizeof(inter));
    int have_idr = 0, have_inter = 0;
    size_t pos = 0;
    while (pos + 4 <= n && (!have_idr || !have_inter)) {
        size_t next = pos + 4;
        while (next + 4 <= n && memcmp(buf + next, SC, 4) != 0) { next++; }
        size_t au_len = (next + 4 <= n ? next : n) - pos;
        coop_au_t au;
        if (coop_au_parse(buf + pos, au_len, &au) && au.slice[0] != NULL && au.slice_len[0] > 0) {
            int t = (au.slice[0][0] >> 1) & 0x3F;
            if (!have_idr && (t == 19 || t == 20)) { idr = au; have_idr = 1; }
            else if (!have_inter && t >= 0 && t <= 9) { inter = au; have_inter = 1; }
        }
        pos = next;
        if (next + 4 > n) { break; }
    }
    if (!have_idr) { return fail("no IDR access unit in capture"); }
    printf("found IDR slice (%zu bytes)%s\n", idr.slice_len[0],
           have_inter ? " and an inter slice" : " but NO inter slice");

    /* Composite parameter sets: the picture is two panes tall. The parameter sets ride on
     * the FILE's first access unit, not necessarily on the IDR we isolated above. */
    coop_au_t head;
    if (!coop_au_parse(buf, n, &head) || head.non_vcl == NULL || head.non_vcl_len == 0) {
        return fail("no parameter sets at the head of the capture");
    }
    static unsigned char nv[64 << 10];
    size_t nvlen = coop_nonvcl_double_height(head.non_vcl, head.non_vcl_len, nv, sizeof(nv));
    if (!nvlen) { return fail("non-vcl rewrite"); }
    coop_poc_params_t comp;
    if (!coop_poc_params_parse(nv, nvlen, &comp) || !comp.valid) { return fail("composite params"); }
    const unsigned bottom = coop_bottom_half_addr(&comp);
    printf("composite %ux%u CTBs, bottom-half addr %u\n", comp.w_ctbs, comp.h_ctbs, bottom);

    /* THE CONVERSION UNDER TEST: IDR slice -> TRAIL_R I-slice with an empty RPS. */
    static unsigned char conv[8 << 20], placed[8 << 20];
    size_t clen = coop_slice_idr_to_trail(&comp, idr.slice[0], idr.slice_len[0], 1, conv, sizeof(conv));
    if (!clen) { return fail("coop_slice_idr_to_trail refused the slice"); }
    int ct = (conv[0] >> 1) & 0x3F;
    printf("converted: nal_unit_type %d -> %d, %zu -> %zu bytes\n",
           (idr.slice[0][0] >> 1) & 0x3F, ct, idr.slice_len[0], clen);
    if (ct != 1) { return fail("converted slice is not TRAIL_R"); }

    /* Verify our own walk still understands it, and that it is still an I-slice. */
    coop_slice_info_t info;
    if (!coop_slice_info(&comp, conv, clen, &info)) { return fail("converted header unwalkable"); }
    printf("converted slice_type=%u (2=I) qp=%d\n", info.slice_type, info.slice_qp);
    if (info.slice_type != 2) { return fail("converted slice is no longer an I-slice"); }

    /* THE QP INVARIANT -- this is the check that has teeth.
     *
     * "walkable" and "still an I-slice" are both decided BEFORE the inserted fields, so a
     * conversion that writes the insertion wrongly still passes them. That is not
     * hypothetical: a missing inter_ref_pic_set_prediction_flag shipped, ffmpeg reported 0
     * errors, and the pane decoded to garbage on the TV. slice_qp_delta sits in the tail,
     * AFTER the insertion, and the conversion does not touch it -- so the converted slice
     * MUST report the source slice's QP. Any bit miscounted in between moves it. The broken
     * version read 26 against the source's 42; that one number was the whole bug, visible
     * with no decoder at all. */
    coop_slice_info_t orig;
    if (!coop_slice_info(&src, idr.slice[0], idr.slice_len[0], &orig)) {
        return fail("could not walk the ORIGINAL IDR slice for the QP invariant");
    }
    printf("qp invariant: original=%d converted=%d\n", orig.slice_qp, info.slice_qp);
    if (orig.slice_qp != info.slice_qp) {
        printf("FAIL: QP moved -- the inserted fields are misaligned, so every later field\n"
               "      (SAO flags, slice_qp_delta, the byte CABAC starts at) is off.\n");
        return 1;
    }

    size_t plen = coop_slice_set_address(&comp, conv, clen, bottom, placed, sizeof(placed));
    if (!plen) { return fail("could not place converted slice at the bottom half"); }

    /* Stream: [1] a legal composite IDR to start the decoder, then [2] the MIXED picture --
     * our slice over the peer's converted I-slice. If ffmpeg decodes both, the mixed
     * picture is valid and the whole sequencing problem is optional. */
    FILE *o = fopen("/tmp/coop-intra.h265", "wb");
    if (!o) { return fail("cannot write /tmp/coop-intra.h265"); }
    static unsigned char au1[16 << 20];
    size_t got = coop_au_splice_halves(&head, &head, &comp, bottom, nv, nvlen, au1, sizeof(au1));
    if (!got) { fclose(o); return fail("bootstrap composite"); }
    fwrite(au1, 1, got, o);

    const coop_au_t *mine = have_inter ? &inter : &idr;
    if (have_inter) {
        fwrite(nv, 1, nvlen, o);
        fwrite(SC, 1, 4, o);
        fwrite(mine->slice[0], 1, mine->slice_len[0], o);   /* our P-slice, slice 0 */
        fwrite(SC, 1, 4, o);
        fwrite(placed, 1, plen, o);                          /* peer's I-slice, bottom half */
    }
    fclose(o);
    printf("wrote /tmp/coop-intra.h265 (%s)\n",
           have_inter ? "bootstrap IDR + MIXED P/I picture" : "bootstrap only -- no inter slice found");
    /* EXPECT GREEN IN THE LOWER PART OF EACH PANE, AND DO NOT READ IT AS A FAULT. The
     * reference capture predates half-height and carries TWO slices per picture, so
     * slice[0] holds only the top half of its source picture; the CTBs no slice covers come
     * out as untouched YUV green. It appears identically in the bootstrap frame, which uses
     * the separately proven splice path. The claim under test is that the MIXED picture
     * decodes and both slices land in the right place -- not that the panes are full. */
    printf("NOTE: each pane will be part-green -- the reference capture has 2 slices/picture\n"
           "      and this uses slice[0] only. Same in the bootstrap frame. Not a fault.\n");
    printf("PASS: conversion produced a walkable TRAIL_R I-slice; decode with ffmpeg to judge\n");
    return 0;
}
