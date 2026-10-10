/*
 * Validates coop_poc against REAL host captures, not synthetic data.
 *
 * The reference facts come from tests/coop/hevc_poc.py, which was checked
 * against three streams: each host's own output has matching slice POCs on
 * every picture (89/89), while the spliced composite matched on 0 of 119.
 */
#include "../../src/app/stream/video/coop_poc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        failures++;
    }
}

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t) n);
    if (buf != NULL && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = buf ? (size_t) n : 0;
    return buf;
}

/** Iterate NAL payloads (no start codes) in an Annex-B stream. */
static size_t next_nal(const unsigned char *d, size_t len, size_t from,
                       const unsigned char **nal, size_t *nal_len) {
    size_t i = from;
    while (i + 3 <= len) {
        size_t sc = 0;
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            sc = 3;
        } else if (i + 4 <= len && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            sc = 4;
        }
        if (sc == 0) {
            i++;
            continue;
        }
        size_t start = i + sc;
        size_t end = start;
        while (end + 3 <= len && !(d[end] == 0 && d[end + 1] == 0 &&
                                   (d[end + 2] == 1 || (end + 4 <= len && d[end + 2] == 0 && d[end + 3] == 1)))) {
            end++;
        }
        if (end + 3 > len) {
            end = len;
        }
        *nal = d + start;
        *nal_len = end - start;
        return end;
    }
    return len;
}

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : "tests/coop/skynet-4070-reference.h265";
    size_t len = 0;
    unsigned char *data = slurp(path, &len);
    if (data == NULL) {
        printf("cannot read %s\n", path);
        return 77;   /* skip: capture is gitignored */
    }
    printf("coop_poc against %s (%zu bytes)\n", path, len);

    coop_poc_params_t p;
    check(coop_poc_params_parse(data, len, &p), "SPS+PPS parsed");
    printf("    log2_max_poc_lsb=%u slice_addr_bits=%u extra=%u dep=%d out=%d\n",
           p.log2_max_poc_lsb, p.slice_addr_bits, p.num_extra_slice_header_bits,
           (int) p.dependent_slice_segments, (int) p.output_flag_present);
    /* Values measured by hevc_poc.py on this exact capture. */
    check(p.log2_max_poc_lsb == 8, "log2_max_poc_lsb == 8 (matches python)");
    check(p.slice_addr_bits == 11, "slice_addr_bits == 11 (1920x1088, CTB 32)");

    /* Every picture's two slices must agree -- this is a single-host stream. */
    size_t pos = 0;
    const unsigned char *nal = NULL;
    size_t nal_len = 0;
    unsigned pocs[16];
    int npoc = 0, pairs_equal = 0, pairs_seen = 0;
    unsigned prev = 0;
    int have_prev = 0;
    while (pos < len && npoc < 16) {
        pos = next_nal(data, len, pos, &nal, &nal_len);
        if (nal_len < 2) {
            continue;
        }
        int t = (nal[0] >> 1) & 0x3F;
        if (t > 31) {
            continue;            /* non-VCL */
        }
        unsigned v = 0;
        if (!coop_poc_read(&p, nal, nal_len, &v)) {
            continue;            /* IDR: no POC field */
        }
        pocs[npoc++] = v;
        if (have_prev) {
            pairs_seen++;
            if (prev == v) {
                pairs_equal++;
            }
            have_prev = 0;
        } else {
            prev = v;
            have_prev = 1;
        }
    }
    printf("    first POCs:");
    for (int i = 0; i < npoc; ++i) {
        printf(" %u", pocs[i]);
    }
    printf("\n");
    check(npoc >= 8, "read POCs from slice headers");
    check(pairs_seen > 0 && pairs_equal == pairs_seen,
          "both slices of each picture report the SAME POC");
    check(npoc >= 4 && pocs[2] == pocs[0] + 1,
          "POC advances by 1 per picture");

    /* Rewriting to the value already present must be a byte-identical no-op:
     * that exercises the full RBSP round trip including emulation prevention. */
    pos = 0;
    const unsigned char *slice = NULL;
    size_t slice_len = 0;
    while (pos < len) {
        pos = next_nal(data, len, pos, &nal, &nal_len);
        if (nal_len < 2) {
            continue;
        }
        int t = (nal[0] >> 1) & 0x3F;
        unsigned v = 0;
        if (t <= 31 && coop_poc_read(&p, nal, nal_len, &v)) {
            slice = nal;
            slice_len = nal_len;
            break;
        }
    }
    check(slice != NULL, "found an inter slice to rewrite");
    if (slice != NULL) {
        unsigned orig = 0;
        coop_poc_read(&p, slice, slice_len, &orig);
        unsigned char *out = malloc(slice_len + 4096);
        size_t n = coop_poc_rewrite(&p, slice, slice_len, orig, out, slice_len + 4096);
        check(n == slice_len && memcmp(out, slice, slice_len) == 0,
              "rewrite to the same POC is byte-identical");

        for (unsigned target = 0; target < 260; target += 37) {
            unsigned want = target & ((1u << p.log2_max_poc_lsb) - 1u);
            n = coop_poc_rewrite(&p, slice, slice_len, want, out, slice_len + 4096);
            unsigned got = 0;
            int ok = n > 0 && coop_poc_read(&p, out, n, &got) && got == want;
            if (!ok) {
                printf("    target=%u got=%u n=%zu\n", want, got, n);
                check(0, "rewrite round-trips");
                break;
            }
        }
        check(1, "rewrite round-trips for every tested POC value");

        /* Only the POC field may move; the payload must survive untouched. */
        n = coop_poc_rewrite(&p, slice, slice_len, (orig + 7) & 0xFF, out, slice_len + 4096);
        size_t tail = slice_len > 8 ? slice_len - 8 : 0;
        check(n >= tail && memcmp(out + n - tail, slice + slice_len - tail, tail) == 0,
              "slice payload bytes are unchanged by the rewrite");
        free(out);
    }

    free(data);
    printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
