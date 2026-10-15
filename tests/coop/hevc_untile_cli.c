/* OFFLINE PROBE: re-emit a ONE-COLUMN tiled HEVC composite (horizontal co-op's two tile rows) untiled,
 * with coop_au_untile() -- the rewrite the Android build applies for decoders that mishandle tile rows.
 * With one tile column, tile scan is raster scan, so the output must decode to the SAME pictures as the
 * input: tests/android/run_untile_test.sh checks exactly that with FFmpeg.
 *
 * Usage: hevc_untile_cli <in.265> <out.265>
 */
#include "coop_splice.h"
#include "coop_poc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (f == NULL) { return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(sz > 0 ? (size_t) sz : 1);
    *n = fread(b, 1, (size_t) sz, f);
    fclose(f);
    return b;
}

/* An access unit starts at a VPS/SPS/PPS/AUD that follows a picture, or at a first slice. */
static int nal_type(const unsigned char *d, size_t i) { return (d[i] >> 1) & 0x3f; }

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.265> <out.265>\n", argv[0]);
        return 2;
    }
    size_t n = 0;
    unsigned char *d = slurp(argv[1], &n);
    if (d == NULL) { perror(argv[1]); return 1; }
    FILE *out = fopen(argv[2], "wb");
    if (out == NULL) { perror(argv[2]); return 1; }

    /* Split into access units: a new AU begins at a parameter set / AUD after a slice was seen, or at
     * a slice with first_slice_segment_in_pic_flag after a slice was seen. */
    size_t au_start = 0;
    int seen_vcl = 0;
    coop_poc_params_t params;
    memset(&params, 0, sizeof(params));
    static unsigned char buf[1 << 23];
    int made = 0, refused = 0;
    for (size_t i = 0; i + 3 <= n; ++i) {
        size_t sc = 0;
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) sc = 3;
        else if (i + 4 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) sc = 4;
        if (sc == 0 || i + sc >= n) continue;
        const int t = nal_type(d, i + sc);
        const int vcl = t < 32;
        const int first = vcl && i + sc + 2 < n && (d[i + sc + 2] & 0x80);
        const int boundary = seen_vcl && ((!vcl && (t == 32 || t == 33 || t == 34 || t == 35)) || first);
        if (boundary) {
            const size_t got = coop_au_untile(d + au_start, i - au_start, &params, buf, sizeof(buf));
            if (got > 0) { fwrite(buf, 1, got, out); made++; }
            else { fwrite(d + au_start, 1, i - au_start, out); refused++; }
            au_start = i;
            seen_vcl = 0;
        }
        if (vcl) seen_vcl = 1;
        i += sc - 1;
    }
    if (au_start < n) {
        const size_t got = coop_au_untile(d + au_start, n - au_start, &params, buf, sizeof(buf));
        if (got > 0) { fwrite(buf, 1, got, out); made++; }
        else { fwrite(d + au_start, 1, n - au_start, out); refused++; }
    }
    fclose(out);
    printf("untiled %d access units, left %d as they were\n", made, refused);
    return made == 0 || refused > 0;
}
