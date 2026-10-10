/*
 * Co-op concealment: build a slice that reproduces the previous picture's bottom half
 * exactly, for the pictures where the peer has no frame to contribute.
 *
 * See coop_conceal.c for why an all-skip slice is the right answer and what the two
 * alternatives (uncoded, or repeating the last peer slice) actually cost.
 */
#pragma once

#include "coop_poc.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * CABAC-encode an all-skip slice payload covering CTBs [start_ctb, PicSizeInCtbsY).
 *
 * @return bytes written to @p out, or 0 if the slice type has no skip syntax (I slices)
 *         or the buffer was too small. The output is the entropy-coded slice DATA only:
 *         it still needs a slice header in front of it.
 */
size_t coop_build_skip_slice_data(const coop_poc_params_t *p, const coop_slice_info_t *si,
                                  unsigned start_ctb, unsigned char *out, size_t cap);

/**
 * Full concealment slice: OUR slice's header re-addressed to @p addr, over synthesised
 * all-skip data. Taking our own header is what keeps POC, the reference picture set and
 * the temporal-MVP flag identical across the picture's two slices, which HEVC requires.
 *
 * @param scratch working buffer for the entropy-coded payload (a few KB is ample: an
 *                all-skip 3840x1088 slice is on the order of 1-2 KB).
 * @return bytes written to @p out, or 0 if a concealment slice cannot be built.
 */
size_t coop_build_skip_slice(const coop_poc_params_t *p, const unsigned char *our_slice,
                             size_t our_len, unsigned addr,
                             unsigned char *scratch, size_t scratch_cap,
                             unsigned char *out, size_t out_cap);
