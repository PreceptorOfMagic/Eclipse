/* An unpaired IDR converted to TRAIL_R must keep the previous picture as a reference.
 *
 * The empty-RPS conversion is correct only when the converted slice is slice 1. As slice 0
 * it evicts the previous picture, so a concealment slice spliced beside it has nothing to
 * copy from -- which is why an unpaired IDR fell back to our-slice-alone and left the peer's
 * half uncoded.
 *
 * A wrong RPS is not a wrong VALUE, it is a wrong LENGTH: every field after it shifts, so
 * the header walk lands mid-field. That is what this checks -- parse the converted slice
 * back with the same walker the splicer uses and require the POC to survive. Bits written
 * one short would still "look like" a slice and still decode to garbage.
 *
 * Build: gcc -I../../src/app/stream/video -o /tmp/t test_idr_keepref.c \
 *            ../../src/app/stream/video/coop_poc.c && /tmp/t <stream.h265>
 */
#include <stdio.h>
#include <string.h>

#include "coop_poc.h"

static const unsigned char SC[4] = {0, 0, 0, 1};

static int fail = 0;
static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { fail = 1; }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "skynet-4070-reference.h265";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); return 1; }
    static unsigned char buf[64 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    coop_poc_params_t p;
    if (!coop_poc_params_parse(buf, n, &p) || !p.valid) { printf("FAIL: params\n"); return 1; }

    /* Find an IDR slice (nal type 19/20) to convert. */
    const unsigned char *idr = NULL;
    size_t idr_len = 0;
    size_t pos = 0;
    while (pos + 5 <= n) {
        size_t next = pos + 4;
        while (next + 4 <= n && memcmp(buf + next, SC, 4) != 0) { next++; }
        const size_t end = (next + 4 <= n) ? next : n;
        const unsigned char *nal = buf + pos + 4;
        const size_t nlen = end - pos - 4;
        const int t = (nal[0] >> 1) & 0x3F;
        if ((t == 19 || t == 20) && nlen > 3) { idr = nal; idr_len = nlen; break; }
        pos = next;
        if (next + 4 > n) { break; }
    }
    if (idr == NULL) { printf("FAIL: no IDR slice in %s\n", path); return 1; }
    printf("stream %ux%u CTBs, IDR slice %zu bytes\n", p.w_ctbs, p.h_ctbs, idr_len);

    static unsigned char out_empty[8 << 20], out_keep[8 << 20];
    const unsigned poc = 7;
    size_t ne = coop_slice_idr_to_trail(&p, idr, idr_len, poc, out_empty, sizeof(out_empty));
    size_t nk = coop_slice_idr_to_trail_keepref(&p, idr, idr_len, poc, out_keep, sizeof(out_keep));

    check(ne > 0, "empty-RPS conversion still works (unchanged behaviour)");
    check(nk > 0, "keepref conversion produces a slice");
    if (ne == 0 || nk == 0) { return 1; }

    check((((out_keep[0] >> 1) & 0x3F) == 1), "keepref slice is TRAIL_R (nal type 1)");

    /* The real test: walk it back. A mis-sized RPS shifts every later field. */
    unsigned got = 0;
    check(coop_poc_read(&p, out_keep, nk, &got), "keepref slice's POC field is locatable");
    check(got == poc, "keepref slice reads back the POC it was given");

    coop_slice_info_t si;
    check(coop_slice_info(&p, out_keep, nk, &si), "keepref slice header walks to the end");
    check(si.slice_type == 2, "keepref slice is still an I-slice");

    check(coop_slice_header_end_bit_public(&p, out_keep, nk) > 0,
          "keepref header end bit resolves");

    /* The declaration is the ONLY difference, so it must cost a few bits and no more. */
    check(nk >= ne, "keepref slice is not shorter than the empty-RPS form");
    check(nk - ne <= 4, "keepref costs only the RPS declaration (<=4 bytes)");

    printf("empty-RPS %zu bytes, keepref %zu bytes (+%zu)\n", ne, nk, nk - ne);
    printf(fail ? "\nFAILED\n" : "\nALL PASS\n");
    return fail;
}
