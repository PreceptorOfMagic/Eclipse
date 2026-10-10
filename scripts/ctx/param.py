#!/usr/bin/env python3
"""Look ONE parameter up in the register. `param.py <name>` -- that is the whole habit.

WHY THIS EXISTS. The rule says check the register every time. On 2026-08-16 I did not, and
the reason was friction, not forgetfulness: looking a parameter up meant reading a 600-line
document, so under time pressure I grepped the source instead and guessed. That cost a run
(`COOP_VT_FLOOR` is the VERTICAL fps floor; horizontal is `COOP_HH_FLOOR`, and the apply
reported success while the floors never moved).

A rule that is more expensive to follow than to skip will be skipped. So the lookup is one
command, it searches every surface INCLUDING the curated prose, and it prints the traps
attached to the name rather than only its definition.

    param.py COOP_HH_FLOOR        # exact or partial name
    param.py held                 # shows BOTH meanings, because it has two
    param.py --missing            # names used in the code with no register row

EXIT 1 when the name is not registered -- that is the signal to go find a primary source
or run a test BEFORE using it, which is the rule.

EXIT 3 when the name IS registered but its implementation was WITHDRAWN. Same STOP, opposite
reason, and it exists because exit 0 could not tell the two apart. `conv[kr= fb=]` was
reverted on 2026-08-16 and stayed in this register, in two agent charters and in the code
review advice for a week afterwards -- every lookup said FOUND, because the row was still
there. A guard that only catches ABSENT names cannot catch a name whose CODE went away.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
REGISTER = os.path.join(ROOT, "docs", "parameter-register.md")


def sections(text):
    """Split the register into (heading, body) so a hit can be reported in context."""
    out, head, buf = [], "(top)", []
    for line in text.splitlines():
        if line.startswith("#"):
            out.append((head, "\n".join(buf)))
            head, buf = line.lstrip("# ").strip(), []
        else:
            buf.append(line)
    out.append((head, "\n".join(buf)))
    return out


def withdrawn(text):
    """Names the register marks WITHDRAWN -> {name: (date, why, instead)}.

    Parsed from the `## WITHDRAWN` table rather than a per-row marker because the case that
    forced this state (`conv[kr= fb=]`) lives in PROSE in two sections, not in a table row --
    a class column alone would not have caught it.
    """
    out, inside = {}, False
    for line in text.splitlines():
        if line.startswith("#"):
            inside = line.lstrip("# ").strip().upper().startswith("WITHDRAWN")
            continue
        if not inside or not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 4 or set(cells[0]) <= set("-: "):
            continue
        nm = cells[0].strip("`").strip()
        if nm and nm.lower() != "name":
            out[nm] = (cells[1], cells[2], cells[3])
    return out


def withdrawn_hit(name, table):
    """Does this query name a withdrawn thing?

    Exact, or a whole-word component of it -- so `conv`, `kr` and `fb` all resolve to
    `conv[kr= fb=]`, while `converted` (live, and the recommended replacement) does NOT.
    Being wrong in the loud direction is the point, but a false positive on a live
    parameter would add friction to the one habit this whole tool exists to keep cheap.
    """
    for w in table:
        if name.lower() == w.lower():
            return w
        if re.search(r"\b%s\b" % re.escape(name), w):
            return w
    return None



def is_row_key(line, name):
    """True when `name` is the KEY of a register table row, not merely words in prose.

    WHY THIS EXISTS. `param.py` used to return exit 0 for ANY line containing the name,
    prose included, and CLAUDE.md makes exit 0 the green light ("ONLY EXIT 0 IS GREEN").
    So a name that appears nowhere but a sentence passed the check. Measured 2026-09-03:
    `param.py climbing` exited 0 -- an ordinary English word out of the phrase "stuck at 1
    with `avcrefresh` climbing" -- and `param.py stuck` exited 0 against that same sentence
    BEFORE any `stuck` row existed. A guard rail that green-lights an arbitrary word is
    worse than none, because the register's whole discipline rests on this one cheap check.
    Found by diff-sentinel while reviewing an unrelated diff.
    """
    if not line.lstrip().startswith("|"):
        return False
    cells = [c.strip() for c in line.strip().strip("|").split("|")]
    if not cells:
        return False
    key = cells[0].strip()
    # A KEY CELL MAY NAME SEVERAL PARAMETERS. Registrars legitimately group a surface into
    # one row -- e.g. "`composite_skipped_pre_irap`, `composite_iraps`, `span_s`, and the
    # rest of `meta.json`" -- so accept ANY backticked token in the key, not just the first.
    # Only the FIRST cell counts: prose in later cells is what this whole check exists to
    # reject. Measured 2026-09-03: matching the first token alone made two genuinely
    # registered fields read as unregistered, which is a false STOP and just as damaging as
    # the false green it replaced.
    ticked = re.findall(r"`([^`]+)`", key)
    for t in ticked:
        t = t.strip()
        if t == name or t.split()[0] == name:
            return True
    bare = key.strip("`").strip()
    if bare == name:
        return True
    return bool(bare.split()) and bare.split()[0].strip("`").strip() == name

def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().split("\n\n")[0])
        return 2
    if not os.path.exists(REGISTER):
        print("no register at %s" % REGISTER)
        return 2
    text = open(REGISTER, errors="replace").read()

    if sys.argv[1] == "--missing":
        sys.path.insert(0, HERE)
        import param_extract as px
        reg = set(re.findall(r"`([^`\n]+)`", text))
        miss = []
        for s in px.SURFACES:
            for r in px.SURFACES[s]():
                cands = {r["name"], r.get("symbol", "")}
                if not (cands & reg):
                    miss.append((s, r["name"]))
        for s, n in sorted(miss):
            print("%-9s %s" % (s, n))
        print("\n%d unregistered" % len(miss))
        return 1 if miss else 0

    name = sys.argv[1]
    hits = []
    rows = []          # hits where the name is the ROW KEY of a table -- an actual registration
    for head, body in sections(text):
        for ln in body.splitlines():
            if re.search(r"\b%s\b" % re.escape(name), ln) or ("`%s`" % name) in ln:
                hits.append((head, ln.strip()))
                if is_row_key(ln, name):
                    rows.append((head, ln.strip()))
    if not hits:
        print("NOT REGISTERED: %s" % name)
        print("Do not use it yet. Find the primary source (who OWNS it -- this repo, Apollo,")
        print("webOS, moonlight-common-c?) or run a dedicated test, then add a row.")
        return 1

    gone = withdrawn_hit(name, withdrawn(text))
    if gone:
        date, why, instead = withdrawn(text)[gone]
        print("WITHDRAWN: %s" % gone)
        print("Its implementation NO LONGER EXISTS -- do not look for it, do not use it as")
        print("an example. This is a STOP, exactly like NOT REGISTERED.")
        print("  withdrawn:   %s" % date)
        print("  why:         %s" % why[:400])
        print("  use instead: %s" % instead[:200])
        print("\nThe register still MENTIONS it below (that is deliberate -- the name lives on")
        print("in commits and comments, so it has to stay findable). Read those lines as")
        print("history, not as instructions.")

    if not rows:
        print("MENTIONED BUT NOT REGISTERED: %s" % name)
        print("The name appears in the register's PROSE but has no table row of its own, so")
        print("it is NOT a registered parameter. This is a STOP, exactly like NOT REGISTERED.")
        print("Treat the lines below as history -- commits and comments keep old names alive --")
        print("not as a registration. Find the owner, get a primary source or run a test, add")
        print("the row, then use it.")
        for head, ln in hits[:6]:
            print("   [%s] %s" % (head[:40], ln[:160]))
        return 1

    seen_heads = []
    for head, ln in hits:
        if head not in seen_heads:
            seen_heads.append(head)
            print("\n== %s" % head)
        print("   %s" % ln[:220])
    print("\n%d line(s) across %d section(s)." % (len(hits), len(seen_heads)))
    if len(seen_heads) > 1:
        print("MORE THAN ONE SECTION MENTIONS IT -- read them all before using it; several "
              "names in this register mean different things in different places.")
    return 3 if gone else 0


if __name__ == "__main__":
    sys.exit(main())
