#include "coop_poc.h"

#include <string.h>

#define NAL_SPS 33
#define NAL_PPS 34

typedef struct {
    const unsigned char *d;
    size_t len;
    size_t pos;      /* bit position */
    bool overrun;
} bits_t;

static void bits_init(bits_t *b, const unsigned char *d, size_t len) {
    b->d = d;
    b->len = len;
    b->pos = 0;
    b->overrun = false;
}

static unsigned bits_u(bits_t *b, unsigned n) {
    unsigned v = 0;
    for (unsigned i = 0; i < n; ++i) {
        if ((b->pos >> 3) >= b->len) {
            b->overrun = true;
            return v;
        }
        unsigned bit = (b->d[b->pos >> 3] >> (7u - (b->pos & 7u))) & 1u;
        v = (v << 1) | bit;
        b->pos++;
    }
    return v;
}

static void skip_scaling_list(bits_t *b);
static int bits_se(bits_t *b);

static unsigned bits_ue(bits_t *b) {
    unsigned lz = 0;
    while (bits_u(b, 1) == 0) {
        if (++lz > 32 || b->overrun) {
            b->overrun = true;
            return 0;
        }
    }
    return (1u << lz) - 1u + (lz ? bits_u(b, lz) : 0u);
}

/** Signed exp-Golomb: k maps to (-1)^(k+1) * ceil(k/2). */
static int bits_se(bits_t *b) {
    unsigned k = bits_ue(b);
    return (k & 1u) ? (int) ((k + 1u) / 2u) : -(int) (k / 2u);
}

/** scaling_list_data(): skipped wholesale -- nothing after it depends on its contents. */
static void skip_scaling_list(bits_t *b) {
    for (unsigned sid = 0; sid < 4; ++sid) {
        for (unsigned mid = 0; mid < 6; mid += (sid == 3) ? 3 : 1) {
            if (!bits_u(b, 1)) {                 /* scaling_list_pred_mode_flag */
                bits_ue(b);                      /* pred_matrix_id_delta */
            } else {
                unsigned n = (sid < 2) ? 16u : 64u;
                if (sid > 1) {
                    bits_se(b);                  /* scaling_list_dc_coef_minus8 */
                }
                for (unsigned i = 0; i < n; ++i) {
                    bits_se(b);
                }
            }
        }
    }
}

/** Strip emulation-prevention bytes (00 00 03 -> 00 00). */
static size_t to_rbsp(const unsigned char *in, size_t len, unsigned char *out, size_t cap) {
    size_t n = 0;
    int zeros = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = in[i];
        if (zeros >= 2 && c == 3) {
            zeros = 0;
            continue;
        }
        if (n >= cap) {
            return 0;
        }
        out[n++] = c;
        zeros = (c == 0) ? zeros + 1 : 0;
    }
    return n;
}

/** Re-insert emulation-prevention bytes. */
static size_t to_ebsp(const unsigned char *in, size_t len, unsigned char *out, size_t cap) {
    size_t n = 0;
    int zeros = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = in[i];
        if (zeros >= 2 && c <= 3) {
            if (n >= cap) { return 0; }
            out[n++] = 3;
            zeros = 0;
        }
        if (n >= cap) { return 0; }
        out[n++] = c;
        zeros = (c == 0) ? zeros + 1 : 0;
    }
    return n;
}

static int nal_type_of(const unsigned char *nal, size_t len) {
    return len >= 2 ? (int) ((nal[0] >> 1) & 0x3F) : -1;
}

static bool is_irap(int t) { return t >= 16 && t <= 23; }

static bool is_idr(int t) { return t == 19 || t == 20; }

static void skip_ptl(bits_t *b, unsigned max_sub_layers_minus1) {
    bits_u(b, 2 + 1 + 5);
    bits_u(b, 32);
    bits_u(b, 4);
    bits_u(b, 32);   /* 43 reserved bits, split to stay within 32-bit reads */
    bits_u(b, 11);
    bits_u(b, 1);
    bits_u(b, 8);    /* general_level_idc */
    unsigned sub_profile[8] = {0}, sub_level[8] = {0};
    for (unsigned i = 0; i < max_sub_layers_minus1 && i < 8; ++i) {
        sub_profile[i] = bits_u(b, 1);
        sub_level[i] = bits_u(b, 1);
    }
    if (max_sub_layers_minus1 > 0) {
        for (unsigned i = max_sub_layers_minus1; i < 8; ++i) {
            bits_u(b, 2);
        }
    }
    for (unsigned i = 0; i < max_sub_layers_minus1 && i < 8; ++i) {
        if (sub_profile[i]) {
            bits_u(b, 2 + 1 + 5);
            bits_u(b, 32);
            bits_u(b, 4);
            bits_u(b, 32);
            bits_u(b, 11);
            bits_u(b, 1);
        }
        if (sub_level[i]) {
            bits_u(b, 8);
        }
    }
}

/* st_ref_pic_set(): the one variable-length structure the slice header shares with the
 * SPS. num_delta_pocs of the REFERENCED set is needed when a set predicts from another,
 * so the running counts are kept per index. */
static unsigned g_num_delta_pocs[65];

/* What the set says about its NEAREST negative reference. Filled only when the caller asks;
 * the bit consumption is identical either way, so this cannot change how anything parses. */
typedef struct {
    unsigned first_neg_delta;   /* 1 == POC-1; 0 == no negative refs */
    unsigned first_neg_used;
    unsigned unresolved;        /* inter-predicted set: the delta is not derivable here */
} rps_capture_t;

static void skip_st_ref_pic_set(bits_t *b, unsigned idx, unsigned num_sets,
                                const coop_poc_params_t *p, rps_capture_t *cap) {
    (void) p;
    if (cap != NULL) {
        cap->first_neg_delta = 0;
        cap->first_neg_used = 0;
        cap->unresolved = 0;
    }
    unsigned inter_pred = 0;
    if (idx != 0) {
        inter_pred = bits_u(b, 1);
    }
    if (inter_pred) {
        if (cap != NULL) {
            cap->unresolved = 1;
        }
        if (idx == num_sets) {
            bits_ue(b);                  /* delta_idx_minus1 */
        }
        bits_u(b, 1);                    /* delta_rps_sign */
        bits_ue(b);                      /* abs_delta_rps_minus1 */
        unsigned ref = (idx > 0) ? idx - 1 : 0;
        unsigned n = (ref < 65) ? g_num_delta_pocs[ref] : 0;
        unsigned count = 0;
        for (unsigned j = 0; j <= n; ++j) {
            unsigned used = bits_u(b, 1);
            if (!used) {
                if (!bits_u(b, 1)) {     /* use_delta_flag */
                    continue;
                }
            }
            count++;
        }
        if (idx < 65) {
            g_num_delta_pocs[idx] = count;
        }
    } else {
        unsigned neg = bits_ue(b);
        unsigned pos = bits_ue(b);
        for (unsigned i = 0; i < neg; ++i) {
            unsigned d = bits_ue(b);            /* delta_poc_s0_minus1[i] */
            unsigned u = bits_u(b, 1);          /* used_by_curr_pic_s0_flag[i] */
            /* Entry 0 is the NEAREST negative reference: DeltaPocS0[0] = -(d+1). Later
             * entries are cumulative deltas from it, so only [0] answers "does this slice
             * predict from the immediately preceding picture". */
            if (i == 0 && cap != NULL) {
                cap->first_neg_delta = d + 1u;
                cap->first_neg_used = u;
            }
        }
        for (unsigned i = 0; i < pos; ++i) { bits_ue(b); bits_u(b, 1); }
        if (idx < 65) {
            g_num_delta_pocs[idx] = neg + pos;
        }
    }
}

static bool parse_sps(const unsigned char *nal, size_t len, coop_poc_params_t *out) {
    unsigned char rb[512];
    size_t n = to_rbsp(nal, len < sizeof(rb) ? len : sizeof(rb), rb, sizeof(rb));
    if (n <= 2) {
        return false;
    }
    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    bits_u(&b, 4);                       /* sps_video_parameter_set_id */
    unsigned max_sub = bits_u(&b, 3);
    bits_u(&b, 1);                       /* sps_temporal_id_nesting_flag */
    skip_ptl(&b, max_sub);
    bits_ue(&b);                         /* sps_seq_parameter_set_id */
    unsigned chroma = bits_ue(&b);
    out->separate_colour_plane = (chroma == 3) ? (bits_u(&b, 1) != 0) : false;
    unsigned width = bits_ue(&b);
    unsigned height = bits_ue(&b);
    if (bits_u(&b, 1)) {                 /* conformance_window_flag */
        bits_ue(&b); bits_ue(&b); bits_ue(&b); bits_ue(&b);
    }
    bits_ue(&b);                         /* bit_depth_luma_minus8 */
    bits_ue(&b);                         /* bit_depth_chroma_minus8 */
    out->log2_max_poc_lsb = bits_ue(&b) + 4;
    unsigned ordering = bits_u(&b, 1);
    for (unsigned i = ordering ? 0 : max_sub; i <= max_sub; ++i) {
        bits_ue(&b); bits_ue(&b); bits_ue(&b);
    }
    unsigned log2_min_cb = bits_ue(&b) + 3;
    unsigned log2_diff = bits_ue(&b);
    unsigned ctb_log2 = log2_min_cb + log2_diff;
    /* Continue past the geometry so the slice header's gating flags are available. The
     * fields between here and sample_adaptive_offset are fixed-order, so they can be
     * stepped over rather than interpreted. */
    bits_ue(&b);                         /* log2_min_transform_block_size_minus2 */
    bits_ue(&b);                         /* log2_diff_max_min_transform_block_size */
    bits_ue(&b);                         /* max_transform_hierarchy_depth_inter */
    bits_ue(&b);                         /* max_transform_hierarchy_depth_intra */
    if (bits_u(&b, 1)) {                 /* scaling_list_enabled_flag */
        if (bits_u(&b, 1)) {             /* sps_scaling_list_data_present_flag */
            skip_scaling_list(&b);
        }
    }
    bits_u(&b, 1);                       /* amp_enabled_flag */
    out->sao_enabled = bits_u(&b, 1) != 0;
    if (bits_u(&b, 1)) {                 /* pcm_enabled_flag */
        bits_u(&b, 4); bits_u(&b, 4);
        bits_ue(&b); bits_ue(&b);
        bits_u(&b, 1);                   /* pcm_loop_filter_disabled_flag */
    }
    out->num_short_term_ref_pic_sets = bits_ue(&b);
    for (unsigned i = 0; i < out->num_short_term_ref_pic_sets; ++i) {
        rps_capture_t rc;
        skip_st_ref_pic_set(&b, i, out->num_short_term_ref_pic_sets, out, i == 0 ? &rc : NULL);
        if (i == 0) {
            out->sps_rps0_first_neg_delta = rc.first_neg_delta;
            out->sps_rps0_first_neg_used = rc.first_neg_used;
            out->sps_rps0_unresolved = rc.unresolved;
        }
    }
    out->long_term_ref_pics_present = bits_u(&b, 1) != 0;
    if (out->long_term_ref_pics_present) {
        unsigned num_lt = bits_ue(&b);
        for (unsigned i = 0; i < num_lt; ++i) {
            bits_u(&b, out->log2_max_poc_lsb);   /* lt_ref_pic_poc_lsb_sps */
            bits_u(&b, 1);                       /* used_by_curr_pic_lt_sps_flag */
        }
    }
    out->sps_temporal_mvp_enabled = bits_u(&b, 1) != 0;
    out->chroma_format_idc = chroma;
    if (b.overrun || ctb_log2 > 6 || width == 0 || height == 0) {
        return false;
    }
    unsigned ctb = 1u << ctb_log2;
    unsigned w_ctbs = (width + ctb - 1) / ctb;
    unsigned h_ctbs = (height + ctb - 1) / ctb;
    unsigned size_ctbs = w_ctbs * h_ctbs;
    unsigned addr_bits = 1;
    while ((1u << addr_bits) < size_ctbs) {
        addr_bits++;
    }
    out->slice_addr_bits = addr_bits;
    out->log2_diff_max_min_cb = log2_diff;
    out->w_ctbs = w_ctbs;
    out->h_ctbs = h_ctbs;
    return true;
}

static bool parse_pps(const unsigned char *nal, size_t len, coop_poc_params_t *out) {
    unsigned char rb[256];
    size_t n = to_rbsp(nal, len < sizeof(rb) ? len : sizeof(rb), rb, sizeof(rb));
    if (n <= 2) {
        return false;
    }
    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    bits_ue(&b);                         /* pps_pic_parameter_set_id */
    bits_ue(&b);                         /* pps_seq_parameter_set_id */
    out->dependent_slice_segments = bits_u(&b, 1) != 0;
    out->output_flag_present = bits_u(&b, 1) != 0;
    out->num_extra_slice_header_bits = bits_u(&b, 3);
    /* Everything below is needed only by the FULL header walk. It is parsed here rather
     * than in a second pass so there is one place where PPS field order is asserted. */
    bits_u(&b, 1);                       /* sign_data_hiding_enabled_flag */
    out->cabac_init_present = bits_u(&b, 1) != 0;
    out->num_ref_idx_l0_default = bits_ue(&b) + 1;
    out->num_ref_idx_l1_default = bits_ue(&b) + 1;
    out->init_qp_minus26 = bits_se(&b);  /* init_qp_minus26 */
    bits_u(&b, 1);                       /* constrained_intra_pred_flag */
    bits_u(&b, 1);                       /* transform_skip_enabled_flag */
    if (bits_u(&b, 1)) {                 /* cu_qp_delta_enabled_flag */
        bits_ue(&b);                     /* diff_cu_qp_delta_depth */
    }
    bits_se(&b);                         /* pps_cb_qp_offset */
    bits_se(&b);                         /* pps_cr_qp_offset */
    out->slice_chroma_qp_offsets_present = bits_u(&b, 1) != 0;
    out->weighted_pred = bits_u(&b, 1) != 0;
    out->weighted_bipred = bits_u(&b, 1) != 0;
    out->transquant_bypass_enabled = bits_u(&b, 1) != 0;
    out->tiles_enabled = bits_u(&b, 1) != 0;
    out->entropy_coding_sync_enabled = bits_u(&b, 1) != 0;
    if (out->tiles_enabled) {
        unsigned cols = bits_ue(&b) + 1;
        unsigned rows = bits_ue(&b) + 1;
        if (!bits_u(&b, 1)) {            /* uniform_spacing_flag */
            for (unsigned i = 0; i + 1 < cols; ++i) { bits_ue(&b); }
            for (unsigned i = 0; i + 1 < rows; ++i) { bits_ue(&b); }
        }
        bits_u(&b, 1);                   /* loop_filter_across_tiles_enabled_flag */
    }
    out->loop_filter_across_slices = bits_u(&b, 1) != 0;
    out->deblocking_filter_control_present = bits_u(&b, 1) != 0;
    if (out->deblocking_filter_control_present) {
        out->deblocking_filter_override_enabled = bits_u(&b, 1) != 0;
        out->pps_deblocking_filter_disabled = bits_u(&b, 1) != 0;
        if (!out->pps_deblocking_filter_disabled) {
            bits_se(&b);                 /* pps_beta_offset_div2 */
            bits_se(&b);                 /* pps_tc_offset_div2 */
        }
    }
    if (bits_u(&b, 1)) {                 /* pps_scaling_list_data_present_flag */
        skip_scaling_list(&b);
    }
    out->lists_modification_present = bits_u(&b, 1) != 0;
    bits_ue(&b);                         /* log2_parallel_merge_level_minus2 */
    out->header_extension_present = bits_u(&b, 1) != 0;
    return !b.overrun;
}

bool coop_poc_params_parse(const unsigned char *au, size_t len, coop_poc_params_t *out) {
    memset(out, 0, sizeof(*out));
    bool have_sps = false, have_pps = false;
    size_t i = 0;
    while (i + 4 <= len) {
        size_t sc = 0;
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            sc = 3;
        } else if (i + 5 <= len && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) {
            sc = 4;
        } else {
            i++;
            continue;
        }
        size_t start = i + sc;
        size_t end = start;
        while (end + 3 <= len &&
               !(au[end] == 0 && au[end + 1] == 0 && (au[end + 2] == 1 ||
                 (end + 4 <= len && au[end + 2] == 0 && au[end + 3] == 1)))) {
            end++;
        }
        if (end + 3 > len) {
            end = len;
        }
        int t = nal_type_of(au + start, end - start);
        if (t == NAL_SPS && !have_sps) {
            have_sps = parse_sps(au + start, end - start, out);
        } else if (t == NAL_PPS && !have_pps) {
            have_pps = parse_pps(au + start, end - start, out);
        }
        if (have_sps && have_pps) {
            break;
        }
        i = end;
    }
    out->valid = have_sps && have_pps;
    return out->valid;
}

/**
 * Walk the slice header and return the bit offset of slice_pic_order_cnt_lsb
 * within @p rb, or 0 when the slice carries no POC field.
 */
static size_t poc_bit_offset(const coop_poc_params_t *p, int t,
                             const unsigned char *rb, size_t n) {
    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    unsigned first = bits_u(&b, 1);
    if (is_irap(t)) {
        bits_u(&b, 1);                   /* no_output_of_prior_pics_flag */
    }
    bits_ue(&b);                         /* slice_pic_parameter_set_id */
    if (!first) {
        if (p->dependent_slice_segments && bits_u(&b, 1)) {
            return 0;                    /* dependent slice inherits the header */
        }
        bits_u(&b, p->slice_addr_bits);
    }
    bits_u(&b, p->num_extra_slice_header_bits);
    bits_ue(&b);                         /* slice_type */
    if (p->output_flag_present) {
        bits_u(&b, 1);
    }
    if (p->separate_colour_plane) {
        bits_u(&b, 2);
    }
    if (b.overrun || is_idr(t)) {
        return 0;
    }
    return b.pos + 16;                   /* +16 for the NAL header skipped above */
}

bool coop_poc_read(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                   unsigned *poc_out) {
    if (!p->valid || len < 3) {
        return false;
    }
    unsigned char rb[256];
    size_t n = to_rbsp(nal, len < sizeof(rb) ? len : sizeof(rb), rb, sizeof(rb));
    if (n <= 2) {
        return false;
    }
    size_t off = poc_bit_offset(p, nal_type_of(nal, len), rb, n);
    if (off == 0) {
        return false;
    }
    bits_t b;
    bits_init(&b, rb, n);
    b.pos = off;
    unsigned v = bits_u(&b, p->log2_max_poc_lsb);
    if (b.overrun) {
        return false;
    }
    if (poc_out != NULL) {
        *poc_out = v;
    }
    return true;
}


/* ---- bit WRITER, for edits that change a field's LENGTH -------------------
 * coop_poc_rewrite() can patch in place because POC is fixed-width u(v). Doubling
 * pic_height_in_luma_samples is a ue(v) edit: 1088 encodes in 21 bits, 2176 in 23, so
 * every bit after it shifts and the tail must be re-emitted rather than poked. */
typedef struct {
    unsigned char *d;
    size_t cap;
    size_t bit;
} bw_t;

static void bw_put(bw_t *w, unsigned val, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        size_t bp = w->bit + i;
        if ((bp >> 3) >= w->cap) {
            return;
        }
        unsigned char mask = (unsigned char) (0x80u >> (bp & 7u));
        if ((val >> (n - 1 - i)) & 1u) {
            w->d[bp >> 3] |= mask;
        } else {
            w->d[bp >> 3] &= (unsigned char) ~mask;
        }
    }
    w->bit += n;
}

/** Exp-Golomb: v+1 in binary, prefixed by that many-1 zeros. */
static void bw_ue(bw_t *w, unsigned v) {
    unsigned code = v + 1, bits = 0;
    while ((code >> bits) != 0) {
        bits++;
    }
    bw_put(w, 0, bits - 1);
    bw_put(w, code, bits);
}

static void bw_copy(bw_t *w, bits_t *r, size_t nbits) {
    for (size_t i = 0; i < nbits; ++i) {
        bw_put(w, bits_u(r, 1), 1);
    }
}



/**
 * Bit position at which the slice header's payload ends, i.e. where byte_alignment()
 * begins. Needed because inserting the address shifts everything after it, and the
 * entropy-coded slice data must stay byte-aligned -- copying the tail verbatim moved it
 * and a real decoder rejected the result ("cu_qp_delta outside valid range", which is
 * shifted bits rather than a bad value).
 *
 * Returns 0 if anything is unsupported, so callers refuse rather than emit a bad picture.
 */
static size_t slice_header_end_bit_x(const coop_poc_params_t *p, int t,
                                     const unsigned char *rb, size_t n,
                                     coop_slice_info_t *info) {
    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    unsigned first = bits_u(&b, 1);
    if (is_irap(t)) {
        bits_u(&b, 1);                          /* no_output_of_prior_pics_flag */
    }
    unsigned pps_id = bits_ue(&b);              /* slice_pic_parameter_set_id */
    if (info != NULL) { info->pps_id = pps_id; }
    if (!first) {
        if (p->dependent_slice_segments && bits_u(&b, 1)) {
            return 0;                           /* dependent slice: no header to re-emit */
        }
        bits_u(&b, p->slice_addr_bits);
    }
    bits_u(&b, p->num_extra_slice_header_bits);
    unsigned slice_type = bits_ue(&b);          /* 0=B 1=P 2=I */
    if (info != NULL) { info->slice_type = slice_type; }
    if (p->output_flag_present) {
        bits_u(&b, 1);
    }
    if (p->separate_colour_plane) {
        bits_u(&b, 2);
    }
    unsigned temporal_mvp = 0;
    if (!is_idr(t)) {
        bits_u(&b, p->log2_max_poc_lsb);        /* slice_pic_order_cnt_lsb */
        if (!bits_u(&b, 1)) {                   /* short_term_ref_pic_set_sps_flag */
            /* Coded INLINE in this slice header: unambiguous, no lookup needed. */
            rps_capture_t rc;
            skip_st_ref_pic_set(&b, p->num_short_term_ref_pic_sets,
                                p->num_short_term_ref_pic_sets, p, &rc);
            if (info != NULL) {
                info->first_neg_delta = rc.first_neg_delta;
                info->first_neg_used = rc.first_neg_used;
                info->rps_unresolved = rc.unresolved;
            }
        } else if (p->num_short_term_ref_pic_sets > 1) {
            unsigned bits = 1;
            while ((1u << bits) < p->num_short_term_ref_pic_sets) {
                bits++;
            }
            unsigned sel = bits_u(&b, bits);    /* short_term_ref_pic_set_idx */
            /* Only set 0 was captured from the SPS (NVENC declares one). Anything else is
             * reported UNRESOLVED rather than silently passing the check. */
            if (info != NULL) {
                info->rps_from_sps = 1;
                if (sel == 0) {
                    info->first_neg_delta = p->sps_rps0_first_neg_delta;
                    info->first_neg_used = p->sps_rps0_first_neg_used;
                    info->rps_unresolved = p->sps_rps0_unresolved;
                } else {
                    info->rps_unresolved = 1;
                }
            }
        } else if (info != NULL) {
            /* sps_flag set with a single declared set: index 0 is implicit, no bits read. */
            info->rps_from_sps = 1;
            info->first_neg_delta = p->sps_rps0_first_neg_delta;
            info->first_neg_used = p->sps_rps0_first_neg_used;
            info->rps_unresolved = p->sps_rps0_unresolved;
        }
        if (p->long_term_ref_pics_present) {
            return 0;                           /* not produced by this encoder */
        }
        if (p->sps_temporal_mvp_enabled) {
            temporal_mvp = bits_u(&b, 1);
        }
    }
    if (p->sao_enabled) {
        unsigned sl = bits_u(&b, 1);            /* slice_sao_luma_flag */
        unsigned sc = 0;
        if (p->chroma_format_idc != 0) {
            sc = bits_u(&b, 1);                 /* slice_sao_chroma_flag */
        }
        if (info != NULL) { info->sao_luma = sl; info->sao_chroma = sc; }
    }
    if (slice_type != 2) {                      /* P or B */
        unsigned l0_active = p->num_ref_idx_l0_default;   /* PPS default unless overridden */
        if (bits_u(&b, 1)) {                    /* num_ref_idx_active_override_flag */
            l0_active = bits_ue(&b) + 1u;       /* num_ref_idx_l0_active_minus1 */
            if (slice_type == 0) {
                bits_ue(&b);
            }
        }
        if (info != NULL) { info->ref_l0_active = l0_active; }
        if (p->lists_modification_present) {
            return 0;                           /* needs NumPicTotalCurr; not emitted here */
        }
        if (slice_type == 0) {
            bits_u(&b, 1);                      /* mvd_l1_zero_flag */
        }
        if (p->cabac_init_present) {
            unsigned ci = bits_u(&b, 1);        /* cabac_init_flag */
            if (info != NULL) { info->cabac_init_flag = ci; }
        }
        if (temporal_mvp) {
            unsigned from_l0 = 1;
            if (slice_type == 0) {
                from_l0 = bits_u(&b, 1);        /* collocated_from_l0_flag */
            }
            unsigned active = from_l0 ? p->num_ref_idx_l0_default : p->num_ref_idx_l1_default;
            if (active > 1) {
                bits_ue(&b);                    /* collocated_ref_idx */
            }
        }
        if ((p->weighted_pred && slice_type == 1) || (p->weighted_bipred && slice_type == 0)) {
            return 0;                           /* pred_weight_table not emitted here */
        }
        unsigned fm = bits_ue(&b);              /* five_minus_max_num_merge_cand */
        if (info != NULL) { info->max_num_merge_cand = (fm <= 4) ? (5u - fm) : 1u; }
    }
    {
        int qd = bits_se(&b);                   /* slice_qp_delta */
        if (info != NULL) { info->slice_qp = 26 + p->init_qp_minus26 + qd; }
    }
    if (p->slice_chroma_qp_offsets_present) {
        int cb = bits_se(&b);
        int cr = bits_se(&b);
        if (info != NULL) { info->slice_cb_qp_offset = cb; info->slice_cr_qp_offset = cr; }
    }
    if (p->chroma_qp_offset_list_enabled) {
        bits_u(&b, 1);                          /* cu_chroma_qp_offset_enabled_flag */
    }
    unsigned deblock_disabled = p->pps_deblocking_filter_disabled;
    if (p->deblocking_filter_override_enabled && bits_u(&b, 1)) {
        deblock_disabled = bits_u(&b, 1);       /* slice_deblocking_filter_disabled_flag */
        if (!deblock_disabled) {
            bits_se(&b);
            bits_se(&b);
        }
    }
    if (p->loop_filter_across_slices && (p->sao_enabled || !deblock_disabled)) {
        bits_u(&b, 1);                          /* slice_loop_filter_across_slices_enabled */
    }
    if (p->tiles_enabled || p->entropy_coding_sync_enabled) {
        return 0;                               /* entry point offsets not handled */
    }
    if (p->header_extension_present) {
        return 0;                               /* header extension not handled */
    }
    if (b.overrun) {
        return 0;
    }
    return b.pos + 16;                          /* +16: NAL header skipped by bits_init */
}

static size_t slice_header_end_bit(const coop_poc_params_t *p, int t,
                                   const unsigned char *rb, size_t n) {
    return slice_header_end_bit_x(p, t, rb, n, NULL);
}

size_t coop_slice_header_end_bit_public(const coop_poc_params_t *p, const unsigned char *nal,
                                        size_t len) {
    static unsigned char rb[4u * 1024u * 1024u];
    if (!p->valid || len < 3) { return 0; }
    size_t n = to_rbsp(nal, len, rb, sizeof(rb));
    if (n <= 2) { return 0; }
    return slice_header_end_bit(p, nal_type_of(nal, len), rb, n);
}

bool coop_slice_info(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                     coop_slice_info_t *out) {
    static unsigned char rb[4u * 1024u * 1024u];
    if (!p->valid || len < 3) {
        return false;
    }
    size_t n = to_rbsp(nal, len, rb, sizeof(rb));
    if (n <= 2) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->max_num_merge_cand = 5;
    out->slice_type = 2;
    return slice_header_end_bit_x(p, nal_type_of(nal, len), rb, n, out) != 0;
}

bool coop_slice_refcheck(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                         coop_slice_info_t *out) {
    if (!p->valid || len < 3) {
        return false;
    }
    /* A PREFIX IS ENOUGH, AND THE WHOLE POINT. slice_header_end_bit_x() returns at the
     * header end bit and never reads slice data, and it refuses every header that could be
     * long (long-term refs, list modification, pred_weight_table, entry point offsets,
     * header extension). What remains is a few dozen bits. If a header somehow did not fit,
     * the walk overruns and we return false -- no data rather than wrong data. */
    unsigned char rb[512];
    size_t n = to_rbsp(nal, len < sizeof(rb) ? len : sizeof(rb), rb, sizeof(rb));
    if (n <= 2) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->max_num_merge_cand = 5;
    out->slice_type = 2;
    return slice_header_end_bit_x(p, nal_type_of(nal, len), rb, n, out) != 0;
}

size_t coop_slice_set_address(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                              unsigned addr, unsigned char *out, size_t out_cap) {
    return coop_slice_set_address_payload(p, nal, len, addr, NULL, 0, out, out_cap);
}

/**
 * As coop_slice_set_address, but the entropy-coded slice DATA can be replaced.
 *
 * Concealment needs a header that is picture-consistent with our own slice -- same POC,
 * same reference picture set, same temporal-MVP setting, because HEVC requires those to
 * match across every slice of a picture -- carrying entirely different payload bits. That
 * is exactly this function: our header, re-addressed to the bottom half, followed by
 * whatever CABAC data the caller synthesised.
 */
/**
 * IDR slice -> TRAIL_R slice carrying an I-slice. See coop_poc.h for why.
 *
 * The two headers differ in exactly two places: an IRAP carries
 * no_output_of_prior_pics_flag (dropped here), and a non-IRAP carries
 * slice_pic_order_cnt_lsb + short_term_ref_pic_set + optional long-term and
 * temporal-mvp fields (inserted here). Everything before and after those is identical,
 * so it is copied bit-for-bit -- and the tail is re-aligned exactly as
 * coop_slice_set_address does, because the inserted bits move the header end.
 *
 * The inserted RPS is EMPTY (num_negative_pics = num_positive_pics = 0), which is the
 * whole point: the slice declares that it references nothing, so it stays self-contained
 * even though the picture around it is a trailing picture that does reference.
 */
static size_t idr_to_trail_x(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                             unsigned poc, bool keep_ref, bool sps_set,
                             unsigned char *out, size_t out_cap) {
    static unsigned char rb[4u * 1024u * 1024u + 64u];
    static unsigned char nrb[4u * 1024u * 1024u + 64u];
    size_t n = to_rbsp(nal, len, rb, sizeof(rb));
    if (n <= 2 || p == NULL || !p->valid) {
        return 0;
    }
    const int t = nal_type_of(nal, len);
    if (t != 19 && t != 20) {
        return 0;                       /* only IDR_W_RADL / IDR_N_LP lack POC + RPS */
    }
    const size_t hdr_end = slice_header_end_bit(p, t, rb, n);
    if (hdr_end == 0) {
        return 0;
    }

    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    unsigned first = bits_u(&b, 1);
    unsigned no_output = bits_u(&b, 1);          /* IRAP-only; deliberately dropped */
    (void) no_output;
    unsigned pps_id = bits_ue(&b);
    unsigned dep = 0, addr = 0;
    if (!first) {
        if (p->dependent_slice_segments) {
            dep = bits_u(&b, 1);
            if (dep) {
                return 0;
            }
        }
        addr = bits_u(&b, p->slice_addr_bits);
    }
    const size_t mid_from = b.pos + 16;          /* extra bits / slice_type / ... start here */
    /* Walk the fields that sit between pps_id and the POC, so they can be copied as a run. */
    bits_u(&b, p->num_extra_slice_header_bits);
    unsigned slice_type = bits_ue(&b);
    if (p->output_flag_present) {
        bits_u(&b, 1);
    }
    if (p->separate_colour_plane) {
        bits_u(&b, 2);
    }
    const size_t tail_from = b.pos + 16;         /* the common tail begins here */
    if (b.overrun || tail_from > hdr_end || mid_from > tail_from) {
        return 0;
    }
    if (slice_type != 2) {
        return 0;                       /* only an I-slice is self-contained */
    }

    memset(nrb, 0, sizeof(nrb) < (n + 64) ? sizeof(nrb) : (n + 64));
    bw_t w = {nrb, sizeof(nrb), 0};
    /* nal_unit_type -> TRAIL_R (1). Byte 0 is forbidden_zero(1) | type(6) | layer_id hi(1). */
    bw_put(&w, (unsigned) ((rb[0] & 0x81u) | (1u << 1)), 8);
    bw_put(&w, rb[1], 8);
    bw_put(&w, first, 1);
    /* no_output_of_prior_pics_flag is NOT emitted: it exists only for IRAP types. */
    bw_ue(&w, pps_id);
    if (!first) {
        if (p->dependent_slice_segments) {
            bw_put(&w, 0, 1);
        }
        bw_put(&w, addr, p->slice_addr_bits);
    }

    bits_t src;
    bits_init(&src, rb + 2, n - 2);
    for (size_t i = 16; i < mid_from; ++i) {
        bits_u(&src, 1);
    }
    bw_copy(&w, &src, tail_from - mid_from);     /* extra bits, slice_type, output, plane */

    /* THE INSERTION: everything a non-IRAP slice header carries that an IDR does not. */
    bw_put(&w, poc & ((1u << p->log2_max_poc_lsb) - 1u), p->log2_max_poc_lsb);
    if (sps_set) {
        /* POINT AT THE SPS's ONE SET, as the picture's other slice does (see
         * coop_slice_idr_to_trail_sps). With a single declared set the index is implicit. */
        bw_put(&w, 1, 1);                        /* short_term_ref_pic_set_sps_flag = 1 */
    } else {
        bw_put(&w, 0, 1);                        /* short_term_ref_pic_set_sps_flag = 0 */
        /* st_ref_pic_set(stRpsIdx) with stRpsIdx = num_short_term_ref_pic_sets. The spec gates
         * inter_ref_pic_set_prediction_flag on stRpsIdx != 0, so an SPS that declares ANY set
         * (NVENC declares 1) makes this bit MANDATORY. Omitting it is not a missing default:
         * the decoder reads our num_negative_pics ue(v) '1' AS the flag, takes the
         * inter-prediction branch, and every later field -- SAO flags, slice_qp_delta, the
         * byte at which CABAC starts -- is off. skip_st_ref_pic_set() above already reads it,
         * so the parser and the writer disagreed and only a PIXEL check could show it. */
        if (p->num_short_term_ref_pic_sets != 0) {
            bw_put(&w, 0, 1);                    /* inter_ref_pic_set_prediction_flag = 0 */
        }
        if (keep_ref) {
            /* KEEP THE PREVIOUS PICTURE AS A REFERENCE.
             *
             * The empty-RPS form below is right when this converted slice is slice 1 and OUR
             * inter slice -- carrying the real RPS -- is slice 0. It is WRONG when the converted
             * slice is slice 0, because the first slice is what the decoder derives DPB state
             * from: an empty RPS there evicts the previous picture, and the concealment slice
             * spliced in beside it then has nothing to copy from. That is the whole reason an
             * unpaired IDR could not carry a concealment slice.
             *
             * An I-slice never reads a reference list, so declaring one costs it nothing -- it
             * only keeps the previous picture alive for the OTHER slice of the same picture. */
            bw_ue(&w, 1);                        /* num_negative_pics = 1 */
            bw_ue(&w, 0);                        /* num_positive_pics = 0 */
            bw_ue(&w, 0);                        /* delta_poc_s0_minus1[0] = 0 -> POC-1 */
            bw_put(&w, 1, 1);                    /* used_by_curr_pic_s0_flag[0] = 1 */
        } else {
            bw_ue(&w, 0);                        /* num_negative_pics = 0 */
            bw_ue(&w, 0);                        /* num_positive_pics = 0  -> EMPTY RPS */
        }
    }
    if (p->long_term_ref_pics_present) {
        bw_ue(&w, 0);                            /* num_long_term_sps  */
        bw_ue(&w, 0);                            /* num_long_term_pics */
    }
    if (p->sps_temporal_mvp_enabled) {
        bw_put(&w, 0, 1);                        /* slice_temporal_mvp_enabled_flag = 0 */
    }

    bw_copy(&w, &src, hdr_end - tail_from);      /* the common tail, verbatim */

    bw_put(&w, 1, 1);                            /* fresh byte_alignment() */
    while (w.bit & 7u) {
        bw_put(&w, 0, 1);
    }

    size_t data_byte = ((hdr_end + 1u) + 7u) / 8u;
    if (data_byte > n) {
        return 0;
    }
    size_t data_len = n - data_byte;
    size_t at = w.bit / 8u;
    if (at + data_len > sizeof(nrb)) {
        return 0;
    }
    memcpy(nrb + at, rb + data_byte, data_len);
    return to_ebsp(nrb, at + data_len, out, out_cap);
}

size_t coop_slice_idr_to_trail(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                               unsigned poc, unsigned char *out, size_t out_cap) {
    return idr_to_trail_x(p, nal, len, poc, false, false, out, out_cap);
}

size_t coop_slice_idr_to_trail_keepref(const coop_poc_params_t *p, const unsigned char *nal,
                                       size_t len, unsigned poc,
                                       unsigned char *out, size_t out_cap) {
    return idr_to_trail_x(p, nal, len, poc, true, false, out, out_cap);
}

size_t coop_slice_idr_to_trail_sps(const coop_poc_params_t *p, const unsigned char *nal,
                                   size_t len, unsigned poc,
                                   unsigned char *out, size_t out_cap) {
    if (p == NULL || p->num_short_term_ref_pic_sets != 1) {
        return 0;                       /* the index would need bits, and we would have to pick one */
    }
    return idr_to_trail_x(p, nal, len, poc, true, true, out, out_cap);
}

size_t coop_slice_to_tile(const coop_poc_params_t *src, const coop_poc_params_t *comp,
                          const unsigned char *nal, size_t len, unsigned addr,
                          unsigned char *out, size_t out_cap) {
    /* Re-home a PANE slice into a TILED composite.
     *
     * This is not just coop_slice_readdress with a different address, because enabling tiles
     * CHANGES THE SLICE HEADER LAYOUT: when tiles_enabled_flag (or WPP) is set, the header
     * carries num_entry_point_offsets near its end. The pane's PPS has tiles OFF, so its
     * slices have no such field. Copy the header across unchanged and the decoder reads the
     * entry-point count out of whatever follows -- which shows up as
     * "alignment_bit_equal_to_one=0", i.e. the byte alignment landing in the wrong place.
     *
     * Each composite slice covers exactly ONE tile, so it has no entry points and the value
     * is simply 0. Everything else -- walking with `src` because the header was written with
     * the pane's addr_bits, writing the address with `comp` -- matches coop_slice_readdress.
     */
    if (src == NULL || comp == NULL || !src->valid || !comp->valid || len < 3) {
        return 0;
    }
    static unsigned char rb[4u * 1024u * 1024u];
    static unsigned char nrb[4u * 1024u * 1024u + 64u];
    size_t n = to_rbsp(nal, len, rb, sizeof(rb));
    if (n <= 2) {
        return 0;
    }
    const int t = nal_type_of(nal, len);
    const size_t hdr_end = slice_header_end_bit(src, t, rb, n);
    if (hdr_end == 0) {
        return 0;
    }
    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    const unsigned first = bits_u(&b, 1);
    const unsigned no_output = is_irap(t) ? bits_u(&b, 1) : 0;
    const unsigned pps_id = bits_ue(&b);
    if (!first) {
        if (src->dependent_slice_segments) {
            if (bits_u(&b, 1)) {
                return 0;
            }
        }
        bits_u(&b, src->slice_addr_bits);        /* old address, replaced */
    }
    const size_t copy_from = b.pos + 16;
    if (b.overrun || copy_from > hdr_end) {
        return 0;
    }

    memset(nrb, 0, sizeof(nrb) < (n + 64) ? sizeof(nrb) : (n + 64));
    bw_t w = {nrb, sizeof(nrb), 0};
    bw_put(&w, rb[0], 8);
    bw_put(&w, rb[1], 8);
    const int want_first = (addr == 0);
    bw_put(&w, want_first ? 1 : 0, 1);
    if (is_irap(t)) {
        bw_put(&w, no_output, 1);
    }
    bw_ue(&w, pps_id);
    if (!want_first) {
        if (comp->dependent_slice_segments) {
            bw_put(&w, 0, 1);
        }
        bw_put(&w, addr, comp->slice_addr_bits);
    }
    bits_t s2;
    bits_init(&s2, rb + 2, n - 2);
    for (size_t i = 16; i < copy_from; ++i) {
        bits_u(&s2, 1);
    }
    bw_copy(&w, &s2, hdr_end - copy_from);       /* the common tail, verbatim */
    bw_ue(&w, 0);                                /* num_entry_point_offsets = 0 */

    bw_put(&w, 1, 1);                            /* fresh byte_alignment() */
    while (w.bit & 7u) {
        bw_put(&w, 0, 1);
    }
    size_t data_byte = ((hdr_end + 1u) + 7u) / 8u;
    if (data_byte > n) {
        return 0;
    }
    size_t data_len = n - data_byte;
    size_t at = w.bit / 8u;
    if (at + data_len > sizeof(nrb)) {
        return 0;
    }
    memcpy(nrb + at, rb + data_byte, data_len);
    return to_ebsp(nrb, at + data_len, out, out_cap);
}

bool coop_slice_address(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                        bool *first_out, unsigned *addr_out) {
    /* Read first_slice_segment_in_pic_flag and, when clear, slice_segment_address.
     * The TILE composite needs the VALUE (to map a pane address into tile scan), which
     * coop_slice_readdress only discards. */
    if (p == NULL || !p->valid || nal == NULL || len < 3) {
        return false;
    }
    unsigned char rb[64];
    size_t n = to_rbsp(nal, len < sizeof(rb) ? len : sizeof(rb), rb, sizeof(rb));
    if (n <= 2) {
        return false;
    }
    const int t = nal_type_of(nal, len);
    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    const unsigned first = bits_u(&b, 1);
    if (is_irap(t)) {
        bits_u(&b, 1);                   /* no_output_of_prior_pics_flag */
    }
    bits_ue(&b);                         /* slice_pic_parameter_set_id */
    unsigned addr = 0;
    if (!first) {
        if (p->dependent_slice_segments) {
            if (bits_u(&b, 1)) {
                return false;            /* dependent segment: not relocatable */
            }
        }
        addr = bits_u(&b, p->slice_addr_bits);
    }
    if (b.overrun) {
        return false;
    }
    if (first_out != NULL) { *first_out = (first != 0); }
    if (addr_out != NULL) { *addr_out = addr; }
    return true;
}

size_t coop_slice_readdress(const coop_poc_params_t *src, const coop_poc_params_t *comp,
                            const unsigned char *nal, size_t len, unsigned addr,
                            unsigned char *out, size_t out_cap) {
    /* REPLACE an existing slice_segment_address, rather than insert one.
     *
     * coop_slice_set_address handles the pane's FIRST slice: first_slice_segment_in_pic_flag
     * is 1, no address is present, and one is inserted. Every other slice of a multi-slice
     * pane already has the flag clear AND an address -- and that address is sized by the
     * PANE's PicSizeInCtbsY, while the composite needs one sized by ITS OWN. So the field
     * is not merely rewritten, it changes WIDTH (12 bits -> 13 for a 60x64 pane in a 120x64
     * composite), which moves every following bit exactly as an insertion would.
     *
     * Hence two parameter sets: `src` to WALK the slice (its header was written with the
     * pane's addr_bits) and `comp` to WRITE the new address. Passing one for both silently
     * mis-parses everything after the address.
     */
    if (src == NULL || comp == NULL || !src->valid || !comp->valid || len < 3) {
        return 0;
    }
    static unsigned char rb[4u * 1024u * 1024u];
    static unsigned char nrb[4u * 1024u * 1024u + 64u];
    size_t n = to_rbsp(nal, len, rb, sizeof(rb));
    if (n <= 2) {
        return 0;
    }
    const int t = nal_type_of(nal, len);
    const size_t hdr_end = slice_header_end_bit(src, t, rb, n);
    if (hdr_end == 0) {
        return 0;
    }

    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    unsigned first = bits_u(&b, 1);
    if (first) {
        return 0;                       /* that is coop_slice_set_address's job */
    }
    unsigned no_output = is_irap(t) ? bits_u(&b, 1) : 0;
    unsigned pps_id = bits_ue(&b);
    if (src->dependent_slice_segments) {
        if (bits_u(&b, 1)) {
            return 0;                   /* dependent segments cannot be relocated */
        }
    }
    bits_u(&b, src->slice_addr_bits);    /* the OLD address, discarded */
    const size_t copy_from = b.pos + 16;
    if (b.overrun || copy_from > hdr_end) {
        return 0;
    }

    memset(nrb, 0, sizeof(nrb) < (n + 64) ? sizeof(nrb) : (n + 64));
    bw_t w = {nrb, sizeof(nrb), 0};
    bw_put(&w, rb[0], 8);
    bw_put(&w, rb[1], 8);
    bw_put(&w, 0, 1);                    /* first_slice_segment_in_pic_flag stays 0 */
    if (is_irap(t)) {
        bw_put(&w, no_output, 1);
    }
    bw_ue(&w, pps_id);
    if (comp->dependent_slice_segments) {
        bw_put(&w, 0, 1);
    }
    bw_put(&w, addr, comp->slice_addr_bits);

    bits_t s2;
    bits_init(&s2, rb + 2, n - 2);
    for (size_t i = 16; i < copy_from; ++i) {
        bits_u(&s2, 1);
    }
    bw_copy(&w, &s2, hdr_end - copy_from);

    bw_put(&w, 1, 1);                    /* fresh byte_alignment() at the new offset */
    while (w.bit & 7u) {
        bw_put(&w, 0, 1);
    }
    size_t data_byte = ((hdr_end + 1u) + 7u) / 8u;
    if (data_byte > n) {
        return 0;
    }
    size_t data_len = n - data_byte;
    size_t at = w.bit / 8u;
    if (at + data_len > sizeof(nrb)) {
        return 0;
    }
    memcpy(nrb + at, rb + data_byte, data_len);
    return to_ebsp(nrb, at + data_len, out, out_cap);
}

size_t coop_slice_set_address_payload(const coop_poc_params_t *p, const unsigned char *nal,
                                      size_t len, unsigned addr,
                                      const unsigned char *payload, size_t payload_len,
                                      unsigned char *out, size_t out_cap) {
    if (!p->valid || len < 3) {
        return 0;
    }
    static unsigned char rb[4u * 1024u * 1024u];
    static unsigned char nrb[4u * 1024u * 1024u + 64u];
    size_t n = to_rbsp(nal, len, rb, sizeof(rb));
    if (n <= 2) {
        return 0;
    }
    const int t = nal_type_of(nal, len);

    /* Where the header's fields stop and byte_alignment() begins. Everything from the
     * following byte boundary is entropy-coded data that must be copied BYTE-WISE and
     * land byte-aligned again -- that is the whole difficulty here. */
    const size_t hdr_end = slice_header_end_bit(p, t, rb, n);
    if (hdr_end == 0) {
        return 0;
    }

    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    unsigned first = bits_u(&b, 1);
    if (!first) {
        return 0;                       /* already a non-first slice */
    }
    unsigned no_output = is_irap(t) ? bits_u(&b, 1) : 0;
    unsigned pps_id = bits_ue(&b);
    const size_t copy_from = b.pos + 16;      /* first header bit copied unchanged */
    if (b.overrun || copy_from > hdr_end) {
        return 0;
    }

    memset(nrb, 0, sizeof(nrb) < (n + 64) ? sizeof(nrb) : (n + 64));
    bw_t w = {nrb, sizeof(nrb), 0};
    bw_put(&w, rb[0], 8);
    bw_put(&w, rb[1], 8);
    bw_put(&w, 0, 1);                          /* first_slice_segment_in_pic_flag = 0 */
    if (is_irap(t)) {
        bw_put(&w, no_output, 1);
    }
    bw_ue(&w, pps_id);
    /* These two exist ONLY when the first-slice flag is clear, which is why this is an
     * insertion. slice_addr_bits must come from the COMPOSITE parameter sets. */
    if (p->dependent_slice_segments) {
        bw_put(&w, 0, 1);
    }
    bw_put(&w, addr, p->slice_addr_bits);

    /* Copy the remaining HEADER bits verbatim... */
    bits_t src;
    bits_init(&src, rb + 2, n - 2);
    for (size_t i = 16; i < copy_from; ++i) {
        bits_u(&src, 1);
    }
    bw_copy(&w, &src, hdr_end - copy_from);

    /* ...then emit a FRESH byte_alignment() at the new position, rather than copying the
     * old one, which was aligned for the old header length. */
    bw_put(&w, 1, 1);
    while (w.bit & 7u) {
        bw_put(&w, 0, 1);
    }

    /* The original data began at the byte boundary after ITS alignment bit. */
    size_t data_byte = ((hdr_end + 1u) + 7u) / 8u;
    if (data_byte > n) {
        return 0;
    }
    size_t data_len = n - data_byte;
    const unsigned char *data_src = rb + data_byte;
    if (payload != NULL) {
        data_src = payload;
        data_len = payload_len;
    }
    size_t at = w.bit / 8u;
    if (at + data_len > sizeof(nrb)) {
        return 0;
    }
    memcpy(nrb + at, data_src, data_len);
    /* Emulation prevention is applied over header AND data together: a 00 00 pair that
     * straddles the boundary needs escaping just like one inside either part. */
    return to_ebsp(nrb, at + data_len, out, out_cap);
}

size_t coop_sps_double_height(const unsigned char *nal, size_t len,
                              unsigned char *out, size_t out_cap) {
    unsigned char rb[512], nrb[640];
    size_t n = to_rbsp(nal, len < sizeof(rb) ? len : sizeof(rb), rb, sizeof(rb));
    if (n <= 2) {
        return 0;
    }
    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    bits_u(&b, 4);
    unsigned max_sub = bits_u(&b, 3);
    bits_u(&b, 1);
    skip_ptl(&b, max_sub);
    bits_ue(&b);
    unsigned chroma = bits_ue(&b);
    if (chroma == 3) {
        bits_u(&b, 1);
    }
    bits_ue(&b);                                  /* pic_width_in_luma_samples */
    const size_t height_bit = b.pos;              /* where the field we replace starts */
    unsigned height = bits_ue(&b);
    unsigned win = bits_u(&b, 1);
    unsigned wl = 0, wr = 0, wt = 0, wb = 0;
    if (win) {
        wl = bits_ue(&b); wr = bits_ue(&b); wt = bits_ue(&b); wb = bits_ue(&b);
    }
    const size_t tail_bit = b.pos;                /* everything from here is copied as-is */
    if (b.overrun || height == 0) {
        return 0;
    }

    memset(nrb, 0, sizeof(nrb));
    bw_t w = {nrb, sizeof(nrb), 0};
    /* NAL header (2 bytes) is outside the RBSP bit-reader's view, so copy it first. */
    bw_put(&w, rb[0], 8);
    bw_put(&w, rb[1], 8);

    bits_t src;
    bits_init(&src, rb + 2, n - 2);
    bw_copy(&w, &src, height_bit);                /* everything before the height */

    bw_ue(&w, height * 2u);
    bw_put(&w, win, 1);
    if (win) {
        bw_ue(&w, wl);
        bw_ue(&w, wr);
        bw_ue(&w, wt);
        /* Offsets are in chroma units, so cropping scales with the height: a source
         * cropping 1088->1080 (bottom=4) must become 2176->2160 (bottom=8). Doubling
         * the offset is exact for any source: new_display = 2*old_display. */
        bw_ue(&w, wb * 2u);
    }
    /* Re-align the reader onto the tail and copy the remainder bit-for-bit. */
    bits_init(&src, rb + 2, n - 2);
    for (size_t i = 0; i < tail_bit; ++i) {
        bits_u(&src, 1);
    }
    const size_t total_bits = (n - 2) * 8;
    if (tail_bit > total_bits) {
        return 0;
    }
    bw_copy(&w, &src, total_bits - tail_bit);

    size_t nbytes = (w.bit + 7) / 8;
    return to_ebsp(nrb, nbytes, out, out_cap);
}

size_t coop_pps_enable_tiles(const unsigned char *nal, size_t len,
                             unsigned char *out, size_t out_cap) {
    /* The vertical split's grid: two columns, one row. */
    return coop_pps_enable_tiles_grid(nal, len, 1, 0, out, out_cap);
}

size_t coop_pps_enable_tiles_grid(const unsigned char *nal, size_t len,
                                  unsigned cols_minus1, unsigned rows_minus1,
                                  unsigned char *out, size_t out_cap) {
    /* Turn a pane's PPS into the composite's: TWO UNIFORM TILES, columns or rows.
     *
     * This is what makes a vertical split cost 2 slices instead of 128. A tile column's CTU
     * scan is 60 CTBs per row, row by row -- byte-for-byte the scan of a 1920-wide pane --
     * so a pane's slice data drops into a tile column verbatim. Slices must be contiguous in
     * RASTER order, which is why one-slice-per-pane cannot be side by side; tiles have no
     * such rule.
     *
     * loop_filter_across_tiles_enabled_flag is written 0 deliberately: deblocking is a
     * separate stage from prediction and would otherwise smear the two players' pixels into
     * each other along the seam, which is the same trap the horizontal path hit.
     *
     * Refuses a PPS that ALREADY has tiles -- rewriting a variable-length structure in place
     * is a different job, and the panes we build from never have them (NVENC HEVC cannot
     * emit tiles at all; verified in the SDK header).
     */
    unsigned char rb[512], nrb[640];
    size_t n = to_rbsp(nal, len < sizeof(rb) ? len : sizeof(rb), rb, sizeof(rb));
    if (n <= 2) {
        return 0;
    }
    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    bits_ue(&b);                         /* pps_pic_parameter_set_id */
    bits_ue(&b);                         /* pps_seq_parameter_set_id */
    bits_u(&b, 1);                       /* dependent_slice_segments_enabled_flag */
    bits_u(&b, 1);                       /* output_flag_present_flag */
    bits_u(&b, 3);                       /* num_extra_slice_header_bits */
    bits_u(&b, 1);                       /* sign_data_hiding_enabled_flag */
    bits_u(&b, 1);                       /* cabac_init_present_flag */
    bits_ue(&b);                         /* num_ref_idx_l0_default_active_minus1 */
    bits_ue(&b);                         /* num_ref_idx_l1_default_active_minus1 */
    bits_se(&b);                         /* init_qp_minus26 */
    bits_u(&b, 1);                       /* constrained_intra_pred_flag */
    bits_u(&b, 1);                       /* transform_skip_enabled_flag */
    if (bits_u(&b, 1)) {                 /* cu_qp_delta_enabled_flag */
        bits_ue(&b);
    }
    bits_se(&b);                         /* pps_cb_qp_offset */
    bits_se(&b);                         /* pps_cr_qp_offset */
    bits_u(&b, 1);                       /* pps_slice_chroma_qp_offsets_present_flag */
    bits_u(&b, 1);                       /* weighted_pred_flag */
    bits_u(&b, 1);                       /* weighted_bipred_flag */
    bits_u(&b, 1);                       /* transquant_bypass_enabled_flag */
    const size_t tiles_bit = b.pos;      /* tiles_enabled_flag starts here */
    const unsigned had_tiles = bits_u(&b, 1);
    const unsigned wpp = bits_u(&b, 1);  /* entropy_coding_sync_enabled_flag */
    if (had_tiles) {
        return 0;
    }
    const size_t tail_bit = b.pos;
    if (b.overrun) {
        return 0;
    }

    memset(nrb, 0, sizeof(nrb));
    bw_t w = {nrb, sizeof(nrb), 0};
    bw_put(&w, rb[0], 8);
    bw_put(&w, rb[1], 8);
    bits_t src;
    bits_init(&src, rb + 2, n - 2);
    bw_copy(&w, &src, tiles_bit);        /* everything before tiles_enabled_flag */

    bw_put(&w, 1, 1);                    /* tiles_enabled_flag = 1 */
    bw_put(&w, wpp, 1);                  /* entropy_coding_sync_enabled_flag, unchanged */
    /* THE GRID IS THE ONLY THING THAT DIFFERS BETWEEN THE TWO SPLIT MODES. Vertical wants
     * (1,0) -- two columns; horizontal wants (0,1) -- two ROWS. Everything else about a tiled
     * composite is identical, which is the point of sharing this function: horizontal then
     * rides coop_slice_to_tile(), the address path the working mode uses. */
    bw_ue(&w, cols_minus1);              /* num_tile_columns_minus1 */
    bw_ue(&w, rows_minus1);              /* num_tile_rows_minus1 */
    bw_put(&w, 1, 1);                    /* uniform_spacing_flag = 1 */
    bw_put(&w, 0, 1);                    /* loop_filter_across_tiles_enabled_flag = 0 */

    bits_init(&src, rb + 2, n - 2);
    for (size_t i = 0; i < tail_bit; ++i) {
        bits_u(&src, 1);
    }
    const size_t total_bits = (n - 2) * 8;
    if (tail_bit > total_bits) {
        return 0;
    }
    bw_copy(&w, &src, total_bits - tail_bit);

    size_t nbytes = (w.bit + 7) / 8;
    return to_ebsp(nrb, nbytes, out, out_cap);
}

size_t coop_sps_double_width(const unsigned char *nal, size_t len,
                             unsigned char *out, size_t out_cap) {
    /* The VERTICAL (left/right) split's counterpart to coop_sps_double_height. Panes sit
     * SIDE BY SIDE, so the width doubles and the height is untouched. Everything else --
     * the bit-exact copy either side of the edited field, the re-alignment of the tail --
     * is identical, because the hazard is identical: pic_width_in_luma_samples is ue(v),
     * so replacing it moves every following bit. */
    unsigned char rb[512], nrb[640];
    size_t n = to_rbsp(nal, len < sizeof(rb) ? len : sizeof(rb), rb, sizeof(rb));
    if (n <= 2) {
        return 0;
    }
    bits_t b;
    bits_init(&b, rb + 2, n - 2);
    bits_u(&b, 4);
    unsigned max_sub = bits_u(&b, 3);
    bits_u(&b, 1);
    skip_ptl(&b, max_sub);
    bits_ue(&b);
    unsigned chroma = bits_ue(&b);
    if (chroma == 3) {
        bits_u(&b, 1);
    }
    const size_t width_bit = b.pos;               /* where the field we replace starts */
    unsigned width = bits_ue(&b);
    unsigned height = bits_ue(&b);
    unsigned win = bits_u(&b, 1);
    unsigned wl = 0, wr = 0, wt = 0, wb = 0;
    if (win) {
        wl = bits_ue(&b); wr = bits_ue(&b); wt = bits_ue(&b); wb = bits_ue(&b);
    }
    const size_t tail_bit = b.pos;
    if (b.overrun || width == 0) {
        return 0;
    }

    memset(nrb, 0, sizeof(nrb));
    bw_t w = {nrb, sizeof(nrb), 0};
    bw_put(&w, rb[0], 8);
    bw_put(&w, rb[1], 8);

    bits_t src;
    bits_init(&src, rb + 2, n - 2);
    bw_copy(&w, &src, width_bit);

    bw_ue(&w, width * 2u);
    bw_ue(&w, height);                            /* unchanged: the panes are side by side */
    bw_put(&w, win, 1);
    if (win) {
        bw_ue(&w, wl);
        bw_ue(&w, wr * 2u);                       /* cropping scales with the width */
        bw_ue(&w, wt);
        bw_ue(&w, wb);
    }
    bits_init(&src, rb + 2, n - 2);
    for (size_t i = 0; i < tail_bit; ++i) {
        bits_u(&src, 1);
    }
    const size_t total_bits = (n - 2) * 8;
    if (tail_bit > total_bits) {
        return 0;
    }
    bw_copy(&w, &src, total_bits - tail_bit);

    size_t nbytes = (w.bit + 7) / 8;
    return to_ebsp(nrb, nbytes, out, out_cap);
}

size_t coop_poc_rewrite(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                        unsigned new_poc, unsigned char *out, size_t out_cap) {
    if (!p->valid || len < 3) {
        return 0;
    }
    /* The whole NAL must go through RBSP space: patching a byte can create or
     * destroy an emulation-prevention pattern anywhere after it, so the escape
     * bytes are recomputed over the entire payload rather than patched in place. */
    static unsigned char rb[4u * 1024u * 1024u];
    size_t n = to_rbsp(nal, len, rb, sizeof(rb));
    if (n <= 2) {
        return 0;
    }
    /* Header parsing only ever touches the first bytes, but the offset is into
     * the full RBSP, so pass the whole thing. */
    size_t off = poc_bit_offset(p, nal_type_of(nal, len), rb, n < 256 ? n : 256);
    if (off == 0) {
        return 0;
    }
    if ((off + p->log2_max_poc_lsb + 7) / 8 > n) {
        return 0;
    }
    for (unsigned i = 0; i < p->log2_max_poc_lsb; ++i) {
        size_t bitpos = off + i;
        unsigned bit = (new_poc >> (p->log2_max_poc_lsb - 1 - i)) & 1u;
        unsigned char mask = (unsigned char) (0x80u >> (bitpos & 7u));
        if (bit) {
            rb[bitpos >> 3] |= mask;
        } else {
            rb[bitpos >> 3] &= (unsigned char) ~mask;
        }
    }
    return to_ebsp(rb, n, out, out_cap);
}
