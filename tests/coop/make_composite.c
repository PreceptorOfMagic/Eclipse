/* BUILD A REAL-GEOMETRY CO-OP COMPOSITE, so the TV can be judged at the size it will
 * actually run: two 3840x1080 panes spliced into one 3840x2160 picture.
 *
 * The older demo used skynet-4070-reference.h265, which is 1920x1088 -- a QUARTER of the
 * pixels. Pillarboxed and upscaled onto a 4K panel it looks soft, and that softness is the
 * scaler, not the codec. Any judgement about compression artifacts has to be made at the
 * real geometry.
 *
 * ALL-INTRA ON PURPOSE, and this is not a quality choice. A half-height picture's INTER
 * slices carry motion vectors that were valid when the slice sat at the TOP of a 1080-row
 * picture; re-addressed into the bottom half of a 2176-row one, those vectors read into the
 * OTHER pane. Apollo avoids this with constrained encoding (enableConstrainedEncoding +
 * sliceMode), which ffmpeg does not expose -- so a composite built here from inter slices
 * would show the top pane bleeding into the bottom. Intra slices reference nothing, so the
 * splice is trivially safe and what appears on the panel is only the codec and the scaler.
 *
 * Usage: make_composite <pane.h265> <out.h265> [max_pictures]
 *   pane.h265 = an all-intra 3840x1080 HEVC stream (one slice per picture).
 * Both halves come from the same pane stream, so the two panes show the same content --
 * a second machine is the only way to make them differ.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_splice.h"
#include "coop_poc.h"

static const unsigned char SC[4] = {0, 0, 0, 1};

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: make_composite <pane.h265> <out.h265> [max]\n"); return 1; }
    const int max_pics = (argc > 3) ? atoi(argv[3]) : 32;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("FAIL: cannot open %s\n", argv[1]); return 1; }
    static unsigned char buf[192 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    coop_poc_params_t src;
    if (!coop_poc_params_parse(buf, n, &src) || !src.valid) { printf("FAIL: params\n"); return 1; }

    coop_au_t head;
    if (!coop_au_parse(buf, n, &head) || head.non_vcl == NULL) { printf("FAIL: no param sets\n"); return 1; }
    static unsigned char nv[64 << 10];
    size_t nvlen = coop_nonvcl_double_height(head.non_vcl, head.non_vcl_len, nv, sizeof(nv));
    if (!nvlen) { printf("FAIL: non-vcl rewrite\n"); return 1; }

    coop_poc_params_t comp;
    if (!coop_poc_params_parse(nv, nvlen, &comp) || !comp.valid) { printf("FAIL: composite params\n"); return 1; }
    const unsigned bottom = coop_bottom_half_addr(&comp);
    printf("pane %ux%u CTBs -> composite %ux%u CTBs, bottom-half addr %u\n",
           src.w_ctbs, src.h_ctbs, comp.w_ctbs, comp.h_ctbs, bottom);

    FILE *o = fopen(argv[2], "wb");
    if (!o) { printf("FAIL: cannot write %s\n", argv[2]); return 1; }
    static unsigned char out[32 << 20];

    /* Walk NALs; every VCL NAL is one picture (the pane is encoded one slice per frame). */
    size_t pos = 0, total = 0;
    int pics = 0;
    while (pos + 4 <= n && pics < max_pics) {
        size_t next = pos + 4;
        while (next + 4 <= n && memcmp(buf + next, SC, 4) != 0) { next++; }
        const size_t end = (next + 4 <= n) ? next : n;
        const int t = (int) ((buf[pos + 4] >> 1) & 0x3F);
        if (t <= 31) {
            coop_au_t au;
            if (coop_au_parse(buf + pos, end - pos, &au) && au.slice[0] != NULL && au.slice_len[0] > 0) {
                size_t got = coop_au_splice_halves(&au, &au, &comp, bottom, nv, nvlen,
                                                   out, sizeof(out));
                if (got > 0) {
                    fwrite(out, 1, got, o);
                    total += got;
                    if (pics < 2) { printf("  picture %d: %zu bytes composed\n", pics + 1, got); }
                    pics++;
                } else if (pics == 0) {
                    printf("FAIL: compose returned 0 on the first picture\n");
                    fclose(o);
                    return 1;
                }
            }
        }
        pos = next;
        if (next + 4 > n) { break; }
    }
    fclose(o);
    printf("wrote %s: %d composite pictures, %zu bytes (%zu KB average)\n",
           argv[2], pics, total, pics ? total / pics / 1024 : 0);
    if (pics == 0) { printf("FAIL: no pictures composed\n"); return 1; }
    printf("PASS: decode with ffmpeg to judge -- expect %ux%u\n",
           comp.w_ctbs * 32, comp.h_ctbs * 32);
    return 0;
}
