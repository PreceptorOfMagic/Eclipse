#!/usr/bin/env python3
"""CAN AN AV1 TILE PAYLOAD BE MOVED INTO A DIFFERENT TILE SLOT?

That single question decides whether a VERTICAL (left/right) co-op split is possible at all.
HEVC cannot do it with one slice per pane -- slices are contiguous CTUs in RASTER order, so a
slice is always a horizontal band. AV1 has real TILE COLUMNS, and tiles are independent in
both entropy coding and intra prediction, which is the property the splice needs along the
axis we want.

THE TEST IS A SWAP WITHIN ONE FRAME, on purpose. Splicing two different streams would change
two things at once -- tile position AND frame header -- so a failure would not say which one
broke it. Swapping tile 0 and tile 1 of a SINGLE frame holds the header fixed by
construction and isolates position alone. If tiles are position-independent the picture comes
back mirrored left-to-right; if it decodes to garbage, relocation is not free and a
cross-stream splice cannot work either.

Finding the tile boundary without a full AV1 header parser: the tile group payload ends with
   [frame header ...][tile_size_minus_1 : TileSizeBytes LE][tile 0 data][tile 1 data]
(the LAST tile carries no size field). The header length is unknown, so candidate offsets are
scanned and accepted only on an EXACT fit -- size field + tile0 + tile1 consuming the payload
to the byte. That equality is a strong constraint, and any ambiguity is reported rather than
guessed at.

Usage: av1_tile_swap.py <in.obu> <out.obu>
"""
import sys


def leb128(buf, pos):
    val = 0
    for i in range(8):
        b = buf[pos + i]
        val |= (b & 0x7F) << (i * 7)
        if not (b & 0x80):
            return val, pos + i + 1
    raise ValueError("bad leb128")


def obus(buf):
    """Yield (type, payload_start, payload_len, obu_start, header_len)."""
    pos = 0
    while pos < len(buf):
        b = buf[pos]
        otype = (b >> 3) & 0x0F
        ext = (b >> 2) & 1
        has_size = (b >> 1) & 1
        p = pos + 1 + (1 if ext else 0)
        if not has_size:
            raise ValueError("OBU without size field -- not supported")
        size, p = leb128(buf, p)
        yield otype, p, size, pos, p - pos
        pos = p + size


def split_tiles(payload, tsb):
    """Split a 2-tile TILE_GROUP payload into (header, tile0, tile1).

    The layout is fixed, so nothing here is a search: with NumTiles > 1 the payload opens
    with tile_start_and_end_present_flag and byte_alignment -- exactly ONE byte when that
    flag is 0 -- then tile_size_minus_1 as TileSizeBytes little-endian, then tile 0, then
    tile 1. The LAST tile carries no size field.

    An earlier version of this script tried to FIND the boundary by scanning offsets and
    accepting an "exact fit". That test was vacuous: tile1 was defined as rest - tile0, so
    the fit always held and 7311 offsets looked equally valid. There is no redundancy in the
    format to exploit -- the structure has to be known, not guessed.

    TileSizeBytes is tile_size_bytes_minus_1 + 1 from the frame header's tile_info(), which
    ffmpeg's trace_headers does not surface, so it is passed in. Only 1..4 are legal and a
    wrong value fails the decode loudly (dav1d: "Invalid data found"), so trying them is
    cheap and unambiguous. NVENC emits 4 here.
    """
    s0 = int.from_bytes(payload[1:1 + tsb], "little") + 1
    t0 = payload[1 + tsb:1 + tsb + s0]
    t1 = payload[1 + tsb + s0:]
    if len(t0) != s0 or len(t1) == 0:
        return None
    return payload[:1], t0, t1


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    tsb = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    buf = bytearray(open(sys.argv[1], "rb").read())
    out = bytearray()
    swapped = 0
    for otype, pstart, plen, ostart, hlen in obus(bytes(buf)):
        # 4 = OBU_TILE_GROUP (tiles only; the frame header is a separate type-3 OBU, which
        # is what NVENC emits). 6 = OBU_FRAME, header and tile group combined.
        if otype not in (4, 6):
            out += buf[ostart:pstart + plen]
            continue
        payload = bytes(buf[pstart:pstart + plen])
        got = split_tiles(payload, tsb)
        if got is None:
            print(f"  tile group ({plen} B): does not split at TileSizeBytes={tsb}, unchanged")
            out += buf[ostart:pstart + plen]
            continue
        hdr, t0, t1 = got
        if swapped == 0:
            print(f"  TileSizeBytes={tsb}: tile0 {len(t0)} B, tile1 {len(t1)} B "
                  f"(payload {plen} B)")
        newpay = hdr + (len(t1) - 1).to_bytes(tsb, "little") + t1 + t0
        assert len(newpay) == len(payload), "payload length must not change"
        out += buf[ostart:pstart] + newpay
        swapped += 1
    open(sys.argv[2], "wb").write(bytes(out))
    print(f"wrote {sys.argv[2]}: {swapped} tile group(s) swapped")
    return 0 if swapped else 1


if __name__ == "__main__":
    sys.exit(main())
