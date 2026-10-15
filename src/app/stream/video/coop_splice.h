/*
 * Co-op split-screen: HEVC access-unit splicing.
 *
 * Two PCs each stream the SAME picture geometry, each rendering only its own
 * half (the other half black, with a guard band across the slice boundary).
 * Both encode 2 slices per picture, which the client already requests via
 * CAPABILITY_SLICES_PER_FRAME -- verified live against stock Apollo, so no
 * host-side fork is required.
 *
 * This takes slice 0 from stream A and slice 1 from stream B and emits a single
 * access unit. The result feeds the ONE existing decoder through the ONE
 * existing video plane, which is the only path that renders on this TV.
 *
 * The approach is validated: tests/coop/hevc_stitch.py produced bit-perfect
 * panes from real encoder output with a 320px guard band.
 *
 * No re-encoding and no header rewriting -- both inputs must come from
 * identical encoder settings so their parameter sets and slice headers already
 * agree.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "coop_poc.h"

/** Parameter sets and slices located within one access unit. */
typedef struct {
    const unsigned char *slice[2];  /**< slice payloads, excluding start codes */
    size_t slice_len[2];
    int slice_type[2];              /**< nal_unit_type of each slice */
    int slice_count;
    const unsigned char *non_vcl;   /**< VPS/SPS/PPS/SEI run, if present */
    size_t non_vcl_len;
} coop_au_t;

/**
 * Parse an Annex-B access unit into its non-VCL prefix and up to two slices.
 * Returns false if the buffer holds no VCL NAL.
 */
bool coop_au_parse(const unsigned char *data, size_t len, coop_au_t *out);

/**
 * Splice slice 0 of @p a with slice 1 of @p b into @p out.
 * Parameter sets are taken from @p a. Returns the bytes written, or 0 if the
 * inputs cannot be spliced (missing slices, or @p out too small).
 */
size_t coop_au_splice(const coop_au_t *a, const coop_au_t *b,
                      unsigned char *out, size_t out_cap);

/**
 * Copy a non-VCL run, rewriting the SPS to declare twice the height.
 * @return bytes written, or 0 if the run is malformed or the SPS could not be rewritten.
 */
size_t coop_nonvcl_double_height(const unsigned char *in, size_t len,
                                 unsigned char *out, size_t cap);

/** Most CTB rows a pane may have. NVENC caps a picture at 64 slices, and the vertical
 *  split needs exactly one slice per CTB row, so a pane is at most 64 rows = 2048px. */
#define COOP_MAX_ROWS 64

/** One pane picture as a list of slices, in CTB-row order (slice r covers CTB row r). */
typedef struct {
    const unsigned char *slice[COOP_MAX_ROWS];
    size_t slice_len[COOP_MAX_ROWS];
    unsigned n;
} coop_pane_pic_t;

/**
 * VERTICAL (left/right) splice: interleave two panes' slices by CTB row into one picture.
 *
 * Composite row r is A's row at CTB address r*Wc and B's at r*Wc + Wp, each Wp CTBs wide.
 * Both are contiguous runs in raster order, which is the only reason a side-by-side split
 * is expressible in HEVC at all -- a slice covering a whole pane could only ever be a
 * horizontal band.
 *
 * REQUIRES ONE SLICE PER CTB ROW in both panes (a->n == b->n == composite->h_ctbs). A slice
 * spanning two rows is not contiguous in the composite, so an approximate slice count is
 * not a degraded version of this -- it simply does not work.
 *
 * REQUIRES CONSTRAINED ENCODING on both panes. Without it inter prediction fetches across
 * the pane boundary and contaminates ~256 columns, growing until the next IDR. With it the
 * panes splice BIT-EXACT (measured: psnr_y = inf on every picture).
 *
 * If B is an IDR while A is not, B's slices are converted to TRAIL_R I-slices so they can
 * ride in A's non-IRAP picture -- every VCL NAL of one picture must share nal_unit_type.
 * The reverse (A an IRAP, B not) cannot be fixed and returns 0, as the horizontal path
 * also refuses it.
 *
 * @return bytes written, or 0 if the panes cannot be spliced.
 */
size_t coop_au_splice_columns(const coop_pane_pic_t *a, const coop_pane_pic_t *b,
                              const coop_poc_params_t *src,
                              const coop_poc_params_t *composite,
                              const unsigned char *nonvcl, size_t nonvcl_len,
                              unsigned char *out, size_t out_cap);

/** Composite parameter sets for the TILE-based vertical split: SPS width doubled AND the
 *  PPS given two tile columns. */
size_t coop_nonvcl_tile_columns(const unsigned char *in, size_t len,
                                unsigned char *out, size_t cap);

/** Composite parameter sets for the TILE-based HORIZONTAL split: SPS height doubled AND the
 *  PPS given two tile ROWS. A full-width tile row's scan is the picture's raster scan over that
 *  row range, so a pane's slice keeps its address unchanged. */
size_t coop_nonvcl_tile_rows(const unsigned char *in, size_t len,
                             unsigned char *out, size_t cap);

/** Side-by-side counterpart: rewrites the SPS width instead of the height. */
size_t coop_nonvcl_double_width(const unsigned char *in, size_t len,
                                unsigned char *out, size_t cap);

/**
 * Compose two HALF-HEIGHT access units into one full-height picture.
 *
 * Ours is used unchanged (already the first slice at address 0); the peer's is relocated
 * to @p peer_addr. @p composite MUST be parsed from the doubled parameter sets, and
 * @p nonvcl is that already-rewritten run.
 *
 * @return bytes written, or 0 if the inputs cannot be composed.
 */
/** HORIZONTAL, TILED: compose two half-height AUs into one two-tile-row picture. Both halves
 *  are re-homed with coop_slice_to_tile(), because enabling tiles adds num_entry_point_offsets
 *  to EVERY slice header including ours. @p composite must come from coop_nonvcl_tile_rows(). */
size_t coop_au_splice_tile_rows(const coop_au_t *a, const coop_au_t *b,
                                const coop_poc_params_t *a_src,
                                const coop_poc_params_t *b_src,
                                const coop_poc_params_t *composite, unsigned peer_addr,
                                const unsigned char *nonvcl, size_t nonvcl_len,
                                unsigned char *out, size_t cap);

size_t coop_au_splice_halves(const coop_au_t *a, const coop_au_t *b,
                             const coop_poc_params_t *composite, unsigned peer_addr,
                             const unsigned char *nonvcl, size_t nonvcl_len,
                             unsigned char *out, size_t cap);

/** Re-emit a ONE-COLUMN tiled composite (two tile rows) untiled: same picture, no tiles, for decoders
 *  that mishandle tile rows. @p params caches the tiled parameter sets across AUs. Returns bytes
 *  written, or 0 (feed the original) if anything is refused. */
size_t coop_au_untile(const unsigned char *au, size_t len, coop_poc_params_t *params,
                      unsigned char *out, size_t out_cap);
