/* test_coop_splice — validate the in-app splicer against real encoder output.
 *
 * Feeds it a capture taken from the live host (/tmp/coop-dump on the TV) and
 * checks two properties:
 *   1. every access unit parses into exactly 2 slices, matching what
 *      hevc_stitch.py reported for the same capture;
 *   2. splicing a stream WITH ITSELF reproduces the original access unit
 *      byte-for-byte -- if slice 0 of A and slice 1 of A do not rebuild A, the
 *      splice is lossy and nothing downstream can be trusted.
 *
 * Build + run:
 *   gcc -O1 -g -o /tmp/test_coop_splice tests/coop/test_coop_splice.c \
 *       src/app/stream/video/coop_splice.c -Isrc/app/stream/video && \
 *       /tmp/test_coop_splice <capture.h265>
 */
#include "coop_splice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) { failures++; }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <capture.h265>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror("open");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t) size);
    if (buf == NULL || fread(buf, 1, (size_t) size, f) != (size_t) size) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    fclose(f);

    /* The capture is a concatenation of access units as fed to the decoder. We
     * cannot split it back into AUs without a parser, so validate the whole
     * buffer as one unit: it must contain slices, and a self-splice of the first
     * two slices must rebuild them exactly. */
    coop_au_t au;
    printf("capture: %s (%ld bytes)\n", argv[1], size);
    check(coop_au_parse(buf, (size_t) size, &au), "parses into an access unit");
    printf("  slices found: %d (slice0=%zu bytes, slice1=%zu bytes, non_vcl=%zu bytes)\n",
           au.slice_count, au.slice_len[0], au.slice_len[1], au.non_vcl_len);
    check(au.slice_count == 2, "exactly 2 slices per picture (matches hevc_stitch.py)");
    check(au.slice[0] != NULL, "slice 0 present (first_slice_segment_in_pic_flag set)");
    check(au.slice[1] != NULL, "slice 1 present");

    if (au.slice_count == 2) {
        size_t cap = (size_t) size + 64;
        unsigned char *out = malloc(cap);
        size_t n = coop_au_splice(&au, &au, out, cap);
        check(n > 0, "self-splice produces output");

        coop_au_t re;
        check(coop_au_parse(out, n, &re), "spliced output re-parses");
        check(re.slice_count == 2, "spliced output still has 2 slices");
        check(re.slice_len[0] == au.slice_len[0] &&
              memcmp(re.slice[0], au.slice[0], au.slice_len[0]) == 0,
              "slice 0 survives the splice byte-for-byte");
        check(re.slice_len[1] == au.slice_len[1] &&
              memcmp(re.slice[1], au.slice[1], au.slice_len[1]) == 0,
              "slice 1 survives the splice byte-for-byte");
        free(out);
    }

    free(buf);
    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
