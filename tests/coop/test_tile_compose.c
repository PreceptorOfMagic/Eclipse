/*
 * Offline: build a TILE-ROW composite from a real PANE stream and let ffmpeg judge it.
 *
 * The pane is mirrored into BOTH tiles, so the only thing under test is coop_slice_to_tile():
 * tile 0 takes the pane slice at addr 0 (want_first), tile 1 takes the SAME slice at the bottom
 * tile's address. If ffmpeg decodes the output, the tiling primitives are sound and the live
 * black screen is elsewhere; if it does not, the failure is reproducible with no TV in the loop.
 *
 *   ./test_tile_compose pane.h265 out.h265 [N]
 */
#include "../../src/app/stream/video/coop_splice.h"
#include "../../src/app/stream/video/coop_poc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char in[16u << 20], out[8u << 20], nonvcl[64u << 10];

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s pane.h265 out.h265 [N]\n", argv[0]); return 2; }
    const int want = (argc > 3) ? atoi(argv[3]) : 60;
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 2; }
    size_t n = fread(in, 1, sizeof(in), f); fclose(f);
    FILE *o = fopen(argv[2], "wb"); if (!o) { perror("out"); return 2; }

    coop_poc_params_t pane, comp;
    size_t nvl = 0, off = 0, made = 0, refused_top = 0, refused_bot = 0;
    static const unsigned char sc[4] = {0,0,0,1};

    while (off < n && (int) made < want) {
        /* Walk AU by AU: an AU starts at a VPS(32). */
        size_t end = off + 4;
        while (end + 4 <= n) {
            if (in[end]==0&&in[end+1]==0&&in[end+2]==0&&in[end+3]==1 &&
                (((in[end+4]>>1)&0x3F) == 32) && end > off) break;
            end++;
        }
        if (end + 4 > n) end = n;
        coop_au_t au;
        if (coop_au_parse(in + off, end - off, &au) && au.slice[0] != NULL) {
            if (nvl == 0 && au.non_vcl && au.non_vcl_len) {
                if (!coop_poc_params_parse(au.non_vcl, au.non_vcl_len, &pane) || !pane.valid) {
                    fprintf(stderr, "pane params will not parse\n"); return 1;
                }
                nvl = coop_nonvcl_tile_rows(au.non_vcl, au.non_vcl_len, nonvcl, sizeof(nonvcl));
                if (!nvl || !coop_poc_params_parse(nonvcl, nvl, &comp) || !comp.valid) {
                    fprintf(stderr, "tile-row params will not build/parse\n"); return 1;
                }
                printf("pane %ux%u CTBs -> composite %ux%u CTBs, bottom tile addr %u\n",
                       pane.w_ctbs, pane.h_ctbs, comp.w_ctbs, comp.h_ctbs,
                       coop_bottom_half_addr(&comp));
            }
            if (nvl) {
                size_t p = 0;
                memcpy(out + p, nonvcl, nvl); p += nvl;
                memcpy(out + p, sc, 4); p += 4;
                size_t g = coop_slice_to_tile(&pane, &comp, au.slice[0], au.slice_len[0], 0,
                                              out + p, sizeof(out) - p);
                if (g == 0) { refused_top++; }
                else {
                    p += g;
                    memcpy(out + p, sc, 4); p += 4;
                    size_t g2 = coop_slice_to_tile(&pane, &comp, au.slice[0], au.slice_len[0],
                                                   coop_bottom_half_addr(&comp),
                                                   out + p, sizeof(out) - p);
                    if (g2 == 0) { refused_bot++; }
                    else { p += g2; fwrite(out, 1, p, o); made++; }
                }
            }
        }
        off = end;
    }
    fclose(o);
    printf("composed %zu pictures; coop_slice_to_tile refused: top(addr0)=%zu bottom=%zu\n",
           made, refused_top, refused_bot);
    return made ? 0 : 1;
}
