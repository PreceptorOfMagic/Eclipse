#!/usr/bin/env python3
"""Diff the parameters the CODE uses against the ones the REGISTER records.

This is what makes the standing rule enforceable. The rule says "you will not use a
parameter that is not in the register"; without a check that is a good intention, and
the register rots the first time someone adds a counter in a hurry.

    param_check.py [--surface ...] [-q]

Exit codes:
    0  every parameter in the code has a register row, and none is OPEN
    1  UNREGISTERED parameters exist, or the code uses a WITHDRAWN one
    2  all registered, but some rows are OPEN (recorded, not yet evidenced)

OPEN is deliberately not a hard failure: an OPEN row means the gap is *known and
written down*, which is the state the rule is trying to produce. An unregistered
parameter means something is in use that nobody has checked, which is the state the
rule exists to prevent.

WITHDRAWN, by contrast, IS a hard failure: the register says that implementation is gone
while the extractor just found it in the code. One of the two is wrong and a human has to
say which -- either the thing came back and the row is stale, or it really is gone and the
code should not still name it. Note the permissive `registered_names()` below cannot catch
this on its own: a withdrawn row is still a backticked name in the register, so it satisfies
"registered" and the check would pass. That gap is exactly how `conv[kr= fb=]` survived.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import param_extract as px  # noqa: E402

REGISTER = os.path.join(px.ROOT, "docs", "parameter-register.md")


def registered_names():
    """Every parameter name the register mentions, from table rows and inline code spans.

    Deliberately permissive: a name written up in prose (the curated sections carry the
    real meaning) counts as registered. Being strict about WHERE the name appears would
    push people to satisfy the checker instead of documenting the parameter.
    """
    if not os.path.exists(REGISTER):
        return None
    text = open(REGISTER, errors="replace").read()
    return set(re.findall(r"`([^`\n]+)`", text))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--surface", default="all", choices=list(px.SURFACES) + ["all"])
    ap.add_argument("-q", "--quiet", action="store_true")
    a = ap.parse_args()

    reg = registered_names()
    if reg is None:
        print("FAIL: %s does not exist -- nothing is registered." % REGISTER)
        return 1

    names = list(px.SURFACES) if a.surface == "all" else [a.surface]
    rows = []
    for s in names:
        rows.extend(px.SURFACES[s]())

    missing = []
    for r in rows:
        n = r["name"]
        # A gauge label is registered if the label OR its backing symbol is written up:
        # the curated sections often name the symbol (`coop_stale_run`) because that is
        # the thing with the meaning, while the log prints the label (`staleRun`).
        cands = {n, r.get("symbol", ""), r.get("symbol", "").split("[")[0]}
        if not (cands & reg):
            missing.append(r)

    sys.path.insert(0, HERE)
    import param as pm
    wtab = pm.withdrawn(open(REGISTER, errors="replace").read())
    dead = []
    for r in rows:
        for cand in {r["name"], r.get("symbol", "")} - {""}:
            w = pm.withdrawn_hit(cand, wtab)
            if w:
                dead.append((r["surface"], r["name"], w))
                break

    open_rows = [ln for ln in open(REGISTER, errors="replace") if "| OPEN |" in ln]

    # BOTH are reported before either returns. An earlier draft returned on `missing`
    # first, which made the WITHDRAWN branch unreachable for as long as any unregistered
    # parameter existed -- a guard that cannot fire, which is the defect this project has
    # shipped more than once and the reason the WITHDRAWN state exists at all.
    if dead:
        print("WITHDRAWN PARAMETERS STILL IN THE CODE (%d) -- the register says these are "
              "gone, the extractor just found them. Resolve the contradiction:" % len(dead))
        for surface, nm, w in sorted(dead):
            print("  %-9s %-30s registered WITHDRAWN as `%s`" % (surface, nm, w))

    if missing:
        print("UNREGISTERED PARAMETERS (%d) -- find the primary source or test one, "
              "then add a row:" % len(missing))
        for r in sorted(missing, key=lambda r: (r["surface"], r["name"]))[:60]:
            print("  %-9s %-30s %s" % (r["surface"], r["name"], r.get("symbol", "")))

    if dead or missing:
        return 1

    if not a.quiet:
        print("OK: all %d parameters across %s are registered."
              % (len(rows), ", ".join(names)))
        # THE ALL-CLEAR HAS A KNOWN BLIND SPOT, AND SAYING SO IS THE POINT. This extractor
        # greps for LITERAL key strings; `COOP_WRITE` (src/app/app_settings.c:259) pastes its
        # key names together at COMPILE time, so the ~30 `coop_*_{h,v}` ini keys it builds are
        # invisible here. Measured 2026-09-06: 26 of those 30 return exit 1 from `param.py`
        # while this check reports clean -- and they are exactly the keys that drive the
        # sentinel writes. A guard rail that reports OK on names it cannot see is worse than
        # one that admits the gap, which is the same lesson as the WITHDRAWN state.
        # RE-MEASURED 2026-09-18, same method, after `coop_bitrate_h`/`coop_bitrate_v` got their
        # own register rows (parameter-register.md §13): 24 of 30 now fail -- update this count
        # again rather than trust it if more of the 30 get resolved later.
        print("   CAVEAT: this checker greps LITERAL key strings, so macro-built names are "
              "invisible to it -- notably the ~30 `coop_*_{h,v}` keys pasted together by "
              "COOP_WRITE (app_settings.c). 24 of 30 fail `param.py` while this reads clean. "
              "An OK here is NOT a statement about those.")
        if open_rows:
            print("%d row(s) are OPEN -- recorded, but NOT yet evidenced. "
                  "Do not draw conclusions from these:" % len(open_rows))
            for ln in open_rows[:20]:
                print("   " + ln.strip()[:110])
    return 2 if open_rows else 0


if __name__ == "__main__":
    sys.exit(main())
