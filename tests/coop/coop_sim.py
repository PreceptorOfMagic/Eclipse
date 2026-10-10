#!/usr/bin/env python3
"""coop_sim — replay the client's splice POLICY offline, with a lossy pane feed.

The live client pairs slice 0 from our host with slice 1 from the other PC. In
practice only 30-80% of pictures get a fresh pane (the other host delivers fewer
frames), and what we emit on the misses decides whether the other pane survives:

  own     what the client did originally -- emit OUR slice 1. That writes THIS
          host's content into the bottom of the decoder's reference picture, so
          the other host's next inter slice predicts from the wrong pixels.
  repeat  re-send the PREVIOUS pane with its POC rewritten to the current
          picture. Its references then resolve to the previous composite, whose
          bottom half already holds the other host's content, so the pane keeps
          its own prediction chain (its motion is applied twice = mild drift).

Deciding between them on the TV costs a full two-host session and the user's
eyes; this decides it from two ordinary captures instead. The pane feed is
modelled as "B runs SLOWER than A": B's frame pointer only advances on a
delivered picture, which is exactly why panes go missing live.

Usage:
  coop_sim.py selftest A.hevc              # POC writer round-trips
  coop_sim.py sim A.hevc B.hevc OUTDIR [--rate 0.6] [--seed 1]
        -> OUTDIR/own.hevc, OUTDIR/repeat.hevc, OUTDIR/map.txt
"""
import os
import random
import sys

from hevc_stitch import split_nals, access_units, nal_type, VCL_MAX
from hevc_poc import load, read_poc, write_poc, IDR_TYPES, IRAP_RANGE

SC = b'\x00\x00\x00\x01'


def selftest(path):
    """Rewriting a slice's POC to the value it already holds must be
    BYTE-IDENTICAL. That is the check that actually exercises the
    emulation-prevention round trip -- a naive writer passes a POC read/write
    test but corrupts NALs whose patched byte creates or destroys an 00 00 03."""
    sps, pps, aus = load(path)
    checked = skipped = 0
    for au in aus:
        for nal in au:
            if nal_type(nal) > VCL_MAX:
                continue
            poc = read_poc(sps, pps, nal)
            if poc is None:
                skipped += 1
                continue
            again = write_poc(sps, pps, nal, poc)
            if again != nal:
                raise SystemExit(f"FAIL: identity rewrite changed the NAL "
                                 f"(poc={poc}, {len(nal)} -> {len(again)} bytes)")
            if read_poc(sps, pps, write_poc(sps, pps, nal, (poc + 7) % 256)) != (poc + 7) % 256:
                raise SystemExit(f"FAIL: rewritten POC does not read back (poc={poc})")
            checked += 1
    print(f"selftest OK: {checked} slices round-tripped byte-identical "
          f"({skipped} IDR slices have no POC field)")


def slices_of(au):
    return [n for n in au if nal_type(n) <= VCL_MAX]


def sim(path_a, path_b, outdir, rate=0.6, seed=1):
    sps, pps, aus_a = load(path_a)
    _, _, aus_b = load(path_b)
    os.makedirs(outdir, exist_ok=True)

    rnd = random.Random(seed)
    out = {p: bytearray() for p in ("own", "repeat", "rebootstrap")}
    stats = {k: dict(paired=0, fallback=0, repeated=0, poc_fail=0, reboot=0) for k in out}
    lastpane = {k: None for k in out}
    boot = {k: False for k in out}
    # Sticky IDR pane, per policy: "rebootstrap" DROPS it when it asks the other
    # host for a fresh keyframe, so it cannot splice a pane from seconds ago.
    sticky = {k: None for k in out}
    j = 0                                          # B's frame pointer
    mapping = []

    for i, au_a in enumerate(aus_a):
        sa = slices_of(au_a)
        if len(sa) < 2:
            # The leading AU carries VPS/SPS/PPS and no slices. Dropping it left
            # the composite headerless ("VPS 0 does not exist") -- pass it through.
            for policy in out:
                for nal in au_a:
                    out[policy] += SC + nal
            continue
        my_type = nal_type(sa[0])
        my_irap = my_type in IRAP_RANGE
        my_poc = read_poc(sps, pps, sa[0])

        # Pane delivery. B only advances when a pane is delivered, i.e. B is
        # producing fewer pictures than A -- the live failure mode.
        delivered = j < len(aus_b) and (rnd.random() < rate or my_irap)
        pane = None
        if delivered:
            while j < len(aus_b) and len(slices_of(aus_b[j])) < 2:
                j += 1
            if j < len(aus_b):
                pane = slices_of(aus_b[j])[1]
                if nal_type(pane) in IDR_TYPES:
                    for k in sticky:
                        sticky[k] = pane
                j += 1
            else:
                delivered = False
        mapping.append((i, j - 1 if delivered else -1))

        for policy in out:
            buf = out[policy]
            st = stats[policy]
            for nal in au_a:                       # VPS/SPS/PPS/SEI from A
                if nal_type(nal) > VCL_MAX:
                    buf += SC + nal
            buf += SC + sa[0]

            use = pane
            # BOOTSTRAP: the pane can only take over at an IRAP, and by the time
            # our host IRAPs the other PC's IDR is long gone from the latest
            # slot -- hence the sticky IDR the live coordinator keeps.
            if not boot[policy]:
                if my_irap and sticky[policy] is not None:
                    use = sticky[policy]
                else:
                    use = None

            emitted = None
            if use is not None and nal_type(use) in IDR_TYPES and my_irap:
                emitted = use                      # IDR pair: no POC field to align
                boot[policy] = True
            elif use is not None and my_poc is not None and nal_type(use) == my_type:
                rw = write_poc(sps, pps, use, my_poc)
                if rw is None:
                    st["poc_fail"] += 1
                else:
                    emitted = rw

            if emitted is not None and use is pane:
                st["paired"] += 1
                lastpane[policy] = pane
            elif emitted is not None:
                st["paired"] += 1                  # bootstrap picture
            else:
                st["fallback"] += 1
                if (policy == "repeat" and boot[policy] and lastpane[policy] is not None
                        and my_poc is not None and nal_type(lastpane[policy]) == my_type):
                    rw = write_poc(sps, pps, lastpane[policy], my_poc)
                    if rw is not None:
                        emitted = rw
                        st["repeated"] += 1
                if emitted is None:
                    emitted = sa[1]                # our own bottom half
            # OUR IRAP WIPES THE PANE: this picture is ours in both slices, so the
            # other host's chain is broken and its later inter slices predict from
            # our content. Re-arm the bootstrap and drop the sticky IDR (the live
            # client also asks the other host for a fresh one over the IPC control
            # channel; here we simply wait for its next natural IDR, which is the
            # pessimistic case).
            if (policy == "rebootstrap" and my_irap and boot[policy]
                    and emitted is sa[1]):
                boot[policy] = False
                sticky[policy] = None
                lastpane[policy] = None
                st["reboot"] += 1
            buf += SC + emitted

    for policy in out:
        p = os.path.join(outdir, policy + ".hevc")
        open(p, 'wb').write(bytes(out[policy]))
        s = stats[policy]
        print(f"{p}: paired={s['paired']} fallback={s['fallback']} "
              f"repeated={s['repeated']} reboot={s['reboot']} "
              f"poc_fail={s['poc_fail']} ({len(out[policy])} bytes)")
    with open(os.path.join(outdir, "map.txt"), 'w') as f:
        for i, bj in mapping:
            f.write(f"{i} {bj}\n")
    print(f"wrote {outdir}/map.txt ({len(mapping)} pictures; -1 = no pane that picture)")


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'selftest':
        selftest(sys.argv[2])
    elif len(sys.argv) >= 5 and sys.argv[1] == 'sim':
        kw = {}
        for a in sys.argv[5:]:
            if a.startswith('--rate='):
                kw['rate'] = float(a.split('=')[1])
            elif a.startswith('--seed='):
                kw['seed'] = int(a.split('=')[1])
        sim(sys.argv[2], sys.argv[3], sys.argv[4], **kw)
    else:
        print(__doc__)
        sys.exit(1)
