/* Round-trip test: take the SPS out of a REAL capture, double it, parse it back.
 * Uses the actual bitstream rather than a synthetic one, because the fields before
 * the height (profile_tier_level in particular) are exactly where a hand-made SPS
 * would differ from what NVENC emits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coop_poc.h"

unsigned sps_height(const unsigned char *n, size_t l);   /* from the test shim */

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no capture\n"); return 1; }
    static unsigned char buf[4 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    /* find the first SPS (nal type 33) in Annex-B */
    for (size_t i = 0; i + 5 < n; ++i) {
        if (buf[i] || buf[i+1] || buf[i+2] != 1) continue;
        int type = (buf[i+3] >> 1) & 0x3F;
        if (type != 33) continue;
        size_t start = i + 3, end = n;
        for (size_t j = start + 4; j + 3 < n; ++j) {
            if (!buf[j] && !buf[j+1] && buf[j+2] == 1) { end = j; break; }
        }
        while (end > start && buf[end-1] == 0) end--;
        size_t len = end - start;
        unsigned h0 = sps_height(buf + start, len);
        static unsigned char out[8192];
        size_t got = coop_sps_double_height(buf + start, len, out, sizeof(out));
        if (!got) { printf("FAIL: rewrite returned 0\n"); return 1; }
        unsigned h1 = sps_height(out, got);
        printf("original height=%u  rewritten height=%u  (%zu -> %zu bytes)\n", h0, h1, len, got);
        if (h1 != h0 * 2) { printf("FAIL: expected %u\n", h0 * 2); return 1; }
        printf("PASS: height doubled and the SPS still parses\n");
        return 0;
    }
    printf("no SPS found\n");
    return 1;
}
