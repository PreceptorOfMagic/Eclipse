#include "coop_splice.h"
#include "coop_poc.h"

#include <string.h>

#define VCL_MAX 31   /* HEVC nal_unit_type 0..31 are VCL (slice) NALs */

/* Co-op is HEVC-only. Ordinary single-host AVC bypasses this composer. */
static int nal_type(const unsigned char *nal, size_t len) {
    return len == 0 ? -1 : ((nal[0] >> 1) & 0x3F);
}
static bool nal_is_vcl(int t) { return t >= 0 && t <= VCL_MAX; }
static bool first_slice_in_pic(const unsigned char *nal, size_t len) {
    return len > 2 && (nal[2] & 0x80) != 0;
}

/** Find the next Annex-B start code at or after @p from. Returns offset or len. */
static size_t next_start_code(const unsigned char *d, size_t len, size_t from, size_t *sc_len) {
    for (size_t i = from; i + 3 <= len; ++i) {
        if (d[i] == 0 && d[i + 1] == 0) {
            if (d[i + 2] == 1) {
                *sc_len = 3;
                return i;
            }
            if (i + 4 <= len && d[i + 2] == 0 && d[i + 3] == 1) {
                *sc_len = 4;
                return i;
            }
        }
    }
    return len;
}

bool coop_au_parse(const unsigned char *data, size_t len, coop_au_t *out) {
    memset(out, 0, sizeof(*out));
    size_t sc = 0;
    size_t pos = next_start_code(data, len, 0, &sc);
    if (pos >= len) {
        return false;
    }
    const unsigned char *non_vcl_start = data + pos;
    size_t non_vcl_end = pos;

    while (pos < len) {
        size_t payload = pos + sc;
        size_t next_sc = 0;
        size_t next = next_start_code(data, len, payload, &next_sc);
        const unsigned char *nal = data + payload;
        size_t nal_len = next - payload;

        int t = nal_type(nal, nal_len);
        if (nal_is_vcl(t)) {
            /* Slice: index by first_slice_segment_in_pic_flag so ordering is
             * explicit rather than positional. */
            int idx = first_slice_in_pic(nal, nal_len) ? 0 : 1;
            if (out->slice[idx] == NULL) {
                out->slice_type[idx] = t;
                out->slice[idx] = nal;
                out->slice_len[idx] = nal_len;
                out->slice_count++;
            }
        } else if (out->slice_count == 0) {
            non_vcl_end = next;   /* still in the parameter-set prefix */
        }
        pos = next;
        sc = next_sc;
    }

    if (non_vcl_end > (size_t) (non_vcl_start - data)) {
        out->non_vcl = non_vcl_start;
        out->non_vcl_len = non_vcl_end - (size_t) (non_vcl_start - data);
    }
    return out->slice_count > 0;
}

size_t coop_au_splice(const coop_au_t *a, const coop_au_t *b,
                      unsigned char *out, size_t out_cap) {
    if (a->slice[0] == NULL || b->slice[1] == NULL) {
        return 0;   /* need pane 0 from A and pane 1 from B */
    }
    /* All VCL NAL units of one picture must share a nal_unit_type. Splicing a
     * TRAIL_R (1) from A with an IDR_W_RADL (19) from B produced a picture that
     * is half-IDR -- illegal, and observed on the TV as the bottom pane glitching
     * out. The two hosts IDR independently, so this happens constantly; refuse it
     * and let the caller fall back to a single-pane picture. */
    if (a->slice_type[0] != b->slice_type[1]) {
        return 0;
    }
    static const unsigned char sc[4] = {0, 0, 0, 1};
    size_t need = a->non_vcl_len + sizeof(sc) + a->slice_len[0] + sizeof(sc) + b->slice_len[1];
    if (need > out_cap) {
        return 0;
    }
    size_t n = 0;
    if (a->non_vcl_len > 0) {
        memcpy(out + n, a->non_vcl, a->non_vcl_len);   /* already start-code prefixed */
        n += a->non_vcl_len;
    }
    memcpy(out + n, sc, sizeof(sc));
    n += sizeof(sc);
    memcpy(out + n, a->slice[0], a->slice_len[0]);
    n += a->slice_len[0];

    memcpy(out + n, sc, sizeof(sc));
    n += sizeof(sc);
    memcpy(out + n, b->slice[1], b->slice_len[1]);
    n += b->slice_len[1];
    return n;
}


/* ---- half-height composition ---------------------------------------------
 * With each host encoding only its own rows, an access unit holds ONE slice of a
 * 1088-row picture rather than two slices of a 2176-row one. Composing is then:
 * our parameter sets with the SPS height doubled, our slice unchanged (it is already
 * the first slice at address 0), and the peer's slice relocated to the bottom half.
 *
 * This is simpler than the full-height splice it replaces. Note it is NOT a ghosting
 * cure: cross-pane spatial prediction is already prevented by NVENC constrained
 * encoding, and the residual ghosting is temporal desync from dropped panes, which
 * geometry does not address. The gain here is the half-resolution desktop each host
 * actually wants, plus half the pixels to deliver. */

size_t coop_nonvcl_double_height(const unsigned char *in, size_t len,
                                 unsigned char *out, size_t cap) {
    if (in == NULL || len == 0) {
        return 0;
    }
    size_t n = 0, i = 0;
    while (i < len) {
        /* Locate this NAL's start code and the next one, so each NAL is copied or
         * substituted whole. */
        size_t sc = 0;
        if (i + 3 < len && in[i] == 0 && in[i+1] == 0 && in[i+2] == 0 && in[i+3] == 1) {
            sc = 4;
        } else if (i + 2 < len && in[i] == 0 && in[i+1] == 0 && in[i+2] == 1) {
            sc = 3;
        } else {
            return 0;                       /* not a start-code run: refuse rather than guess */
        }
        size_t body = i + sc, next = len;
        for (size_t j = body + 2; j + 2 < len; ++j) {
            if (in[j] == 0 && in[j+1] == 0 && (in[j+2] == 1 ||
                (j + 3 < len && in[j+2] == 0 && in[j+3] == 1))) {
                next = j;
                break;
            }
        }
        const int type = (body < len) ? ((in[body] >> 1) & 0x3F) : -1;
        if (n + sc > cap) {
            return 0;
        }
        memcpy(out + n, in + i, sc);
        n += sc;
        if (type == 33) {                   /* SPS: the one NAL that changes */
            size_t got = coop_sps_double_height(in + body, next - body, out + n, cap - n);
            if (got == 0) {
                return 0;
            }
            n += got;
        } else {
            size_t blen = next - body;
            if (n + blen > cap) {
                return 0;
            }
            memcpy(out + n, in + body, blen);
            n += blen;
        }
        i = next;
    }
    return n;
}

size_t coop_nonvcl_tile_columns(const unsigned char *in, size_t len,
                                unsigned char *out, size_t cap) {
    /* The composite's parameter sets for a VERTICAL split: SPS width doubled AND the PPS
     * given two tile columns. Both NALs change, which is why this is not just the width
     * doubler with an extra step bolted on. */
    if (in == NULL || len == 0) {
        return 0;
    }
    size_t n = 0, i = 0;
    while (i < len) {
        size_t sc = 0;
        if (i + 3 < len && in[i] == 0 && in[i+1] == 0 && in[i+2] == 0 && in[i+3] == 1) {
            sc = 4;
        } else if (i + 2 < len && in[i] == 0 && in[i+1] == 0 && in[i+2] == 1) {
            sc = 3;
        } else {
            return 0;
        }
        size_t body = i + sc, next = len;
        for (size_t j = body + 2; j + 2 < len; ++j) {
            if (in[j] == 0 && in[j+1] == 0 && (in[j+2] == 1 ||
                (j + 3 < len && in[j+2] == 0 && in[j+3] == 1))) {
                next = j;
                break;
            }
        }
        const int type = (body < len) ? ((in[body] >> 1) & 0x3F) : -1;
        if (n + sc > cap) {
            return 0;
        }
        memcpy(out + n, in + i, sc);
        n += sc;
        size_t got = 0;
        if (type == 33) {
            got = coop_sps_double_width(in + body, next - body, out + n, cap - n);
        } else if (type == 34) {
            got = coop_pps_enable_tiles(in + body, next - body, out + n, cap - n);
        }
        if (type == 33 || type == 34) {
            if (got == 0) {
                return 0;
            }
            n += got;
        } else {
            size_t blen = next - body;
            if (n + blen > cap) {
                return 0;
            }
            memcpy(out + n, in + body, blen);
            n += blen;
        }
        i = next;
    }
    return n;
}

size_t coop_nonvcl_tile_rows(const unsigned char *in, size_t len,
                                unsigned char *out, size_t cap) {
    /* The composite's parameter sets for a HORIZONTAL split: SPS height doubled AND the PPS
     * given two tile ROWS. The exact counterpart of the columns builder above.
     *
     * WHY HORIZONTAL IS THE EASY CASE. A full-width tile row's CTU scan is the picture's own
     * raster scan restricted to that row range, so CtbAddrInRs and CtbAddrInTs COINCIDE and a
     * pane's slice data drops in verbatim with its address unchanged -- no per-CTB-row slices,
     * which is what the column split needed before tiles. The pane is 34 CTB rows of the
     * composite's 68, so uniform_spacing_flag = 1 splits it exactly.
     *
     * The point of this is NOT the composition -- the half-height splice already worked. It is
     * that the peer's slice then travels through coop_slice_to_tile(), the address path the
     * VERTICAL mode uses and which is demonstrably correct, instead of the bespoke half-height
     * path that produced both the catch-up readdress bug and (still open) the concealer's
     * inexactness. */
    if (in == NULL || len == 0) {
        return 0;
    }
    size_t n = 0, i = 0;
    while (i < len) {
        size_t sc = 0;
        if (i + 3 < len && in[i] == 0 && in[i+1] == 0 && in[i+2] == 0 && in[i+3] == 1) {
            sc = 4;
        } else if (i + 2 < len && in[i] == 0 && in[i+1] == 0 && in[i+2] == 1) {
            sc = 3;
        } else {
            return 0;
        }
        size_t body = i + sc, next = len;
        for (size_t j = body + 2; j + 2 < len; ++j) {
            if (in[j] == 0 && in[j+1] == 0 && (in[j+2] == 1 ||
                (j + 3 < len && in[j+2] == 0 && in[j+3] == 1))) {
                next = j;
                break;
            }
        }
        const int type = (body < len) ? ((in[body] >> 1) & 0x3F) : -1;
        if (n + sc > cap) {
            return 0;
        }
        memcpy(out + n, in + i, sc);
        n += sc;
        size_t got = 0;
        if (type == 33) {
            got = coop_sps_double_height(in + body, next - body, out + n, cap - n);
        } else if (type == 34) {
            got = coop_pps_enable_tiles_grid(in + body, next - body, 0, 1,
                                             out + n, cap - n);
        }
        if (type == 33 || type == 34) {
            if (got == 0) {
                return 0;
            }
            n += got;
        } else {
            size_t blen = next - body;
            if (n + blen > cap) {
                return 0;
            }
            memcpy(out + n, in + body, blen);
            n += blen;
        }
        i = next;
    }
    return n;
}

static const unsigned char COOP_SC[4] = {0, 0, 0, 1};

size_t coop_au_splice_columns(const coop_pane_pic_t *a, const coop_pane_pic_t *b,
                              const coop_poc_params_t *src,
                              const coop_poc_params_t *composite,
                              const unsigned char *nonvcl, size_t nonvcl_len,
                              unsigned char *out, size_t out_cap) {
    if (a == NULL || b == NULL || src == NULL || composite == NULL || out == NULL) {
        return 0;
    }
    if (!src->valid || !composite->valid || a->n == 0 || a->n != b->n) {
        return 0;
    }
    /* One slice per CTB row is structural, not a tuning knob: composite row r is two
     * contiguous runs, and a slice covering two pane rows spans a gap in the composite. */
    if (a->n != composite->h_ctbs || composite->w_ctbs != src->w_ctbs * 2u) {
        return 0;
    }
    if (composite->h_ctbs > COOP_MAX_ROWS) {
        return 0;
    }

    /* Every VCL NAL of one picture must share nal_unit_type. Decide the picture's type from
     * pane A and make B agree, exactly as the horizontal path does. */
    const int ta = (a->slice_len[0] > 1) ? ((a->slice[0][0] >> 1) & 0x3F) : -1;
    const int tb = (b->slice_len[0] > 1) ? ((b->slice[0][0] >> 1) & 0x3F) : -1;
    const bool a_irap = ta >= 16 && ta <= 23;
    const bool b_idr = tb == 19 || tb == 20;
    bool convert_b = false;
    if (ta != tb) {
        if (ta == 1 && !a_irap && b_idr) {
            convert_b = true;          /* B re-anchored alone; fold its IDR into our TRAIL */
        } else {
            return 0;                  /* A is an IRAP and B is not: not fixable, refuse */
        }
    }

    static unsigned char conv[2u << 20];
    static unsigned char poc[2u << 20];
    static unsigned char re[2u << 20];

    /* THE PEER'S POC MUST BECOME OURS. Pane B is an independent encode with its own POC
     * count, and when B re-anchors alone its count RESTARTS while the composite's carries
     * on. Leave that alone and the decoder reorders B's slices out of the picture -- seen
     * on the panel as the right pane flashing on and off. The horizontal path has always
     * done this; the column splice originally did not. Read ours once, from A's first
     * slice, and stamp every B slice with it. */
    unsigned my_poc = 0;
    const bool have_poc = coop_poc_read(src, a->slice[0], a->slice_len[0], &my_poc);
    size_t n = 0;
    if (nonvcl != NULL && nonvcl_len > 0) {
        if (nonvcl_len > out_cap) {
            return 0;
        }
        memcpy(out, nonvcl, nonvcl_len);
        n = nonvcl_len;
    }

    for (unsigned r = 0; r < a->n; ++r) {
        const unsigned addr_a = r * composite->w_ctbs;
        const unsigned addr_b = addr_a + src->w_ctbs;
        for (int side = 0; side < 2; ++side) {
            const unsigned char *sl = side ? b->slice[r] : a->slice[r];
            size_t sl_len = side ? b->slice_len[r] : a->slice_len[r];
            const unsigned addr = side ? addr_b : addr_a;
            if (sl == NULL || sl_len < 3) {
                return 0;
            }
            if (side && convert_b) {
                /* SRC, not composite: this walks a PANE slice, whose address field is sized
                 * by the pane (12 bits vs the composite's 13). Walking it with the composite
                 * parameters misreads by one bit from the address onward and the conversion
                 * silently refuses. Re-addressing to the composite happens below, after. */
                size_t cn = coop_slice_idr_to_trail(src, sl, sl_len, 0,
                                                    conv, sizeof(conv));
                if (cn == 0) {
                    return 0;
                }
                sl = conv;
                sl_len = cn;
            }
            if (side && have_poc) {
                /* src params: the slice is still pane-addressed at this point. Re-addressing
                 * to the composite happens below, after. */
                size_t pn = coop_poc_rewrite(src, sl, sl_len, my_poc, poc, sizeof(poc));
                if (pn > 0) {
                    sl = poc;
                    sl_len = pn;
                }
            }
            const unsigned char *emit = sl;
            size_t emit_len = sl_len;
            const int first = (sl[2] >> 7) & 1;
            if (side == 0 && r == 0 && first) {
                /* A's first row IS the composite's first slice: address 0, no field. */
            } else if (first) {
                emit_len = coop_slice_set_address(composite, sl, sl_len, addr, re, sizeof(re));
                emit = re;
            } else {
                /* Already carries an address, but sized by the PANE -- it must be REPLACED
                 * at the composite's width, which moves every following bit. */
                emit_len = coop_slice_readdress(src, composite, sl, sl_len, addr,
                                                re, sizeof(re));
                emit = re;
            }
            if (emit_len == 0 || n + 4 + emit_len > out_cap) {
                return 0;
            }
            memcpy(out + n, COOP_SC, 4);
            n += 4;
            memcpy(out + n, emit, emit_len);
            n += emit_len;
        }
    }
    return n;
}

size_t coop_nonvcl_double_width(const unsigned char *in, size_t len,
                                unsigned char *out, size_t cap) {
    /* Same NAL walk as coop_nonvcl_double_height; only the SPS substitution differs. */
    if (in == NULL || len == 0) {
        return 0;
    }
    size_t n = 0, i = 0;
    while (i < len) {
        size_t sc = 0;
        if (i + 3 < len && in[i] == 0 && in[i+1] == 0 && in[i+2] == 0 && in[i+3] == 1) {
            sc = 4;
        } else if (i + 2 < len && in[i] == 0 && in[i+1] == 0 && in[i+2] == 1) {
            sc = 3;
        } else {
            return 0;
        }
        size_t body = i + sc, next = len;
        for (size_t j = body + 2; j + 2 < len; ++j) {
            if (in[j] == 0 && in[j+1] == 0 && (in[j+2] == 1 ||
                (j + 3 < len && in[j+2] == 0 && in[j+3] == 1))) {
                next = j;
                break;
            }
        }
        const int type = (body < len) ? ((in[body] >> 1) & 0x3F) : -1;
        if (n + sc > cap) {
            return 0;
        }
        memcpy(out + n, in + i, sc);
        n += sc;
        if (type == 33) {
            size_t got = coop_sps_double_width(in + body, next - body, out + n, cap - n);
            if (got == 0) {
                return 0;
            }
            n += got;
        } else {
            size_t blen = next - body;
            if (n + blen > cap) {
                return 0;
            }
            memcpy(out + n, in + body, blen);
            n += blen;
        }
        i = next;
    }
    return n;
}

/* HORIZONTAL, TILED: both halves re-homed with coop_slice_to_tile().
 *
 * BOTH slices must go through it, not just the peer's. Enabling tiles adds
 * num_entry_point_offsets to EVERY slice header, and our own pane was encoded with tiles OFF --
 * so passing our slice through unchanged (as the untiled splice does, since ours is already
 * first at address 0) yields a header the composite's PPS says is one field short. That decodes
 * as garbage with no error, which is this feature's whole failure signature.
 * coop_slice_to_tile handles addr==0 by keeping first_slice_segment_in_pic_flag set and still
 * writing the entry-point field, so the same call serves both halves.
 *
 * Each half is walked with the parameters IT was encoded against and written against the
 * composite's -- @p a_src for ours, @p b_src for the peer's.
 */
size_t coop_au_splice_tile_rows(const coop_au_t *a, const coop_au_t *b,
                                const coop_poc_params_t *a_src,
                                const coop_poc_params_t *b_src,
                                const coop_poc_params_t *composite, unsigned peer_addr,
                                const unsigned char *nonvcl, size_t nonvcl_len,
                                unsigned char *out, size_t cap) {
    if (a == NULL || b == NULL || a->slice[0] == NULL || b->slice[0] == NULL ||
        composite == NULL || a_src == NULL || b_src == NULL) {
        return 0;
    }
    static const unsigned char sc[4] = {0, 0, 0, 1};
    size_t n = 0;
    if (nonvcl != NULL && nonvcl_len > 0) {
        if (n + nonvcl_len > cap) { return 0; }
        memcpy(out + n, nonvcl, nonvcl_len);
        n += nonvcl_len;
    }
    if (n + sizeof(sc) > cap) { return 0; }
    memcpy(out + n, sc, sizeof(sc));
    n += sizeof(sc);
    size_t got = coop_slice_to_tile(a_src, composite, a->slice[0], a->slice_len[0], 0,
                                    out + n, cap - n);
    if (got == 0) { return 0; }
    n += got;

    if (n + sizeof(sc) > cap) { return 0; }
    memcpy(out + n, sc, sizeof(sc));
    n += sizeof(sc);
    got = coop_slice_to_tile(b_src, composite, b->slice[0], b->slice_len[0], peer_addr,
                             out + n, cap - n);
    if (got == 0) { return 0; }        /* refuse rather than emit a bad picture */
    return n + got;
}

size_t coop_au_splice_halves(const coop_au_t *a, const coop_au_t *b,
                             const coop_poc_params_t *composite, unsigned peer_addr,
                             const unsigned char *nonvcl, size_t nonvcl_len,
                             unsigned char *out, size_t cap) {
    if (a->slice[0] == NULL || b->slice[0] == NULL || composite == NULL) {
        return 0;
    }
    static const unsigned char sc[4] = {0, 0, 0, 1};
    size_t n = 0;
    if (nonvcl != NULL && nonvcl_len > 0) {
        if (n + nonvcl_len > cap) {
            return 0;
        }
        memcpy(out + n, nonvcl, nonvcl_len);   /* already start-code prefixed */
        n += nonvcl_len;
    }
    if (n + sizeof(sc) + a->slice_len[0] > cap) {
        return 0;
    }
    memcpy(out + n, sc, sizeof(sc));
    n += sizeof(sc);
    memcpy(out + n, a->slice[0], a->slice_len[0]);   /* ours: already first slice at 0 */
    n += a->slice_len[0];

    if (n + sizeof(sc) > cap) {
        return 0;
    }
    memcpy(out + n, sc, sizeof(sc));
    n += sizeof(sc);
    size_t got = coop_slice_set_address(composite, b->slice[0], b->slice_len[0],
                                        peer_addr, out + n, cap - n);
    if (got == 0) {
        return 0;                            /* refuse rather than emit a bad picture */
    }
    return n + got;
}

size_t coop_au_untile(const unsigned char *au, size_t len, coop_poc_params_t *params,
                      unsigned char *out, size_t out_cap) {
    /* A ONE-COLUMN tiled composite (horizontal co-op's two tile rows), re-emitted untiled: the PPS
     * through coop_pps_disable_tiles, every slice through coop_slice_untile, everything else as is.
     * With one tile column, tile scan is raster scan, so this is the same picture -- for decoders
     * that decode a second tile row as garbage (the SHIELD TV's Tegra, 2026-10-15).
     *
     * @p params caches the TILED parameters across access units, refreshed from any AU that
     * carries an SPS and PPS. Returns 0 -- the caller then feeds the original -- when there are no
     * parameters yet, or when any NAL is refused: a picture is converted whole or not at all. */
    if (au == NULL || params == NULL || out == NULL) {
        return 0;
    }
    coop_poc_params_t fresh;
    if (coop_poc_params_parse(au, len, &fresh) && fresh.valid) {
        *params = fresh;
    }
    if (!params->valid || !params->tiles_enabled) {
        return 0;
    }
    static unsigned char nalbuf[4u * 1024u * 1024u + 1024u];
    size_t w = 0, i = 0;
    while (i + 3 <= len) {
        size_t sc = 0;
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            sc = 3;
        } else if (i + 4 <= len && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) {
            sc = 4;
        }
        if (sc == 0) {
            ++i;
            continue;
        }
        const size_t start = i + sc;
        size_t end = start;
        while (end + 3 <= len && !(au[end] == 0 && au[end + 1] == 0 && (au[end + 2] == 1 ||
               (end + 4 <= len && au[end + 2] == 0 && au[end + 3] == 1)))) {
            ++end;
        }
        if (end + 3 > len) {
            end = len;
        }
        const unsigned char *nal = au + start;
        const size_t nlen = end - start;
        const int type = nlen > 0 ? (nal[0] >> 1) & 0x3f : -1;
        size_t got = 0;
        const unsigned char *src = nal;
        size_t src_len = nlen;
        if (type == 34) {
            got = coop_pps_disable_tiles(nal, nlen, nalbuf, sizeof(nalbuf));
            if (got == 0) {
                return 0;
            }
            src = nalbuf;
            src_len = got;
        } else if (type >= 0 && type < 32) {
            got = coop_slice_untile(params, nal, nlen, nalbuf, sizeof(nalbuf));
            if (got == 0) {
                return 0;
            }
            src = nalbuf;
            src_len = got;
        }
        if (w + 4 + src_len > out_cap) {
            return 0;
        }
        out[w++] = 0;
        out[w++] = 0;
        out[w++] = 0;
        out[w++] = 1;
        memcpy(out + w, src, src_len);
        w += src_len;
        i = end;
    }
    return w;
}
