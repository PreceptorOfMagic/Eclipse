# test_coop_sps — SPS height-doubling round trip

    gcc -O1 -o /tmp/t_sps tests/coop/test_coop_sps.c tests/coop/test_coop_sps_shim.c \
        src/app/stream/video/coop_poc.c -Isrc/app/stream/video
    /tmp/t_sps tests/coop/skynet-4070-reference.h265

Half-height co-op needs the composed stream to declare 2176 rows while each host only
encodes 1088. `coop_sps_double_height()` does that, and this checks it against a REAL
capture rather than a synthetic SPS — the fields before the height (profile_tier_level
especially) are exactly where a hand-made SPS would diverge from what NVENC emits.

The shim re-parses the height with an INDEPENDENT reader, so a bug that corrupted both
the writer and the parser in the same way cannot pass.

Expected: `original height=1088  rewritten height=2176 ... PASS`.

# test_coop_slice_addr — slice_segment_address insertion

    gcc -O1 -o /tmp/t_addr tests/coop/test_coop_slice_addr.c \
        tests/coop/test_coop_slice_addr_shim.c src/app/stream/video/coop_poc.c \
        -Isrc/app/stream/video
    /tmp/t_addr tests/coop/skynet-4070-reference.h265

The peer encodes its rows as a complete picture, so its slice says
`first_slice_segment_in_pic_flag = 1` and carries NO address. Sitting in the bottom half
of the composite requires the flag clear and an address present -- a field that only
exists when the flag is clear, so this is an insertion, not a patch.

THE TRAP: `slice_addr_bits` is derived from PicSizeInCtbsY and therefore DIFFERS between
the source picture and the composite. Doubling the height always adds exactly one bit
(ceil(log2(2N)) == ceil(log2(N))+1), so the composite parameters must be used or every
following bit shifts.

Expected: `first_flag 1 -> 0  address=4080 ... PASS`.

# test_coop_intra — IDR slice -> TRAIL_R I-slice (the self-contained pane)

    gcc -O1 -o /tmp/t_intra tests/coop/test_coop_intra.c \
        src/app/stream/video/coop_poc.c src/app/stream/video/coop_splice.c \
        -Isrc/app/stream/video
    /tmp/t_intra tests/coop/skynet-4070-reference.h265

Converts the peer's IDR slice into a TRAIL_R I-slice with an EMPTY RPS, so its pane
references nothing and can be spliced against any picture of ours, in any order, with
gaps. An IDR carries `no_output_of_prior_pics_flag` (dropped); a non-IRAP carries
`slice_pic_order_cnt_lsb` + `st_ref_pic_set` + optional long-term/temporal-mvp fields
(inserted). Everything else is copied bit-for-bit and the tail re-aligned.

THE TRAP, and it cost a full build/deploy/TV cycle: `st_ref_pic_set(stRpsIdx)` is called
here with `stRpsIdx = num_short_term_ref_pic_sets`, and the spec gates
`inter_ref_pic_set_prediction_flag` on `stRpsIdx != 0`. NVENC declares 1 set, so that bit
is MANDATORY. Omitting it does not fall back to a default -- the decoder reads our
`num_negative_pics` ue(v) `1` AS the flag, takes the inter-prediction branch, and every
later field shifts, including the byte at which CABAC starts.

WHAT MAKES THIS TEST WORTH ANYTHING IS THE QP INVARIANT. `slice_qp_delta` sits after the
insertion and the conversion never touches it, so the converted slice must report the
source slice's QP; a single miscounted bit moves it. The broken version printed
`converted=26` against `original=42`. "Walkable" and "still an I-slice" are both decided
BEFORE the insertion and passed happily -- and so did ffmpeg, with 0 errors.

**AN ERROR COUNT IS NOT A DECODE.** ffmpeg reported 0 errors and frames `[I, P]` on the
broken bitstream while rendering the pane as a sliver of content over green, and the TV
rendered it as smeared colour. Judge this by pixels:

    ffmpeg -v error -i /tmp/coop-intra.h265 -vsync 0 -y /tmp/ff_%d.png

`ff_1.png` (bootstrap, proven splice path) and `ff_2.png` (the mixed P/I picture) must
carry the SAME amount of content in the bottom pane -- 136 of 270 sampled rows for this
capture, the rest green because the reference predates half-height and has 2 slices per
picture, so slice[0] covers only half a pane. Then put it in front of the TV's own
decoder, which is the only opinion that counts:

    scripts/tv/play_file_test.sh /tmp/coop-intra.h265 mixed 25

Expected: `qp invariant: original=42 converted=42 ... PASS`.
