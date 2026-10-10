#!/usr/bin/env python3
"""Extract every PARAMETER this project actually uses, straight from the source.

WHY THIS EXISTS
---------------
The register (docs/parameter-register.md) is only trustworthy if its entries are
derived from the code rather than recalled. Three gauges were misread in a single
session on 2026-08-15 -- `staleRun` (a run-length read as a cumulative delta),
`paneAge` (~27,000 ms while panes were ~340 ms apart), and `paired_this` (assumed
to mean "a fresh peer frame"; it is also true for a REPEAT, so an instrument built
on it measured nothing) -- and each misreading survived because the name looked
self-explanatory. A name is not evidence.

So this script never says what a parameter MEANS. It finds every parameter and the
exact file:line sites that determine its meaning, and leaves the meaning to be filled
in from those sites (or from an upstream doc, or from a test). That division is the
whole point: extraction is mechanical, interpretation is evidenced.

SURFACES COVERED
  ini      moonlight.ini keys           -- from the ini_write_* writer list (canonical)
  sentinel /tmp/coop-* flag files       -- from access()/sentinel_set() call sites
  const    COOP_*/VT_*/HH_* #defines    -- compile-time tunables
  gauge    labels inside [COOP-*] lines -- the diag counters every test reads
  pattern  pattern.html title readout   -- what the panel shows during every test
  script   ${VAR} knobs in scripts/     -- the shell surface; its absence cost a run
  host     Apollo keys host_encoder owns -- from KEY_FILTER, the authoritative list

Usage:
    param_extract.py [--surface ini|sentinel|const|gauge|pattern|script|host|all] [--json]

Output is one row per parameter: surface, name, backing symbol, and the read/write
sites that define it. Feed it to param_check.py to diff against the register.
"""

import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src")


def rg(pattern, path=SRC, globs=("*.c", "*.h")):
    """Grep the source, returning (relpath, lineno, text). Uses grep -rn for portability."""
    cmd = ["grep", "-rnE", pattern, path]
    for g in globs:
        cmd.insert(3, "--include=" + g)
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=120).stdout
    except Exception:
        return []
    rows = []
    for line in out.splitlines():
        parts = line.split(":", 2)
        if len(parts) != 3:
            continue
        rows.append((os.path.relpath(parts[0], ROOT), int(parts[1]), parts[2].strip()))
    return rows


# ── surface: moonlight.ini keys ────────────────────────────────────────────
# The writer is canonical: a key that is never written is not a live setting, and
# the writer names the struct field, which is the symbol the readers use.
def surface_ini():
    rows = []
    settings_c = os.path.join(SRC, "app", "app_settings.c")
    if not os.path.exists(settings_c):
        return rows
    pat = re.compile(r'ini_write_(\w+)\(fp,\s*"([^"]+)",\s*([^)]+)\)')
    with open(settings_c, errors="replace") as f:
        for n, line in enumerate(f, 1):
            m = pat.search(line)
            if not m:
                continue
            kind, key, field = m.group(1), m.group(2), m.group(3).strip()
            sym = field.split("->")[-1].split(".")[-1].strip()
            reads = [r for r in rg(r"\b%s\b" % re.escape(sym))
                     if not r[0].endswith("app_settings.c")]
            rows.append({
                "surface": "ini", "name": key, "type": kind, "symbol": sym,
                "write_site": "src/app/app_settings.c:%d" % n,
                "read_sites": ["%s:%d" % (r[0], r[1]) for r in reads[:6]],
                "n_reads": len(reads),
            })
    return rows


# ── surface: /tmp sentinels ───────────────────────────────────────────────
# A sentinel's meaning lives at the access() that TESTS it, not at the touch that
# creates it -- the same file is armed by scripts, the UI dialog and the tunables
# module, and only the reader decides what it switches.
def surface_sentinel():
    seen = {}
    for path, n, text in rg(r'"/tmp/[a-zA-Z0-9_.-]+"'):
        for name in re.findall(r'"(/tmp/[a-zA-Z0-9_.-]+)"', text):
            if name.endswith((".log", ".sock", ".h265")):
                continue
            # Not every /tmp path is a switch: XDG_RUNTIME_DIR and a DS5 template path
            # are ordinary file locations. Listing them as unread sentinels invents gaps.
            if "setenv" in text or "tmpl_path" in text:
                continue
            e = seen.setdefault(name, {"surface": "sentinel", "name": name,
                                       "read_sites": [], "write_sites": [], "n_reads": 0})
            site = "%s:%d" % (path, n)
            # A sentinel is read two ways: as a FLAG (access()) and as a VALUE file
            # (fopen "r" -> a number, e.g. /tmp/coop-fps). Counting every fopen as a
            # write marks the value files "written but never read", which is the
            # opposite of what they are.
            if ("access(" in text or "sentinel_get" in text or "stat(" in text
                    or ("fopen" in text and '"r"' in text)):
                e["read_sites"].append(site)
                e["n_reads"] += 1
            elif "sentinel_set" in text or "unlink" in text or "fopen" in text:
                e["write_sites"].append(site)
    for e in seen.values():
        e["read_sites"] = e["read_sites"][:6]
        e["write_sites"] = e["write_sites"][:4]
    return sorted(seen.values(), key=lambda e: e["name"])


# ── surface: compile-time constants ───────────────────────────────────────
def surface_const():
    rows = []
    for path, n, text in rg(r"^#define\s+(COOP|VT|HH)_[A-Z0-9_]+"):
        m = re.match(r"#define\s+(\w+)\s*(.*)", text)
        if not m:
            continue
        name, value = m.group(1), m.group(2).strip()
        uses = [r for r in rg(r"\b%s\b" % re.escape(name)) if not r[2].startswith("#define")]
        rows.append({
            "surface": "const", "name": name, "value": value,
            "define_site": "%s:%d" % (path, n),
            "read_sites": ["%s:%d" % (r[0], r[1]) for r in uses[:6]],
            "n_reads": len(uses),
        })
    return rows


# ── surface: diag-log gauges ──────────────────────────────────────────────
# THE HARD ONE, AND THE ONE THAT HAS BURNED US. A gauge is a bare label in a log
# line; its meaning is whatever mutates the variable behind it. So: pair each
# `label=%spec` with its positional argument, then find every mutation of that
# argument. A gauge whose only mutation is `x = run` is a LEVEL; one whose
# mutations are all `x++` is a COUNT. Reading the first as the second is exactly
# the staleRun error.
FMT_SPEC = re.compile(r"%[-+ #0-9.]*(?:l|ll|z|h)?[diouxXeEfgGcsp]")


def _join_literals(blob):
    """Concatenate adjacent C string literals into one format string."""
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', blob))


def _split_args(argblob):
    """Split a C argument list on TOP-LEVEL commas (parens/brackets nest)."""
    args, depth, cur = [], 0, ""
    for ch in argblob:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    return args


def _mutations(sym):
    """Every site that changes `sym`, classified. This is what defines the gauge."""
    # Strip a leading cast: `(int) coop_hh_ready` is still coop_hh_ready.
    base = re.sub(r"^\(\s*\w[\w\s*]*\)\s*", "", sym.strip())
    base = base.split("[")[0].strip()
    # An ACCESSOR hides the real gauge behind a function. Follow it to what it
    # returns, or the gauge resolves to nothing and silently drops out of the
    # register -- which is how `recv`, `evicted` and `stale` (the ones every test
    # actually reads) came back as "no mutation found".
    fn = re.match(r"^(\w+)\s*\(\s*\)$", base)
    if fn:
        for path, n, text in rg(r"%s\s*\(\s*void\s*\)\s*\{" % re.escape(fn.group(1))):
            body = _returned_symbol(os.path.join(ROOT, path), n)
            if body:
                sites, shape = _mutations(body)
                return sites, "via %s() -> %s: %s" % (fn.group(1), body, shape)
        return [], "accessor (definition not found)"
    # Keep struct fields whole: `coop_hh_seq.stale_dropped++` is not a mutation of
    # `coop_hh_seq`, and truncating at the dot finds neither.
    if not re.match(r"^[A-Za-z_][\w.]*$", base):
        return [], "derived"
    # A struct field is almost never mutated through the instance -- the owning
    # module takes a pointer and writes `s->field`. Search the FIELD, both ways.
    if "." in base:
        field = base.rsplit(".", 1)[1]
        alts = [r"(\.|->)%s" % re.escape(field)]
    else:
        alts = [r"\b%s\b" % re.escape(base)]
    return _scan(alts)


def _scan(alts):
    """Classify every mutation matching any of `alts` (regex fragments)."""
    sites, kinds = [], set()
    for alt in alts:
        for path, n, text in rg(alt):
            if re.search(alt + r"\s*(\+\+|\+=)", text) or re.search(r"\+\+\s*" + alt, text):
                kinds.add("increment")
            elif re.search(alt + r"\s*=\s*0\s*[;,)]", text):
                kinds.add("reset")
            elif re.search(alt + r"\s*=[^=]", text):
                kinds.add("assign")
            else:
                continue
            sites.append("%s:%d" % (path, n))
    if "assign" in kinds and "increment" in kinds:
        shape = "MIXED -- both counted and assigned; read the sites"
    elif "assign" in kinds:
        shape = "LEVEL (assigned -- NOT a running total)"
    elif "increment" in kinds:
        shape = "count (monotonic)" if "reset" not in kinds else "count (resettable)"
    elif kinds:
        shape = "reset-only"
    else:
        shape = "no mutation found (accessor or const?)"
    return sites, shape


def _returned_symbol(path, defline):
    """The symbol a one-line accessor returns, so the gauge resolves to real storage."""
    try:
        lines = open(path, errors="replace").read().splitlines()
    except Exception:
        return None
    for line in lines[defline - 1:defline + 6]:
        m = re.search(r"return\s+([A-Za-z_][\w.]*)\s*;", line)
        if m:
            return m.group(1)
    return None


def surface_gauge():
    rows = []
    for cpath in [os.path.join(SRC, "app", "stream", "video", "session_video.c")]:
        if not os.path.exists(cpath):
            continue
        text = open(cpath, errors="replace").read()
        rel = os.path.relpath(cpath, ROOT)
        for m in re.finditer(r'fprintf\(\s*diag_log\s*,', text):
            start = m.end()
            depth, i = 1, start
            while i < len(text) and depth > 0:
                if text[i] == "(":
                    depth += 1
                elif text[i] == ")":
                    depth -= 1
                i += 1
            call = text[start:i - 1]
            lineno = text.count("\n", 0, m.start()) + 1
            fmt = _join_literals(call)
            tag = re.match(r"\[(COOP-[A-Z]+)\]", fmt)
            if not tag:
                continue
            # Args = everything after the format string's last closing quote.
            qend = call.rfind('"')
            args = _split_args(call[qend + 1:].lstrip().lstrip(","))
            # Walk specifiers in order, attaching each to the nearest preceding label.
            specs = []
            for sm in FMT_SPEC.finditer(fmt):
                before = fmt[:sm.start()]
                lm = re.findall(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*$", before)
                specs.append(lm[-1] if lm else None)
            if len(specs) != len(args):
                continue  # alignment not provable -- skip rather than guess
            for label, arg in zip(specs, args):
                if not label:
                    continue
                sites, shape = _mutations(arg)
                rows.append({
                    "surface": "gauge", "name": label, "tag": tag.group(1),
                    "symbol": arg, "shape": shape,
                    "emit_site": "%s:%d" % (rel, lineno),
                    "mutation_sites": sites[:6], "n_mutations": len(sites),
                })
    # A label can appear in several lines; keep the richest instance of each.
    best = {}
    for r in rows:
        k = (r["tag"], r["name"])
        if k not in best or r["n_mutations"] > best[k]["n_mutations"]:
            best[k] = r
    return sorted(best.values(), key=lambda r: (r["tag"], r["name"]))


# ── surface: the test pattern's on-screen readout ─────────────────────────
# ADDED 2026-08-16 AFTER THIS SURFACE COST A SESSION. The user asked why the top pane
# showed "so many missed and stalls"; the answer is that both are derived from a vsync
# estimate taken as the WINDOW MINIMUM callback interval, which on a loaded host reads
# 185fps on a 165Hz panel and makes ordinary frames score as skips. `missed` over-reported
# the real callback deficit ~30x. These are read off the panel in every single test and
# were in no register at all.
PATTERN = os.path.join("scripts", "coop", "testpattern", "pattern.html")


def surface_pattern():
    path = os.path.join(ROOT, PATTERN)
    if not os.path.exists(path):
        return []
    text = open(path, errors="replace").read()
    lines = text.splitlines()
    rows, seen = [], set()
    # The readout is built as  ' label=' + expr  in publishTitle().
    for m in re.finditer(r"'[\s\\n]*([a-zA-Z_]\w*)=' \+ ([^\n]+?)(?: \+|,|;|$)", text):
        label, expr = m.group(1), m.group(2).strip()
        if label in seen:
            continue
        seen.add(label)
        ident = re.findall(r"[A-Za-z_]\w*", expr)
        ident = [i for i in ident if i not in ("ONEFRAME", "toFixed", "now", "Math")]
        sym = ident[0] if ident else expr
        sites, kinds = [], set()
        for n, ln in enumerate(lines, 1):
            if re.search(r"\b%s\s*(\+\+|\+=)" % re.escape(sym), ln):
                kinds.add("increment"); sites.append("%s:%d" % (PATTERN, n))
            elif re.search(r"\b%s\s*=[^=]" % re.escape(sym), ln):
                kinds.add("assign"); sites.append("%s:%d" % (PATTERN, n))
        if "assign" in kinds and "increment" in kinds:
            shape = "MIXED -- both counted and assigned; read the sites"
        elif "increment" in kinds:
            shape = "count (monotonic)"
        elif "assign" in kinds:
            shape = "LEVEL (assigned -- NOT a running total)"
        else:
            shape = "derived"
        rows.append({"surface": "pattern", "name": label, "symbol": sym, "shape": shape,
                     "emit_site": "%s (publishTitle)" % PATTERN,
                     "mutation_sites": sites[:4], "n_mutations": len(sites)})
    return sorted(rows, key=lambda r: r["name"])


# ── surface: shell knobs (script environment variables) ───────────────────
# ADDED 2026-08-16 BECAUSE ITS ABSENCE COST A RUN. Needing the horizontal fps-floor knob
# and finding nothing to look up, I grepped and picked `COOP_VT_FLOOR` -- the VERTICAL one.
# The apply reported success, the floors did not move, and the run was void. `COOP_HH_FLOOR`
# is the horizontal knob and was in no register at all. Every `${VAR:-default}` in the
# co-op and webos scripts is a parameter someone will one day set from the outside.
SCRIPT_DIRS = ("scripts/coop", "scripts/webos", "scripts/lib")


def surface_script():
    rows, seen = [], {}
    for d in SCRIPT_DIRS:
        full = os.path.join(ROOT, d)
        if not os.path.isdir(full):
            continue
        for fn in sorted(os.listdir(full)):
            if not fn.endswith(".sh"):
                continue
            rel = os.path.join(d, fn)
            try:
                text = open(os.path.join(full, fn), errors="replace").read()
            except Exception:
                continue
            for n, line in enumerate(text.splitlines(), 1):
                # ${VAR:-default} / ${VAR:?msg} / ${VAR} -- the outside-settable form.
                for m in re.finditer(r"\$\{([A-Z][A-Z0-9_]{2,})(:[-?=][^}]*)?\}", line):
                    name, dflt = m.group(1), (m.group(2) or "")
                    if name in ("PATH", "HOME", "USER", "SHELL", "PWD", "BASH_SOURCE", "IFS"):
                        continue
                    e = seen.setdefault(name, {"surface": "script", "name": name,
                                               "default": dflt[2:] if dflt[:2] in (":-", ":=") else "",
                                               "sites": [], "n_sites": 0})
                    if len(e["sites"]) < 4:
                        e["sites"].append("%s:%d" % (rel, n))
                    e["n_sites"] += 1
    rows = sorted(seen.values(), key=lambda r: r["name"])
    return rows


# ── surface: host (Apollo) config keys this project writes ────────────────
# The keys host_encoder.sh OWNS are named in its KEY_FILTER, which is the authoritative
# list -- `restore` is defined as removing exactly these. Extracting from there rather
# than from prose keeps the register honest when the script changes.
def surface_host():
    path = os.path.join(ROOT, "scripts", "coop", "host_encoder.sh")
    if not os.path.exists(path):
        return []
    text = open(path, errors="replace").read()
    m = re.search(r"KEY_FILTER=\"(.*?)\"\n", text, re.S)
    if not m:
        return []
    keys = sorted(set(re.findall(r"\^([a-z_]+)", m.group(1))))
    rows = []
    for k in keys:
        sites = ["scripts/coop/host_encoder.sh:%d" % (text[:mm.start()].count("\n") + 1)
                 for mm in list(re.finditer(re.escape(k), text))[:3]]
        rows.append({"surface": "host", "name": k, "owner": "Apollo",
                     "sites": sites, "n_sites": len(sites)})
    return rows


SURFACES = {"ini": surface_ini, "sentinel": surface_sentinel,
            "const": surface_const, "gauge": surface_gauge,
            "pattern": surface_pattern, "script": surface_script,
            "host": surface_host}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--surface", default="all", choices=list(SURFACES) + ["all"])
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    names = list(SURFACES) if a.surface == "all" else [a.surface]
    out = []
    for s in names:
        out.extend(SURFACES[s]())
    if a.json:
        json.dump(out, sys.stdout, indent=1)
        return
    for r in out:
        head = "%-9s %-28s" % (r["surface"], r["name"])
        if r["surface"] in ("gauge", "pattern"):
            print("%s %-34s %s" % (head, r["symbol"], r["shape"]))
        elif r["surface"] == "script":
            print("%s default=%-14s sites=%d" % (head, r["default"] or "(none)", r["n_sites"]))
        elif r["surface"] == "host":
            print("%s owner=%-10s sites=%d" % (head, r["owner"], r["n_sites"]))
        elif r["surface"] == "const":
            print("%s %-34s reads=%d" % (head, r["value"][:34], r["n_reads"]))
        elif r["surface"] == "ini":
            print("%s %-34s reads=%d" % (head, r["symbol"], r["n_reads"]))
        else:
            print("%s reads=%d writes=%d" % (head, r["n_reads"], len(r.get("write_sites", []))))
    print("\n%d parameters across %s" % (len(out), ", ".join(names)), file=sys.stderr)


if __name__ == "__main__":
    main()
