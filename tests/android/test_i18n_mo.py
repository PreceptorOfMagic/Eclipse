#!/usr/bin/env python3
"""Every string of every compiled catalogue, through the Android reader and through Python's gettext:
they must agree exactly (tests/android/test_i18n_mo_dump.c)."""
import gettext, os, subprocess, sys

dump, mo_root = sys.argv[1], sys.argv[2]
locales = sorted(os.listdir(mo_root))
assets = os.path.join(os.path.dirname(dump), "android-i18n-assets")
os.makedirs(assets, exist_ok=True)
link = os.path.join(assets, "locale")
if not os.path.islink(link):
    os.symlink(mo_root, link)
total = fails = 0
for loc in locales:
    t = gettext.GNUTranslations(open(os.path.join(mo_root, loc, "LC_MESSAGES", "moonlight-tv.mo"), "rb"))
    ids = [k for k in t._catalog if isinstance(k, str) and k]
    out = subprocess.run([dump, loc.replace("_", "-")], input="\n".join(i.encode().hex() for i in ids) + "\n",
                         capture_output=True, text=True, env={**os.environ, "TEST_ASSETS": assets}).stdout.split("\n")
    for k, h in zip(ids, out):
        total += 1
        if bytes.fromhex(h).decode() != t.gettext(k):
            fails += 1
            print("MISMATCH", loc, repr(k))
print(f"{total} strings across {len(locales)} catalogues, {fails} mismatches")
sys.exit(1 if fails or total == 0 else 0)
