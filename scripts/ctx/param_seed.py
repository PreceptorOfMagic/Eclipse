#!/usr/bin/env python3
"""Seed the mechanical half of docs/parameter-register.md from the source.

Run ONCE to create the register, and again (to stdout) whenever you want to see what
the code now contains versus what the register records. It writes the rows that can be
derived -- name, owner, backing symbol, shape, evidence sites -- and marks everything it
cannot prove as OPEN so the gaps are visible instead of implied.

It deliberately does NOT write a "meaning" column. Meaning is curated by hand from the
cited sites; a generated meaning would be exactly the assumption the register exists to
forbid.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import param_extract as px  # noqa: E402


def owner_of(row):
    """WHO DEFINES THIS PARAMETER'S MEANING -- the question that decides evidence class.

    If this repo's code defines it, the code IS the primary source and citing the
    mutation sites is sufficient. If another component defines it (Apollo, webOS,
    moonlight-common-c), our source only *passes it on*: our own comments about it are
    hearsay and the entry needs an upstream doc or a test. Getting this wrong is not
    hypothetical -- `idr_refresh_interval_sec` is documented in this repo's comments as
    "Apollo's default, and what both hosts run", and it is a CLIENT setting that does not
    appear anywhere in Apollo's installed binary.
    """
    if row["surface"] == "gauge":
        return "aurora"           # we emit it, we define it
    if row["surface"] == "const":
        return "aurora"
    if row["surface"] == "sentinel":
        return "aurora"
    return "aurora?"              # ini keys: some are ours, some go to the stream stack


# Resolutions the extractor cannot derive but a human has confirmed. Kept HERE rather
# than only in the register's prose, so a regeneration does not silently revert an
# evidenced row back to OPEN -- which would quietly undo the checking the rule asks for.
# Each value is (class, evidence). See docs/parameter-register.md §7.
RESOLVED = {
    "packetsize": ("SRC", "rides inside STREAM_CONFIGURATION, session.c:353; "
                          "moonlight-common-c Connection.c:295, SdpGenerator.c:324"),
    "coop_pad1_pane": ("SRC", "coop_pad_pane[], coop_input.c:76"),
    "coop_pad2_pane": ("SRC", "coop_pad_pane[], coop_input.c:76"),
    "coop_pad3_pane": ("SRC", "coop_pad_pane[], coop_input.c:76"),
    "coop_pad4_pane": ("SRC", "coop_pad_pane[], coop_input.c:76"),
    "known_pad": ("SRC", "known_pads[], app_settings.c:281"),
    "why": ("SRC", "coop_hh_cu_why[1..8], session_video.c:779-862 (idx 6 overloaded)"),
    # MEASURED 2026-08-18 by A/B capture, twice: once on a single-host PANE and once on the
    # two-host COMPOSITE. Signalled effect is only PPS loop_filter_across_slices (0 with the
    # key on, 1 off); constrained_intra_pred_flag is 0 either way.
    #
    # DO NOT CONCLUDE FROM THAT THAT THE KEY IS REPLACEABLE -- I did, and it was wrong. The
    # composite already carries loop_filter_across_tiles_enabled_flag = 0 and the seam IS the
    # tile boundary (tiles 1x2, second slice address 4080 = 120*34), so seam deblocking is off
    # in BOTH cases -- yet the panel shows blocky seam damage at 0 and is clean at 1. The
    # working effect is the UNSIGNALLED encode-time prediction restriction, which no client-side
    # header edit can reproduce. LOAD-BEARING and NVENC-ONLY. A PPS diff bounds what a key
    # signals; it cannot establish what an encoder does.
    "nvenc_constrained": ("TEST", "LOAD-BEARING, NVENC-only: unsignalled encode-time prediction "
                                  "restriction. Signals only PPS loop_filter_across_slices, which "
                                  "is NOT the mechanism (across_tiles already 0). A/B on pane and "
                                  "composite 2026-08-18; see memory three-box-architecture"),
}


def evidence_of(row):
    """The strongest evidence the extractor can prove, or OPEN."""
    if row["name"] in RESOLVED:
        return RESOLVED[row["name"]]
    if row["surface"] in ("gauge", "pattern"):
        sites = row.get("mutation_sites") or []
        if sites:
            return "SRC", "; ".join(sites[:3])
        # A gauge computed inline (`ratio=%.3f`, `(double) now / 1000.0`) has no storage
        # to mutate: the printf expression IS its definition, so the emit site is the
        # primary source. Calling these OPEN buries the genuine gaps in noise.
        if row.get("shape") == "derived" or "/" in row.get("symbol", "") or \
           "(" in row.get("symbol", ""):
            return "SRC", row["emit_site"] + " (computed inline)"
        return "OPEN", "no mutation site found -- resolve before use"
    if row["surface"] == "const":
        return ("SRC", row["define_site"]) if row.get("define_site") else ("OPEN", "")
    if row["surface"] == "sentinel":
        sites = row.get("read_sites") or []
        if sites:
            return "SRC", "; ".join(sites[:3])
        return "OPEN", "written but never read -- dead, or read outside src/"
    sites = row.get("read_sites") or []
    if sites:
        return "SRC", "; ".join(sites[:3])
    return "OPEN", "written to the ini but never read in src/"


def table(rows, cols):
    out = ["| " + " | ".join(cols) + " |",
           "|" + "|".join("---" for _ in cols) + "|"]
    for r in rows:
        out.append("| " + " | ".join(r) + " |")
    return "\n".join(out)


def main():
    gauges = px.surface_gauge()
    consts = px.surface_const()
    sentinels = px.surface_sentinel()
    inis = px.surface_ini()

    print("<!-- SEEDED by scripts/ctx/param_seed.py -- curate by hand, then keep it -->\n")

    print("## Gauges (`[COOP-*]` diag labels)\n")
    rows = []
    for g in gauges:
        cls, ev = evidence_of(g)
        rows.append(["`%s`" % g["name"], g["tag"], "`%s`" % g["symbol"],
                     g["shape"].replace("|", "/"), cls, "`%s`" % ev if ev else ""])
    print(table(rows, ["gauge", "line", "symbol", "shape", "ev", "evidence"]))

    print("\n## Test-pattern readout (`pattern.html` title bar)\n")
    rows = []
    for p in px.surface_pattern():
        cls, ev = evidence_of(p)
        rows.append(["`%s`" % p["name"], "`%s`" % p["symbol"],
                     p["shape"].replace("|", "/"), cls, "`%s`" % ev if ev else ""])
    print(table(rows, ["field", "symbol", "shape", "ev", "evidence"]))

    print("\n## Shell knobs (`${VAR}` in scripts/)\n")
    rows = []
    for k in px.surface_script():
        rows.append(["`%s`" % k["name"], "`%s`" % (k["default"] or "(none)")[:28],
                     str(k["n_sites"]), "SRC", "`%s`" % "; ".join(k["sites"][:2])])
    print(table(rows, ["knob", "default", "uses", "ev", "set at"]))

    print("\n## Host (Apollo) keys this project writes\n")
    rows = []
    for k in px.surface_host():
        rows.append(["`%s`" % k["name"], k["owner"], "`%s`" % "; ".join(k["sites"][:2])])
    print(table(rows, ["key", "owner", "written at"]))

    print("\n## Compile-time constants\n")
    rows = []
    for c in consts:
        cls, ev = evidence_of(c)
        rows.append(["`%s`" % c["name"], "`%s`" % c["value"].replace("|", "/")[:40],
                     str(c["n_reads"]), cls, "`%s`" % ev])
    print(table(rows, ["const", "value", "uses", "ev", "defined at"]))

    print("\n## Sentinels (`/tmp/*` flag files)\n")
    rows = []
    for s in sentinels:
        cls, ev = evidence_of(s)
        rows.append(["`%s`" % s["name"], str(s["n_reads"]), cls, "`%s`" % ev])
    print(table(rows, ["sentinel", "reads", "ev", "read at"]))

    print("\n## moonlight.ini keys\n")
    rows = []
    for i in inis:
        cls, ev = evidence_of(i)
        rows.append(["`%s`" % i["name"], i["type"], "`%s`" % i["symbol"],
                     str(i["n_reads"]), cls, "`%s`" % ev])
    print(table(rows, ["key", "type", "symbol", "reads", "ev", "read at"]))


if __name__ == "__main__":
    main()
