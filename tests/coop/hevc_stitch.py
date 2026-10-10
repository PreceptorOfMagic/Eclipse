#!/usr/bin/env python3
"""hevc_stitch — merge two HEVC streams into one by taking a different slice from each.

The co-op split-screen idea: two PCs each encode the SAME picture geometry as two
rectangular slices, each rendering only its own half (the other half black). If the
encoder makes slices genuinely independent (NVENC's constrained encoding), slice 0
of PC1 and slice 1 of PC2 can be spliced into a single access unit -- one bitstream,
so the TV needs only ONE decode pipeline, which is the resource we proved is capped
at one per app.

This tool does that splice offline so the seam can be inspected before anything is
built. It is deliberately dumb: no re-encoding, no header rewriting -- both inputs
must come from identical encoder settings so their parameter sets and slice headers
already agree.

Usage:
  hevc_stitch.py analyze A.hevc
  hevc_stitch.py stitch A.hevc B.hevc out.hevc
"""
import sys

VCL_MAX = 31          # nal_unit_type 0..31 are VCL (slice) NALs
IRAP_RANGE = range(16, 24)


def nal_type(nal: bytes) -> int:
    return (nal[0] >> 1) & 0x3F if nal else -1


def first_slice_in_pic(nal: bytes) -> bool:
    """first_slice_segment_in_pic_flag is the first bit after the 2-byte NAL header."""
    return bool(nal[2] & 0x80) if len(nal) > 2 else False


def split_nals(data: bytes):
    """Yield NAL payloads (without start codes) from an Annex-B stream."""
    starts = []
    i = 0
    n = len(data)
    while i < n - 3:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append((i, 3))
                i += 3
                continue
            if i < n - 4 and data[i + 2] == 0 and data[i + 3] == 1:
                starts.append((i, 4))
                i += 4
                continue
        i += 1
    for idx, (pos, sclen) in enumerate(starts):
        end = starts[idx + 1][0] if idx + 1 < len(starts) else n
        yield data[pos + sclen:end]


def access_units(nals):
    """Group NALs into access units. A new AU begins at a VCL NAL whose
    first_slice_segment_in_pic_flag is set; non-VCL NALs attach to the AU ahead."""
    au, pending = [], []
    for nal in nals:
        t = nal_type(nal)
        if t <= VCL_MAX:
            if first_slice_in_pic(nal) and au:
                yield au
                au = []
            if pending:
                au.extend(pending)
                pending = []
            au.append(nal)
        else:
            if au:
                pending.append(nal)
            else:
                au.append(nal)
    if au:
        yield au + pending


def analyze(path):
    data = open(path, 'rb').read()
    aus = list(access_units(split_nals(data)))
    print(f"{path}: {len(aus)} access units")
    counts = {}
    for au in aus:
        slices = [n for n in au if nal_type(n) <= VCL_MAX]
        counts[len(slices)] = counts.get(len(slices), 0) + 1
    for k in sorted(counts):
        print(f"  {k} slice(s)/picture : {counts[k]} pictures")
    first = aus[0]
    print("  first AU NAL types:", [nal_type(n) for n in first])
    irap = sum(1 for au in aus
               if any(nal_type(n) in IRAP_RANGE for n in au))
    print(f"  IRAP (keyframe) pictures: {irap}")
    return aus


def stitch(path_a, path_b, out_path):
    aus_a = list(access_units(split_nals(open(path_a, 'rb').read())))
    aus_b = list(access_units(split_nals(open(path_b, 'rb').read())))
    n = min(len(aus_a), len(aus_b))
    print(f"stitching {n} access units (A={len(aus_a)} B={len(aus_b)})")

    out = bytearray()
    sc = b'\x00\x00\x00\x01'
    used_b = 0
    for i in range(n):
        au_a, au_b = aus_a[i], aus_b[i]
        # Non-VCL (VPS/SPS/PPS/SEI) come from A; both encoders were configured
        # identically, so A's parameter sets describe B's slices correctly too.
        for nal in au_a:
            if nal_type(nal) > VCL_MAX:
                out += sc + nal
        slices_a = [x for x in au_a if nal_type(x) <= VCL_MAX]
        slices_b = [x for x in au_b if nal_type(x) <= VCL_MAX]
        # slice 0 (top half) from A
        if slices_a:
            out += sc + slices_a[0]
        # slice 1 (bottom half) from B -- the actual splice
        if len(slices_b) > 1:
            out += sc + slices_b[1]
            used_b += 1
        elif len(slices_a) > 1:
            out += sc + slices_a[1]
    open(out_path, 'wb').write(bytes(out))
    print(f"wrote {out_path} ({len(out)} bytes); {used_b}/{n} pictures took slice 1 from B")


def params(path):
    """Print VPS/SPS/PPS bytes. Two hosts must agree on these for their slices
    to be spliceable: the splice keeps stream A's parameter sets and discards
    B's, so B's slices are decoded under A's SPS. When they differed by only
    colour metadata in testing, slices decoded WRONG (27dB) rather than failing
    cleanly -- so this must be checked between different encoder generations
    (e.g. Ampere vs Ada NVENC) before trusting a split picture."""
    data = open(path, 'rb').read()
    seen = {}
    for nal in split_nals(data):
        t = nal_type(nal)
        if t in (32, 33, 34) and t not in seen:
            seen[t] = nal
        if len(seen) == 3:
            break
    for t, label in ((32, 'VPS'), (33, 'SPS'), (34, 'PPS')):
        n = seen.get(t, b'')
        print(f"{label} len={len(n):3d} {n.hex()}")
    return seen


def compare(path_a, path_b):
    print(f"--- A: {path_a}")
    a = params(path_a)
    print(f"\n--- B: {path_b}")
    b = params(path_b)
    print()
    ok = True
    for t, label in ((32, 'VPS'), (33, 'SPS'), (34, 'PPS')):
        same = a.get(t) == b.get(t)
        ok &= same
        print(f"{label}: {'MATCH' if same else 'DIFFER'}")
    print("\n*** SPLICEABLE: parameter sets identical ***" if ok else
          "\n*** NOT SPLICEABLE AS-IS: parameter sets differ -> B's slices would "
          "decode under A's SPS and corrupt. Match encoder settings, or rewrite headers. ***")
    return ok


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'analyze':
        analyze(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'params':
        params(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == 'compare':
        sys.exit(0 if compare(sys.argv[2], sys.argv[3]) else 1)
    elif len(sys.argv) >= 5 and sys.argv[1] == 'stitch':
        stitch(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(__doc__)
        sys.exit(1)
