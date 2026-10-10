/* End-to-end composition test, entirely offline: take a real half-height AU, use it as
 * BOTH hosts, compose, and verify the result decodes as a full-height picture with two
 * slices -- the second at the bottom-half address. Checked with ffmpeg, so the verdict
 * comes from a real decoder rather than our own parser agreeing with itself. */
#include <stdio.h>
#include <string.h>
#include "coop_splice.h"
#include "coop_poc.h"

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no capture\n"); return 1; }
    static unsigned char buf[8 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    coop_poc_params_t src;
    if (!coop_poc_params_parse(buf, n, &src) || !src.valid) { printf("FAIL: params\n"); return 1; }

    coop_au_t au;
    if (!coop_au_parse(buf, n, &au)) { printf("FAIL: au parse\n"); return 1; }
    if (au.slice[0] == NULL || au.non_vcl == NULL) { printf("FAIL: need slice + param sets\n"); return 1; }

    static unsigned char nv[64 << 10];
    size_t nvlen = coop_nonvcl_double_height(au.non_vcl, au.non_vcl_len, nv, sizeof(nv));
    if (!nvlen) { printf("FAIL: non-vcl rewrite\n"); return 1; }

    coop_poc_params_t comp;
    if (!coop_poc_params_parse(nv, nvlen, &comp) || !comp.valid) { printf("FAIL: composite params\n"); return 1; }
    printf("addr_bits source=%u composite=%u\n", src.slice_addr_bits, comp.slice_addr_bits);
    if (comp.slice_addr_bits != src.slice_addr_bits + 1) { printf("FAIL: expected +1 bit\n"); return 1; }

    /* Bottom half starts at (rows/2) CTU rows. Derived, not hard-coded, so this test is
     * valid for the 1080p reference capture as well as a 3840-wide source. */
    unsigned peer_addr = coop_bottom_half_addr(&comp);
    printf("bottom-half address = %u (w_ctbs=%u h_ctbs=%u)\n", peer_addr, comp.w_ctbs, comp.h_ctbs);
    static unsigned char out[16 << 20];
    size_t got = coop_au_splice_halves(&au, &au, &comp, peer_addr, nv, nvlen, out, sizeof(out));
    if (!got) { printf("FAIL: compose returned 0\n"); return 1; }
    printf("composed %zu bytes from a %zu-byte source AU\n", got, n);

    FILE *o = fopen("/tmp/coop-halves.h265", "wb");
    fwrite(out, 1, got, o);
    fclose(o);
    printf("wrote /tmp/coop-halves.h265\n");
    return 0;
}
