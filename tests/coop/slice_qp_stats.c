/* WHAT DID THE ENCODER HAVE TO GIVE UP? Average slice QP over a captured stream.
 *
 * Apollo encodes CBR, so a config change that costs efficiency does NOT show up as a
 * bitrate change -- the rate controller spends the same bits either way. It shows up as
 * QUALITY, and the encoder's own statement of quality is the QP it had to choose to hit
 * the target. Higher mean QP at the same bitrate = the change cost something.
 *
 * That is the only way to price constrained encoding on a live host: it restricts inter
 * prediction to the slice region, which is exactly right when slice == pane and severe
 * when slice == one CTB row, and no offline tool can answer it because ffmpeg never sets
 * NVENC's per-picture constrainedFrame flag.
 *
 * Usage: slice_qp_stats <captured.h265>
 */
#include <stdio.h>
#include <string.h>

#include "coop_splice.h"
#include "coop_poc.h"

static const unsigned char SC[4] = {0, 0, 0, 1};

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: slice_qp_stats <captured.h265>\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("FAIL: cannot open %s\n", argv[1]); return 1; }
    static unsigned char buf[192 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    coop_poc_params_t p;
    if (!coop_poc_params_parse(buf, n, &p) || !p.valid) { printf("FAIL: params\n"); return 1; }
    printf("stream: %ux%u CTBs (%ux%u luma), %zu bytes\n",
           p.w_ctbs, p.h_ctbs, p.w_ctbs * 32, p.h_ctbs * 32, n);

    long qsum = 0, qn = 0, qmin = 1000, qmax = -1000;
    long pictures = 0, slices = 0, first_pic_slices = 0;
    long vcl_bytes = 0;
    size_t pos = 0;
    while (pos + 5 <= n) {
        size_t next = pos + 4;
        while (next + 4 <= n && memcmp(buf + next, SC, 4) != 0) { next++; }
        const size_t end = (next + 4 <= n) ? next : n;
        const unsigned char *nal = buf + pos + 4;
        const size_t nlen = end - pos - 4;
        const int t = (nal[0] >> 1) & 0x3F;
        if (t <= 31 && nlen > 3) {
            slices++;
            vcl_bytes += (long) nlen;
            if ((nal[2] >> 7) & 1) { pictures++; }
            if (pictures == 1) { first_pic_slices++; }
            coop_slice_info_t si;
            if (coop_slice_info(&p, nal, nlen, &si)) {
                qsum += si.slice_qp; qn++;
                if (si.slice_qp < qmin) { qmin = si.slice_qp; }
                if (si.slice_qp > qmax) { qmax = si.slice_qp; }
            }
        }
        pos = next;
        if (next + 4 > n) { break; }
    }
    if (qn == 0) { printf("FAIL: no walkable slices\n"); return 1; }
    printf("pictures=%ld  slices=%ld  slices/picture=%.1f  (first picture: %ld)\n",
           pictures, slices, pictures ? (double) slices / pictures : 0.0, first_pic_slices);
    printf("VCL bytes=%ld  mean bytes/picture=%.0f\n",
           vcl_bytes, pictures ? (double) vcl_bytes / pictures : 0.0);
    printf("SLICE QP: mean=%.2f  min=%ld  max=%ld  (n=%ld)\n",
           (double) qsum / (double) qn, qmin, qmax, qn);
    return 0;
}
