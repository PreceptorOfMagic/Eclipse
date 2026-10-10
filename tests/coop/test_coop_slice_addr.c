/* Round trip for coop_slice_set_address(): take a REAL first-slice from a capture,
 * place it at the composite's bottom-half address, and read the address back with an
 * independent parser. Catches the trap that matters most here -- slice_addr_bits must
 * come from the COMPOSITE sps (13 bits), not the source's (12). */
#include <stdio.h>
#include <string.h>
#include "coop_poc.h"

unsigned slice_addr_of(const unsigned char *nal, size_t len, int irap,
                       int dependent_enabled, unsigned addr_bits, unsigned *first_out);

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no capture\n"); return 1; }
    static unsigned char buf[8 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    coop_poc_params_t p;
    if (!coop_poc_params_parse(buf, n, &p) || !p.valid) { printf("FAIL: no params\n"); return 1; }
    printf("source slice_addr_bits=%u (from the 1088-row SPS)\n", p.slice_addr_bits);
    /* The composite is twice as tall, so PicSizeInCtbsY doubles and the address widens. */
    coop_poc_params_t comp = p;
    comp.slice_addr_bits = p.slice_addr_bits + 1;
    const unsigned ADDR = 4080;   /* 34 CTU rows x 120 CTBs = start of the bottom half */

    for (size_t i = 0; i + 5 < n; ++i) {
        if (buf[i] || buf[i+1] || buf[i+2] != 1) continue;
        int type = (buf[i+3] >> 1) & 0x3F;
        if (type > 31) continue;                      /* VCL only */
        size_t start = i + 3, end = n;
        for (size_t j = start + 4; j + 3 < n; ++j)
            if (!buf[j] && !buf[j+1] && buf[j+2] == 1) { end = j; break; }
        while (end > start && buf[end-1] == 0) end--;
        size_t len = end - start;

        unsigned was_first = 0;
        slice_addr_of(buf + start, len, type >= 16 && type <= 23, p.dependent_slice_segments,
                      p.slice_addr_bits, &was_first);
        if (!was_first) continue;                     /* want a first-slice to convert */

        static unsigned char out[8 << 20];
        size_t got = coop_slice_set_address(&comp, buf + start, len, ADDR, out, sizeof(out));
        if (!got) { printf("FAIL: rewrite returned 0\n"); return 1; }

        unsigned first_after = 9;
        unsigned addr = slice_addr_of(out, got, type >= 16 && type <= 23,
                                      comp.dependent_slice_segments, comp.slice_addr_bits,
                                      &first_after);
        printf("nal type=%d  %zu -> %zu bytes  first_flag %u -> %u  address=%u\n",
               type, len, got, was_first, first_after, addr);
        if (first_after != 0 || addr != ADDR) {
            printf("FAIL: expected first_flag=0 address=%u\n", ADDR);
            return 1;
        }
        printf("PASS: first-slice converted to bottom-half slice at %u\n", ADDR);
        return 0;
    }
    printf("no first-slice VCL NAL found\n");
    return 1;
}
