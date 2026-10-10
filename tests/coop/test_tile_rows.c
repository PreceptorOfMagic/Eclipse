/*
 * Offline proof that the HORIZONTAL tile-row parameter sets are well formed.
 *
 * Takes any real capture, lifts its non-VCL run, builds the tiled composite parameter sets
 * with coop_nonvcl_tile_rows(), and re-parses them. A tiled composite is only usable if the
 * SPS still describes a picture of twice the height AND the PPS re-parses with tiles on --
 * both of which are silent failures otherwise: a malformed PPS decodes as "no tiles" and the
 * peer's slice then lands at the wrong CTB address with no error anywhere.
 *
 *   ./test_tile_rows capture.h265
 */
#include "../../src/app/stream/video/coop_splice.h"
#include "../../src/app/stream/video/coop_poc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <capture.h265>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    static unsigned char buf[8u << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    coop_au_t au;
    if (!coop_au_parse(buf, n, &au) || au.non_vcl == NULL || au.non_vcl_len == 0) {
        fprintf(stderr, "no non-VCL run in %s\n", argv[1]);
        return 1;
    }
    coop_poc_params_t pane;
    int fails = 0;
    if (coop_poc_params_parse(au.non_vcl, au.non_vcl_len, &pane) && pane.valid) {
        printf("source  : %ux%u CTBs  addr_bits=%u\n", pane.w_ctbs, pane.h_ctbs,
               pane.slice_addr_bits);
    } else {
        printf("source  : (unparsed)\n");
    }

    static unsigned char out[64u << 10];
    size_t got = coop_nonvcl_tile_rows(au.non_vcl, au.non_vcl_len, out, sizeof(out));
    printf("%-8s: coop_nonvcl_tile_rows -> %zu bytes\n", got ? "[PASS]" : "[FAIL]", got);
    if (got == 0) return 1;

    coop_poc_params_t comp;
    if (!coop_poc_params_parse(out, got, &comp) || !comp.valid) {
        printf("[FAIL] composite parameter sets do not re-parse\n");
        return 1;
    }
    printf("composite: %ux%u CTBs  addr_bits=%u\n", comp.w_ctbs, comp.h_ctbs,
           comp.slice_addr_bits);

    if (comp.w_ctbs != pane.w_ctbs) { printf("[FAIL] width changed (%u -> %u)\n",
                                             pane.w_ctbs, comp.w_ctbs); fails++; }
    else printf("[PASS] width unchanged (%u CTBs)\n", comp.w_ctbs);

    if (comp.h_ctbs != pane.h_ctbs * 2u) { printf("[FAIL] height not doubled (%u -> %u)\n",
                                                  pane.h_ctbs, comp.h_ctbs); fails++; }
    else printf("[PASS] height doubled (%u -> %u CTBs)\n", pane.h_ctbs, comp.h_ctbs);

    /* The bottom tile row starts at the first CTB of composite row h_ctbs/2, and for a
     * FULL-WIDTH tile row that raster address is also its tile-scan address -- the property
     * the whole horizontal-tiles idea rests on. */
    const unsigned bottom = coop_bottom_half_addr(&comp);
    const unsigned expect = comp.w_ctbs * (comp.h_ctbs / 2u);
    if (bottom != expect) { printf("[FAIL] bottom tile addr %u, expected %u\n", bottom, expect); fails++; }
    else printf("[PASS] bottom tile starts at CTB %u\n", bottom);

    if (argc > 2) {
        FILE *o = fopen(argv[2], "wb");
        if (o) { fwrite(out, 1, got, o); fclose(o); printf("wrote %s\n", argv[2]); }
    }
    printf(fails ? "\nFAILURES (%d)\n" : "\nPASS: tile-row parameter sets\n", fails);
    return fails ? 1 : 0;
}
