/* Syntax regression, not a decoder/performance test. Deliberately opaque CABAC
 * payloads let the test check that changing an Exp-Golomb ID preserves the tail. */
#include "coop_avc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned char d[256]; unsigned bit; } bits_t;
static void put(bits_t *b, unsigned v, unsigned n) {
    for (unsigned i = n; i; --i, ++b->bit)
        b->d[b->bit / 8] |= ((v >> (i - 1)) & 1u) << (7 - b->bit % 8);
}
static void ue(bits_t *b, unsigned v) {
    unsigned n = 0, x = v + 1;
    for (unsigned a = x; a > 1; a >>= 1) ++n;
    put(b, 0, n); put(b, x, n + 1);
}
static unsigned get(bits_t *b, unsigned n) {
    unsigned v = 0;
    while (n--) { v = (v << 1) | ((b->d[b->bit / 8] >> (7 - b->bit % 8)) & 1); ++b->bit; }
    return v;
}
static unsigned read_ue(bits_t *b) {
    unsigned n = 0;
    while (!get(b, 1)) { assert(n < 31); ++n; }
    return ((1u << n) - 1) + get(b, n);
}
static const unsigned char tail[] = {0x9f, 0x7a, 0xe1, 0xb8};
static size_t make_slice(unsigned char *au, unsigned id, unsigned fn_bits, bool idr) {
    bits_t b = {0};
    ue(&b, 0); ue(&b, idr ? 2 : 0); ue(&b, 0); put(&b, idr ? 0 : 1, fn_bits);
    if (idr) { ue(&b, id); put(&b, 0, 2); } /* id, IDR marking flags */
    else { put(&b, 0, 3); ue(&b, 0); } /* ref override, list modification, marking, CABAC init */
    ue(&b, 0); ue(&b, 1); /* slice_qp_delta = 0; deblock disabled */
    while (b.bit % 8) put(&b, 1, 1);
    memcpy(b.d + b.bit / 8, tail, sizeof(tail));
    size_t n = b.bit / 8 + sizeof(tail), w = 5;
    memcpy(au, "\0\0\0\1", 4); au[4] = idr ? 0x65 : 0x61;
    unsigned zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros == 2 && b.d[i] <= 3) { au[w++] = 3; zeros = 0; }
        au[w++] = b.d[i]; zeros = b.d[i] ? 0 : zeros + 1;
    }
    return w;
}
static void inspect(const unsigned char *nal, size_t n, unsigned first, unsigned id, bool idr) {
    assert(n > 1 && (nal[0] & 31) == (idr ? 5 : 1));
    bits_t b = {0}; unsigned z = 0, w = 0;
    for (size_t i = 1; i < n; ++i) {
        if (z == 2 && nal[i] == 3) { z = 0; continue; }
        b.d[w++] = nal[i]; z = nal[i] ? 0 : z + 1;
    }
    assert(read_ue(&b) == first);
    unsigned st = read_ue(&b) % 5;
    assert(read_ue(&b) == 0); /* PPS */
    assert(get(&b, 8) == (idr ? 0 : 1));
    if (idr) assert(read_ue(&b) == id);
    if (st == 0) { assert(get(&b, 1) == 0); assert(get(&b, 1) == 0); }
    assert(get(&b, idr ? 2 : 1) == 0);
    if (st != 2) assert(read_ue(&b) == 0);
    assert(read_ue(&b) == 0); /* zero QP delta */
    assert(read_ue(&b) == 1); /* original deblock disable remains */
    while (b.bit % 8) assert(get(&b, 1) == 1);
    assert(w - b.bit / 8 == sizeof(tail));
    assert(memcmp(b.d + b.bit / 8, tail, sizeof(tail)) == 0);
}
static void run(unsigned prim_id, unsigned peer_id, bool peer_top, bool renumber, bool prim_idr) {
    coop_avc_state_t s = {0};
    s.src_sps = (coop_avc_sps_t){.valid=true, .width_mbs=2, .height_mbs=2,
        .frame_mbs_only=true, .poc_type=2, .log2_max_frame_num=8, .max_num_ref_frames=1};
    s.peer_sps = s.src_sps; s.peer_sps.log2_max_frame_num = 7;
    s.comp_sps = s.src_sps; s.comp_sps.height_mbs = 4;
    s.src_pps = (coop_avc_pps_t){.valid=true, .entropy_cabac=true, .deblock_present=true, .pic_init_qp=26};
    s.peer_pps = s.comp_pps = s.src_pps;
    s.ready = true; s.renumber = renumber; s.comp_fn = 1; s.peer_top = peer_top;
    s.peer_first_mb = peer_top ? 0 : 4; s.mine_first_mb = peer_top ? 4 : 0;
    unsigned char a[256], b[256], out[1024];
    size_t an = make_slice(a, prim_id, 8, prim_idr), bn = make_slice(b, peer_id, 7, true);
    size_t n = coop_avc_compose(a, an, b, bn, out, sizeof out, &s);
    assert(n > 0);
    size_t off = 0; unsigned count = 0;
    while (off < n) {
        assert(!memcmp(out + off, "\0\0\0\1", 4));
        size_t end = off + 5;
        while (end + 4 <= n && memcmp(out + end, "\0\0\0\1", 4)) ++end;
        if (end + 4 > n) end = n;
        inspect(out + off + 4, end - off - 4, count * 4, prim_id, prim_idr);
        off = end; ++count;
    }
    assert(count == 2);
}
int main(void) {
    const unsigned ids[][2] = {{0,2}, {1,0}, {37,2}, {65535,1}, {2,65535}};
    for (unsigned p=0; p<2; ++p) for (unsigned r=0; r<2; ++r)
        for (unsigned k=0; k<sizeof(ids)/sizeof(ids[0]); ++k) {
            run(ids[k][0], ids[k][1], p, r, true);
            run(ids[k][0], ids[k][1], p, r, false);
        }
    puts("AVC picture identity and CABAC tail: PASS (both pane orders, IDR progression, IDR-to-I conversion)");
}
