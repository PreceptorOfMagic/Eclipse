/* OFFLINE PROBE: cabac_zero_words in a co-op composite.
 *
 *   hevc_zerowords_cli add <n> <in.265> <out.265>   append n cabac_zero_words to the FIRST slice of every picture,
 *                                                   as a composite whose first slice was a host's last has them
 *   hevc_zerowords_cli strip <in.265> <out.265>     run every access unit through coop_au_strip_zero_words(), as
 *                                                   the Android build feeds a composite
 *
 * tests/android/run_zerowords_test.sh checks that the stripped stream decodes to exactly the same pictures and
 * that no slice followed by another still ends in padding. */
#include "coop_splice.h"

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

static unsigned char outbuf[1 << 24];

int main(int argc, char **argv) {
    const int add = argc == 5 && strcmp(argv[1], "add") == 0;
    const int strip = argc == 4 && strcmp(argv[1], "strip") == 0;
    if (!add && !strip) {
        fprintf(stderr, "usage: %s add <n> <in.265> <out.265> | strip <in.265> <out.265>\n", argv[0]);
        return 2;
    }
    const int n_words = add ? atoi(argv[2]) : 0;
    size_t n = 0;
    unsigned char *d = slurp(argv[add ? 3 : 2], &n);
    if (d == NULL) { perror("input"); return 1; }
    FILE *out = fopen(argv[add ? 4 : 3], "wb");
    if (out == NULL) { perror("output"); return 1; }

    /* Access units: a new one begins at a VPS/SPS/PPS/AUD after a slice, or at a first slice after a slice. */
    size_t au_start = 0, removed_total = 0;
    int seen_vcl = 0, aus = 0, changed = 0;
    for (size_t i = 0; i <= n; ++i) {
        size_t sc = 0;
        if (i + 3 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) sc = 3;
        else if (i + 4 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) sc = 4;
        int boundary = i == n;
        int vcl = 0;
        if (sc != 0 && i + sc + 2 < n) {
            const int t = (d[i + sc] >> 1) & 0x3f;
            vcl = t < 32;
            const int first = vcl && (d[i + sc + 2] & 0x80);
            boundary = seen_vcl && ((t >= 32 && t <= 35) || first);
        } else if (i != n) {
            continue;
        }
        if (boundary && i > au_start) {
            const unsigned char *au = d + au_start;
            const size_t au_len = i - au_start;
            aus++;
            if (strip) {
                unsigned removed = 0;
                const size_t got = coop_au_strip_zero_words(au, au_len, outbuf, sizeof(outbuf), &removed);
                if (got > 0) { fwrite(outbuf, 1, got, out); changed++; removed_total += removed; }
                else { fwrite(au, 1, au_len, out); }
            } else {
                /* The words go right after the first slice NAL: just before the second slice's start code. */
                size_t second = 0;
                int vcl_seen = 0;
                for (size_t j = 0; j + 3 < au_len; ++j) {
                    if (au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1) {
                        if (((au[j + 3] >> 1) & 0x3f) < 32) {
                            if (vcl_seen) { second = j; break; }
                            vcl_seen = 1;
                        }
                        j += 2;
                    }
                }
                if (second == 0) { fwrite(au, 1, au_len, out); }
                else {
                    size_t e = second;
                    while (e > 0 && au[e - 1] == 0) { e--; }
                    fwrite(au, 1, e, out);
                    for (int k = 0; k < n_words; ++k) { fwrite("\0\0\3", 1, 3, out); }
                    fwrite(au + e, 1, au_len - e, out);
                    changed++;
                }
            }
            au_start = i;
            seen_vcl = 0;
        }
        if (i == n) break;
        if (vcl) seen_vcl = 1;
        i += sc - 1;
    }
    fclose(out);
    if (strip) {
        printf("%d access units, %d stripped, %zu cabac_zero_words removed\n", aus, changed, removed_total);
    } else {
        printf("%d access units, %d padded with %d cabac_zero_words after the first slice\n", aus, changed, n_words);
    }
    return changed == 0;
}
