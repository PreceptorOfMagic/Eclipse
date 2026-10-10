/*
 * Co-op split-screen: slice-header picture-order-count rewriting.
 *
 * The splice puts slice 0 from one host and slice 1 from another into ONE
 * picture. HEVC requires every slice of a picture to carry the same
 * slice_pic_order_cnt_lsb, and each slice derives its reference list from its
 * OWN header. The two hosts count pictures independently and at different
 * effective frame rates, so their counts diverge: measured 0 of 119 spliced
 * pictures had matching POCs (poc=[51,2], [52,3], ...) while each host's own
 * stream was internally perfect (89 of 89 matching). The decoder therefore
 * could not resolve the spliced slice's references and discarded it -- which is
 * why the second pane only ever showed its bootstrap IDR and then went black.
 *
 * Fixing it means patching the incoming slice's POC to match ours. The field is
 * FIXED WIDTH (log2_max_pic_order_cnt_lsb bits), so the header does not change
 * length and the CABAC payload never has to be shifted -- only the emulation
 * prevention bytes have to be recomputed.
 *
 * Reference implementation and the measurement above: tests/coop/hevc_poc.py.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Fields from SPS/PPS needed to locate slice_pic_order_cnt_lsb. */
typedef struct {
    unsigned log2_max_poc_lsb;
    unsigned slice_addr_bits;            /**< width of slice_segment_address */
    unsigned log2_diff_max_min_cb;       /**< 0 means the CTB IS the minimum CB, so
                                          *   coding_quadtree codes no split_cu_flag */
    unsigned w_ctbs;                     /**< picture width in CTBs */
    unsigned h_ctbs;                     /**< picture height in CTBs */
    /* --- flags the FULL slice-header walk needs. Each one gates a field, so a wrong
     * value here silently mis-locates everything after it. From the PPS unless noted. */
    bool cabac_init_present;
    bool slice_chroma_qp_offsets_present;
    bool weighted_pred;                  /**< P slices carry a pred_weight_table */
    bool weighted_bipred;                /**< B slices carry a pred_weight_table */
    /* PPS. When set, EVERY coding unit opens with cu_transquant_bypass_flag --
     * including a skip CU -- so a synthesised slice that omits it desynchronises
     * the decoder's parse without producing a decode error. */
    bool transquant_bypass_enabled;
    bool tiles_enabled;
    bool entropy_coding_sync_enabled;
    bool loop_filter_across_slices;
    bool deblocking_filter_control_present;
    bool deblocking_filter_override_enabled;
    bool pps_deblocking_filter_disabled;
    bool lists_modification_present;
    bool header_extension_present;
    bool chroma_qp_offset_list_enabled;  /**< PPS range extension */
    int init_qp_minus26;                 /**< needed to derive SliceQpY for CABAC init */
    unsigned num_ref_idx_l0_default;     /**< +1 already applied */
    unsigned num_ref_idx_l1_default;
    /* from the SPS */
    bool sps_temporal_mvp_enabled;
    bool sao_enabled;
    bool long_term_ref_pics_present;
    unsigned num_short_term_ref_pic_sets;
    unsigned chroma_format_idc;
    unsigned num_extra_slice_header_bits;
    bool dependent_slice_segments;
    bool output_flag_present;
    bool separate_colour_plane;
    /* st_ref_pic_set(0) AS DECLARED BY THIS HOST'S SPS. Kept per-host and not in a shared
     * static (the way g_num_delta_pocs is) because the two panes are parsed into SEPARATE
     * params and whichever SPS was parsed last would otherwise answer for both -- which for
     * a DETECTOR is a false positive, not merely untidy. Only set 0 is kept: NVENC declares
     * exactly one. See coop_slice_refcheck(). */
    unsigned sps_rps0_first_neg_delta;   /**< 1 == POC-1; 0 == no negative refs */
    unsigned sps_rps0_first_neg_used;    /**< used_by_curr_pic_s0_flag[0] */
    unsigned sps_rps0_unresolved;        /**< 1 = inter-predicted set; delta not derivable here */
    bool valid;
} coop_poc_params_t;

/** Slice-header fields a SYNTHESISED slice must reproduce to stay consistent with the
 *  picture it joins, and that CABAC initialisation depends on. */
typedef struct {
    unsigned slice_type;                 /**< 0=B 1=P 2=I */
    unsigned sao_luma, sao_chroma;       /**< whether sao() is coded per CTU */
    unsigned cabac_init_flag;
    unsigned max_num_merge_cand;
    int slice_qp;                        /**< SliceQpY = 26 + init_qp_minus26 + delta */
    /* CHROMA. Walked past and discarded until now, which is why a fault that destroys
     * chroma and leaves luma perfect had no offline signature. Both are read straight
     * from the header; 0 when the PPS does not carry per-slice offsets. */
    int slice_cb_qp_offset;
    int slice_cr_qp_offset;
    unsigned pps_id;                     /**< slice_pic_parameter_set_id -- two panes
                                          *   resolving to DIFFERENT PPSs would give each
                                          *   its own chroma QP offsets. */
    /* --- WHICH PICTURE THIS SLICE PREDICTS FROM. See coop_slice_refcheck(). --------------
     * The composite can only honour a reference at delta -1, because that is the only one
     * where the previous composite picture is guaranteed to hold this pane's previous frame
     * bit-exactly (a skip conceal reproduces it exactly; a fresh pane IS it). Any other
     * delta means "N pictures back" in COMPOSITE numbering, and while a pane trails, the
     * composite emits several pictures per pane -- so delta -2 is not the pane's previous-
     * but-one, it is a picture that pane never saw. session_video.c:1106 already asks the
     * host for a single reference frame for exactly this reason; these fields are how we
     * find out whether it obeyed. */
    unsigned ref_l0_active;              /**< num_ref_idx_l0_active: >1 means it MAY reach further back */
    unsigned first_neg_delta;            /**< nearest negative ref: 1 == POC-1; 0 == none */
    unsigned first_neg_used;             /**< used_by_curr_pic_s0_flag[0] */
    unsigned rps_unresolved;             /**< 1 = we could not derive the delta; treat as suspect */
} coop_slice_info_t;

/** Read the fields above out of a slice NAL. False if the header walk cannot handle it. */
bool coop_slice_info(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                     coop_slice_info_t *out);

/**
 * As coop_slice_info, but converts only the first bytes of the NAL rather than all of it.
 *
 * WHY A SEPARATE ENTRY POINT: coop_slice_info() runs to_rbsp() over the WHOLE access unit,
 * which is fine once per synthesised conceal and ruinous at 165 pictures a second against a
 * multi-megabyte slice. The header walk provably never reads past the slice header (it
 * returns at the header end bit), so a short prefix answers the same question for a fixed
 * cost. Returns false if the walk cannot handle the header.
 */
bool coop_slice_refcheck(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                         coop_slice_info_t *out);

/**
 * Parse the SPS and PPS out of an access unit's parameter-set prefix.
 * Both hosts run identical encoder settings, so this only needs doing once.
 */
bool coop_poc_params_parse(const unsigned char *au, size_t len, coop_poc_params_t *out);

/**
 * Read slice_pic_order_cnt_lsb from a slice NAL (payload, no start code).
 * Returns false for IDR and dependent slices, which carry no POC field.
 */
bool coop_poc_read(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                   unsigned *poc_out);

/**
 * Copy @p nal into @p out with slice_pic_order_cnt_lsb replaced by @p new_poc.
 * Returns the output length, or 0 if the slice carries no POC field or @p out
 * is too small. Output length may differ from the input by a few bytes because
 * emulation prevention is recomputed.
 */
size_t coop_poc_rewrite(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                        unsigned new_poc, unsigned char *out, size_t out_cap);

/**
 * Rewrite an SPS so it declares TWICE the picture height.
 *
 * Half-height co-op: each host encodes only its own 1080 rows, so its SPS says 1088
 * (CTU-padded). The composite is two of those stacked, so the stream handed to the
 * decoder must say 2176 -- and the conformance window must still crop to 2*1080=2160,
 * which is why the bottom offset is doubled too.
 *
 * This is why each encoder's reference picture can only ever contain its OWN pane, so
 * cross-pane prediction -- the ghosting -- becomes impossible rather than merely bounded.
 *
 * @return bytes written to @p out, or 0 if the SPS could not be rewritten.
 */
size_t coop_sps_double_height(const unsigned char *nal, size_t len,
                              unsigned char *out, size_t out_cap);

/** Side-by-side counterpart: doubles pic_width_in_luma_samples, leaves the height alone. */
size_t coop_sps_double_width(const unsigned char *nal, size_t len,
                             unsigned char *out, size_t out_cap);

/** Rewrite a PPS to declare TWO UNIFORM TILE COLUMNS with deblocking across tiles OFF.
 *  Returns 0 if the PPS already enables tiles. */
size_t coop_pps_enable_tiles(const unsigned char *nal, size_t len,
                             unsigned char *out, size_t out_cap);

/** As above but with an explicit uniform grid: (1,0) = two columns (vertical),
 *  (0,1) = two ROWS (horizontal). Deblocking across tiles is OFF either way. */
size_t coop_pps_enable_tiles_grid(const unsigned char *nal, size_t len,
                                  unsigned cols_minus1, unsigned rows_minus1,
                                  unsigned char *out, size_t out_cap);

/**
 * Turn a FIRST-slice header into a non-first slice at @p addr.
 *
 * The peer encodes its 1080 rows as a complete picture, so its slice says
 * `first_slice_segment_in_pic_flag = 1` and carries no address. To sit in the bottom
 * half of the composite it must say 0 and carry `slice_segment_address`, which only
 * EXISTS when that flag is clear -- so this inserts a field rather than patching one.
 *
 * @p p MUST be parsed from the COMPOSITE parameter sets: `slice_addr_bits` is derived
 * from PicSizeInCtbsY, which differs between the peer's 1088-row picture (12 bits) and
 * the 2176-row composite (13 bits). Using the peer's width shifts every following bit.
 *
 * @return bytes written to @p out, or 0 (including when it is already a non-first slice).
 *
 * BYTE ALIGNMENT IS THE WHOLE DIFFICULTY, and it is handled. Inserting the address adds
 * 13 bits, but a slice header ends with byte_alignment() precisely so the entropy-coded
 * data starts on a byte boundary. An earlier version copied the tail bit-for-bit, which
 * moved that data 5 bits; ffmpeg rejected it with "cu_qp_delta -51 is outside the valid
 * range" -- shifted bits, not a bad value, and a good reminder that a plausible-looking
 * error message can describe a completely different fault.
 *
 * So the header is walked in full (slice_header_end_bit), re-emitted with the inserted
 * field, given a FRESH byte_alignment(), and the slice data copied byte-wise from its
 * original boundary.
 *
 * Returns 0 -- refusing rather than emitting a bad picture -- for anything the walk does
 * not handle: dependent slices, long-term refs, list modification, weighted prediction,
 * tiles/WPP entry points, and slice-header extensions. None are produced by the NVENC
 * configuration in use, and each would silently mis-locate the header end if guessed.
 */
/**
 * Convert an IDR slice into a TRAIL_R slice carrying an I-slice, with an EMPTY reference
 * picture set.
 *
 * WHY THIS EXISTS. Splicing two hosts' panes into one coded picture couples them: the
 * peer's inter slices predict from the peer's PREVIOUS picture, so a dropped pane is a
 * HOLE that poisons everything after it, and recovering costs a re-anchor that needs BOTH
 * machines to emit a keyframe at once. That coupling -- not concealment, not the network --
 * is what makes the composite stutter, and it is structural.
 *
 * If the peer's slice never predicts, there is no chain to break: any pane can be spliced
 * against any picture of ours, in any order, with gaps, for free. HEVC allows this because
 * `slice_type` is PER SLICE while `nal_unit_type` is per picture -- so a TRAIL_R picture
 * may legally contain our P-slice AND the peer's I-slice. An IDR-encoded pane is already
 * the self-contained slice we want; it is only wearing the wrong nal_unit_type, because an
 * IRAP would force the whole composite picture to be an IRAP.
 *
 * @p poc must be the POC of the picture this slice is being spliced into.
 * @return bytes written, or 0 if the slice is not an IDR I-slice or the walk refuses it.
 */
size_t coop_slice_idr_to_trail(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                               unsigned poc, unsigned char *out, size_t out_cap);

/**
 * As above, but the converted slice declares ONE negative reference (the previous picture)
 * instead of an empty RPS.
 *
 * Use this when the converted IDR is slice 0. The decoder derives DPB state from the FIRST
 * slice, so an empty RPS there evicts the previous picture -- and a concealment slice
 * spliced in beside it then has no reference to copy from. That is exactly why an unpaired
 * IDR previously had to fall back to our-slice-alone, leaving the peer's half UNCODED and
 * poisoning its tile for the rest of an infinite-GOP session.
 *
 * The I-slice itself never reads a reference list, so the declaration costs it nothing; it
 * exists solely to keep the previous picture alive for the picture's OTHER slice.
 */
size_t coop_slice_idr_to_trail_keepref(const coop_poc_params_t *p, const unsigned char *nal,
                                       size_t len, unsigned poc,
                                       unsigned char *out, size_t out_cap);

size_t coop_slice_set_address(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                              unsigned addr, unsigned char *out, size_t out_cap);

/** Re-home a pane slice into a TILED composite: sets the address AND inserts the
 *  num_entry_point_offsets field that enabling tiles adds to the slice header. */
size_t coop_slice_to_tile(const coop_poc_params_t *src, const coop_poc_params_t *comp,
                          const unsigned char *nal, size_t len, unsigned addr,
                          unsigned char *out, size_t out_cap);

/** Read first_slice_segment_in_pic_flag and slice_segment_address from a slice. */
bool coop_slice_address(const coop_poc_params_t *p, const unsigned char *nal, size_t len,
                        bool *first_out, unsigned *addr_out);

/** Replace an EXISTING slice_segment_address (non-first slice of a multi-slice pane) with
 *  one sized by the composite. @p src walks the slice, @p comp writes the new address. */
size_t coop_slice_readdress(const coop_poc_params_t *src, const coop_poc_params_t *comp,
                            const unsigned char *nal, size_t len, unsigned addr,
                            unsigned char *out, size_t out_cap);

/** As above, but replaces the entropy-coded slice data with @p payload. Used to build a
 *  concealment slice: our own header (so POC/RPS match the picture) over synthesised
 *  CABAC data. @p payload NULL/0 behaves exactly like coop_slice_set_address. */
size_t coop_slice_set_address_payload(const coop_poc_params_t *p, const unsigned char *nal,
                                      size_t len, unsigned addr,
                                      const unsigned char *payload, size_t payload_len,
                                      unsigned char *out, size_t out_cap);

/**
 * CTB address at which the BOTTOM half of a composite picture starts.
 *
 * Must be DERIVED, never assumed: it is w_ctbs * (h_ctbs/2), so a 3840-wide composite
 * gives 120*34 = 4080 while a 1920-wide one gives 60*34 = 2040. Hard-coding 4080 made a
 * real decoder reject the stream with "Invalid slice segment address" on 1920 content --
 * 4080 is that picture's TOTAL CTB count, so it is one past the end.
 */
static inline unsigned coop_bottom_half_addr(const coop_poc_params_t *p) {
    return p->w_ctbs * (p->h_ctbs / 2u);
}

/** Bit position at which a slice header's fields end (before byte_alignment), or 0 if the
 *  walk cannot handle this slice. Exposed for offline probes that need to find where a real
 *  slice's entropy-coded payload begins. */
size_t coop_slice_header_end_bit_public(const coop_poc_params_t *p, const unsigned char *nal,
                                        size_t len);
