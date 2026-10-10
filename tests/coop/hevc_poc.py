#!/usr/bin/env python3
"""hevc_poc — read slice_pic_order_cnt_lsb out of HEVC slice headers.

Why this exists: the co-op splice puts slice 0 from one host and slice 1 from
another into ONE picture. HEVC requires every slice of a picture to carry the
same slice_pic_order_cnt_lsb, and each slice's reference list is derived from
its OWN header -- so if the two hosts disagree, the spliced slice references
POCs that are not in the decoder's buffer and gets discarded. That is the
suspected reason the second pane only ever showed its bootstrap IDR.

This measures it instead of assuming it, and is the reference implementation
for the C rewriter (coop_poc.c): same field walk, same bit offsets.

Usage:
  hevc_poc.py params  SPS.hevc            # decoded parameter-set fields
  hevc_poc.py poc     stream.hevc [N]     # per-picture slice POCs
  hevc_poc.py check   stream.hevc [N]     # flag pictures whose slices disagree
  hevc_poc.py geometry stream.hevc        # pane boundary and heights from slice addresses
"""
import sys

from hevc_stitch import split_nals, access_units, nal_type, VCL_MAX

IDR_TYPES = (19, 20)          # IDR_W_RADL, IDR_N_LP -- carry no POC field
IRAP_RANGE = range(16, 24)


def rbsp(nal: bytes) -> bytes:
    """Strip emulation-prevention bytes: 00 00 03 -> 00 00."""
    out = bytearray()
    zeros = 0
    for b in nal:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue          # drop the 03
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


class Bits:
    def __init__(self, data: bytes):
        self.d = data
        self.pos = 0          # bit position

    def u(self, n: int) -> int:
        v = 0
        for _ in range(n):
            byte = self.d[self.pos >> 3]
            bit = (byte >> (7 - (self.pos & 7))) & 1
            v = (v << 1) | bit
            self.pos += 1
        return v

    def ue(self) -> int:
        """Exp-Golomb. Leading zeros count, then that many suffix bits."""
        lz = 0
        while self.u(1) == 0:
            lz += 1
            if lz > 32:
                raise ValueError("runaway exp-golomb")
        return (1 << lz) - 1 + (self.u(lz) if lz else 0)

    def se(self) -> int:
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)


def parse_ptl(b: Bits, max_sub_layers_minus1: int):
    """profile_tier_level with profilePresentFlag=1."""
    b.u(2 + 1 + 5)                       # space, tier, profile_idc
    b.u(32)                              # compatibility flags
    b.u(4)                               # progressive/interlaced/non-packed/frame-only
    b.u(43)                              # reserved
    b.u(1)                               # reserved
    b.u(8)                               # general_level_idc
    sub_profile, sub_level = [], []
    for _ in range(max_sub_layers_minus1):
        sub_profile.append(b.u(1))
        sub_level.append(b.u(1))
    if max_sub_layers_minus1 > 0:
        for _ in range(max_sub_layers_minus1, 8):
            b.u(2)                       # reserved_zero_2bits
    for i in range(max_sub_layers_minus1):
        if sub_profile[i]:
            b.u(2 + 1 + 5)
            b.u(32)
            b.u(4)
            b.u(43)
            b.u(1)
        if sub_level[i]:
            b.u(8)


def parse_sps(nal: bytes) -> dict:
    b = Bits(rbsp(nal)[2:])              # skip the 2-byte NAL header
    b.u(4)                               # sps_video_parameter_set_id
    max_sub = b.u(3)                     # sps_max_sub_layers_minus1
    b.u(1)                               # sps_temporal_id_nesting_flag
    parse_ptl(b, max_sub)
    b.ue()                               # sps_seq_parameter_set_id
    chroma = b.ue()
    separate_colour_plane = b.u(1) if chroma == 3 else 0
    width = b.ue()
    height = b.ue()
    if b.u(1):                           # conformance_window_flag
        b.ue(); b.ue(); b.ue(); b.ue()
    b.ue()                               # bit_depth_luma_minus8
    b.ue()                               # bit_depth_chroma_minus8
    log2_max_poc_lsb = b.ue() + 4
    sub_layer_ordering = b.u(1)
    for _ in range(0 if sub_layer_ordering else max_sub, max_sub + 1):
        b.ue(); b.ue(); b.ue()
    log2_min_cb = b.ue() + 3
    log2_diff_max_min_cb = b.ue()
    ctb_log2 = log2_min_cb + log2_diff_max_min_cb
    ctb = 1 << ctb_log2
    w_ctbs = -(-width // ctb)
    h_ctbs = -(-height // ctb)
    size_ctbs = w_ctbs * h_ctbs
    addr_bits = max(1, (size_ctbs - 1).bit_length())
    return {
        "width": width, "height": height,
        "log2_max_poc_lsb": log2_max_poc_lsb,
        "separate_colour_plane": separate_colour_plane,
        "ctb": ctb, "pic_size_in_ctbs": size_ctbs,
        "slice_addr_bits": addr_bits,
    }


def parse_pps(nal: bytes) -> dict:
    b = Bits(rbsp(nal)[2:])
    b.ue()                               # pps_pic_parameter_set_id
    b.ue()                               # pps_seq_parameter_set_id
    dependent = b.u(1)
    output_flag = b.u(1)
    extra_bits = b.u(3)
    return {"dependent_slice_segments": dependent,
            "output_flag_present": output_flag,
            "num_extra_slice_header_bits": extra_bits}


def poc_bit_offset(sps: dict, pps: dict, nal: bytes):
    """Walk the slice header and return (bit offset of slice_pic_order_cnt_lsb,
    rbsp bytes), or (None, rbsp) when the slice carries no POC field (IDR)."""
    t = nal_type(nal)
    r = rbsp(nal)
    b = Bits(r[2:])
    first = b.u(1)
    if t in IRAP_RANGE:
        b.u(1)                           # no_output_of_prior_pics_flag
    b.ue()                               # slice_pic_parameter_set_id
    dependent = 0
    if not first:
        if pps["dependent_slice_segments"]:
            dependent = b.u(1)
        b.u(sps["slice_addr_bits"])      # slice_segment_address
    if dependent:
        return None, r                   # dependent slices inherit the header
    b.u(pps["num_extra_slice_header_bits"])
    b.ue()                               # slice_type
    if pps["output_flag_present"]:
        b.u(1)                           # pic_output_flag
    if sps["separate_colour_plane"]:
        b.u(2)                           # colour_plane_id
    if t in IDR_TYPES:
        return None, r                   # IDR has no POC field
    return b.pos + 16, r                 # +16: the 2-byte NAL header skipped above


def read_slice_address(sps, pps, nal):
    """slice_segment_address, i.e. the CTU index this slice starts at. This is
    what actually determines where the pane boundary lands -- it is CTU-aligned,
    so at 4K (2160/32 = 67.5 -> 68 CTU rows) it cannot be the midpoint."""
    t = nal_type(nal)
    r = rbsp(nal)
    b = Bits(r[2:])
    first = b.u(1)
    if first:
        return 0
    if t in IRAP_RANGE:
        b.u(1)
    b.ue()
    if pps["dependent_slice_segments"]:
        b.u(1)
    return b.u(sps["slice_addr_bits"])


def read_poc(sps, pps, nal):
    off, r = poc_bit_offset(sps, pps, nal)
    if off is None:
        return None
    b = Bits(r)
    b.pos = off
    return b.u(sps["log2_max_poc_lsb"])


def emulate(payload: bytes) -> bytes:
    """Re-insert emulation-prevention bytes: any 00 00 followed by 00..03 gets
    an 03 wedged in. Inverse of rbsp(). Patching a POC byte can create OR destroy
    such a pattern, so a rewritten slice must always round-trip through this."""
    out = bytearray()
    zeros = 0
    for b in payload:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def write_poc(sps, pps, nal: bytes, value: int):
    """Return the NAL with slice_pic_order_cnt_lsb replaced by `value`, or None
    if it carries no POC field. The field is FIXED WIDTH, so the header length
    is unchanged and the CABAC payload never shifts -- the whole point of
    rewriting this particular field. Reference for coop_poc_rewrite() in C."""
    off, r = poc_bit_offset(sps, pps, nal)
    if off is None:
        return None
    width = sps["log2_max_poc_lsb"]
    r = bytearray(r)
    for i in range(width):
        bit = (value >> (width - 1 - i)) & 1
        pos = off + i
        mask = 0x80 >> (pos & 7)
        if bit:
            r[pos >> 3] |= mask
        else:
            r[pos >> 3] &= ~mask & 0xFF
    return bytes(r[:2]) + emulate(bytes(r[2:]))


def load(path):
    data = open(path, 'rb').read()
    nals = list(split_nals(data))
    sps = pps = None
    for n in nals:
        if nal_type(n) == 33 and sps is None:
            sps = parse_sps(n)
        elif nal_type(n) == 34 and pps is None:
            pps = parse_pps(n)
    if sps is None or pps is None:
        raise SystemExit("stream has no SPS/PPS")
    return sps, pps, list(access_units(nals))


def cmd_params(path):
    sps, pps, aus = load(path)
    for k, v in sps.items():
        print(f"  sps.{k} = {v}")
    for k, v in pps.items():
        print(f"  pps.{k} = {v}")
    print(f"  access units = {len(aus)}")


def cmd_poc(path, limit=12):
    sps, pps, aus = load(path)
    print(f"{path}: log2_max_poc_lsb={sps['log2_max_poc_lsb']} "
          f"slice_addr_bits={sps['slice_addr_bits']}")
    for i, au in enumerate(aus[:limit]):
        slices = [n for n in au if nal_type(n) <= VCL_MAX]
        pocs = [read_poc(sps, pps, n) for n in slices]
        types = [nal_type(n) for n in slices]
        print(f"  AU {i:3d}  types={types}  poc={pocs}")


def cmd_geometry(path):
    sps, pps, aus = load(path)
    ctb = sps["ctb"]
    w_ctbs = -(-sps["width"] // ctb)
    h_ctbs = -(-sps["height"] // ctb)
    print(f"{path}")
    print(f"  picture      : {sps['width']}x{sps['height']}  CTB={ctb}  "
          f"grid={w_ctbs}x{h_ctbs} ({w_ctbs*h_ctbs} CTBs)")
    for au in aus:
        slices = [n for n in au if nal_type(n) <= VCL_MAX]
        if len(slices) < 2:
            continue
        addrs = [read_slice_address(sps, pps, n) for n in slices]
        rows = [a // w_ctbs for a in addrs]
        ys = [r * ctb for r in rows]
        heights = [ys[i + 1] - ys[i] for i in range(len(ys) - 1)]
        heights.append(sps["height"] - ys[-1])
        print(f"  slice addrs  : {addrs}")
        print(f"  boundary y   : {ys[1:]}")
        print(f"  pane heights : {heights}")
        return
    print("  (no 2-slice picture found)")


def cmd_check(path, limit=200):
    sps, pps, aus = load(path)
    bad = same = idr = 0
    for au in aus[:limit]:
        slices = [n for n in au if nal_type(n) <= VCL_MAX]
        if len(slices) < 2:
            continue
        pocs = [read_poc(sps, pps, n) for n in slices]
        if any(p is None for p in pocs):
            idr += 1
        elif len(set(pocs)) == 1:
            same += 1
        else:
            bad += 1
    print(f"{path}: pictures with matching slice POCs = {same}, "
          f"MISMATCHED = {bad}, IDR (no POC field) = {idr}")
    return bad


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'params':
        cmd_params(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'poc':
        cmd_poc(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 12)
    elif len(sys.argv) >= 3 and sys.argv[1] == 'geometry':
        cmd_geometry(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'check':
        cmd_check(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 200)
    else:
        print(__doc__)
        sys.exit(1)
