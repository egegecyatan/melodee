#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# riffton: the sounds' display names, roles and character tags.
"""riffton shows every factory sound by a display name in Title Case, filed under a role, with two short character
tags (TRY: "round · plucky"). The list is a reviewable file, tools/preset_names.csv; this tool makes it and turns it
into the firmware's table.

    python3 tools/gen_preset_names.py --csv [build/host/desc.json]   (re)write tools/preset_names.csv from the presets
    python3 tools/gen_preset_names.py --header OUT.h                  the firmware table (tools/build.py runs this)

--csv takes the factory presets from the host tests' build/host/desc.json (tests/descdump.c: every engine's presets as
the firmware makes them) and the categories from firmware/src/browse.c PRESET_CAT (FMINI's from eng_fmini_bank.h). A
row of the existing CSV whose engine, index and original name are unchanged keeps its display, role and tags: edit
those columns by hand, then run --csv again after the presets change.

Display names: each word Title Case; acronyms, numbers and roman numerals stay as they are (KEEP); a few abbreviations
are expanded (EXPAND); a name that already has lower case letters (the Prophet-5's) is kept, with the upstream
spelling rules applied (FIXES). Roles: bass, lead, pad, arp, keys, fx, other (TRY lists bass, lead, pad, and arp with
keys). Tags: at most two of TAGS.
"""
import argparse
import csv
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CSV = ROOT / "tools" / "preset_names.csv"
FIELDS = ["engine", "index", "original", "display", "role", "tags"]
ENGINES = {"FMINI": 22, "PROPHET": 19, "FM6": 12, "CZ-1": 15, "SID": 3}     # the engines TRY offers (core.h ENGI_*)
KEEP = {"FM", "SID", "CZ", "OB", "PWM", "LFO", "EP", "DX", "303", "808", "909", "II", "III", "IV", "VI", "VII", "VIII",
        "CS", "JP", "MS", "TB", "PPG", "FX", "UFO", "SH", "DS", "PB", "AM", "FB", "EG", "8580", "6581", "X",
        "TX", "JX", "MG", "VS", "CR78", "MKII", "OK", "DJ", "MC", "TR", "PS", "RS", "SX", "XL", "E"}
SMALL = {"a", "an", "and", "as", "at", "by", "for", "in", "of", "on", "or", "the", "to"}   # lower case inside a name
EXPAND = {"PLK": "Pluck", "BRS": "Brass", "HRN": "Horn", "LD": "Lead", "CLAR": "Clarinet", "TRP": "Trumpet",
          "STR": "Strings", "ST": "Strings", "SQR": "Square", "SECT": "Section", "HARPSI": "Harpsi", "OCT": "Oct"}
FIXES = [(r"\bProphet ?5\b", "Prophet-5"), (r"\bMini ?Moog\b", "Minimoog"), (r"\bM-?Vave\b", "M-VAVE")]
ROLE_OF = {"B": "bass", "L": "lead", "R": "lead", "W": "lead", "P": "pad", "S": "pad", "G": "keys", "K": "keys",
           "U": "arp", "E": "arp", "F": "fx", "D": "other", "O": "other"}
ROLE_WORDS = [("bass", ("BASS", "SUB", "808", "REESE")), ("pad", ("PAD", "STRING", "CHOIR", "SWELL", "AIR", "DRONE")),
              ("arp", ("PLUCK", "PLK", "BELL", "ARP", "MALLET", "HARP", "MARIMBA", "KALIMBA", "PIZZ")),
              ("keys", ("PIANO", "ORGAN", "CLAV", "EP", "KEYS", "WURL", "RHODES")),
              ("lead", ("LEAD", "SOLO", "SYNC", "BRASS", "HORN", "FLUTE", "SAX", "WHISTLE"))]
TAGS = ["", "plucky", "punchy", "soft", "slow", "short", "held", "dark", "bright", "warm", "glassy", "round",
        "buzzy", "wide", "airy", "bell"]          # 4 bits each: index 0 = none (the firmware's RT_TAG)
TAG_WORDS = [("glassy", ("GLASS", "CRYSTAL", "ICE")), ("warm", ("WARM", "VELVET", "HONEY", "SOFT")),
             ("dark", ("DARK", "DEEP", "NIGHT", "MIDNIGHT", "VOID", "SUB")), ("bright", ("BRIGHT", "SUN", "SHINE")),
             ("airy", ("AIR", "BREATH", "WIND", "WHISPER", "CHOIR")), ("buzzy", ("SAW", "FUZZ", "BUZZ", "GROWL")),
             ("round", ("SINE", "ROUND", "RUBBER", "FLUTE", "MELLOW")), ("bell", ("BELL", "CHIME", "TINE", "BOX")),
             ("wide", ("WIDE", "SUPER", "WALL", "STRINGS", "SPACE")), ("punchy", ("PUNCH", "STAB", "HIT", "STRIKE"))]
CAT_TAG = {"bass": "round", "lead": "bright", "pad": "wide", "arp": "plucky", "keys": "warm", "fx": "airy"}


def words(name):
    if re.search(r"[a-z]", name):                    # already mixed case (the Prophet-5's, Sequential's own names)
        out = name
    else:
        def word(w):
            core = w.strip("()'.,")
            if not core:
                return w
            if core.upper() in EXPAND:
                return w.replace(core, EXPAND[core.upper()])
            if core.upper() in KEEP or re.fullmatch(r"[0-9][0-9A-Z]*|[A-Z][0-9]+", core):
                return w
            parts = re.split(r"([-.])", core)         # CS-80, E.PIANO: each part
            fixed = "".join(p if p in "-." or p.upper() in KEEP or re.fullmatch(r"[0-9]+[A-Z]*", p)
                            else p[:1].upper() + p[1:].lower() for p in parts)
            return w.replace(core, fixed)
        ws = [word(w) for w in name.split(" ")]
        out = " ".join(w.lower() if i and w.lower() in SMALL else w for i, w in enumerate(ws))
    for pat, rep in FIXES:
        out = re.sub(pat, rep, out)
    return out


def role_for(cat, name):
    role = ROLE_OF.get(cat, "other")
    if role == "other":
        up = name.upper()
        for r, ws in ROLE_WORDS:
            if any(re.search(r"\b" + w, up) for w in ws):
                return r
    return role


def tags_for(name, role, env, eng, e, patch):
    up, out = name.upper(), []
    for t, ws in TAG_WORDS:
        if len(out) < 1 and any(re.search(r"\b" + w, up) for w in ws):
            out.append(t)
    if env != [0, 0, 127, 0]:                         # (native engines: their own envelopes, not the preset's)
        atk, dec, sus, rel = env
        t = ("plucky" if sus < 50 else "punchy") if atk <= 6 else "slow" if atk >= 60 else "soft"
        if sus >= 100 and t == "soft":
            t = "held"
        if t not in out:
            out.append(t)
    if eng == "FMINI" and len(out) < 2 and patch is not None:
        cut = e[0]
        t = "dark" if cut < 45 else "bright" if cut > 88 else None
        if t is None:
            waves = {patch[3], patch[4], patch[5]}
            t = "buzzy" if waves & {2, 4, 5} else "round"
        if t not in out:
            out.append(t)
    if len(out) < 2 and CAT_TAG.get(role) and CAT_TAG[role] not in out:
        out.append(CAT_TAG[role])
    return out[:2]


def fmini_patches():
    src = (ROOT / "firmware/src/eng_fmini_bank.h").read_text()
    return [[int(x) for x in m.split(",")] for m in re.findall(r"^\s*\{\{([0-9, ]+)\}", src, re.M)]


def categories():
    src = (ROOT / "firmware/src/browse.c").read_text()
    body = re.search(r"PRESET_CAT\[NENGINES\] = \{(.*?)\n\};", src, re.S)[1]
    body = re.sub(r"#if MELODEE_LEGACY_EXTRAS.*?#endif", "", body, flags=re.S)
    cats = {}
    for key, val in re.findall(r"\[(\w+)\]\s*=\s*((?:\s*\"[A-Z]*\")+|FMINI_CATS)", body):
        idx = {"ENGI_MIDI": 21, "ENGI_FMINI": 22, "ENGI_CZ": 15, "ENGI_PROPHET": 19}.get(key, None)
        idx = idx if idx is not None else int(key)
        if val == "FMINI_CATS":
            val = re.search(r'#define FMINI_CATS "([A-Z]*)"', (ROOT / "firmware/src/eng_fmini_bank.h").read_text())[1]
        else:
            val = "".join(re.findall(r'"([A-Z]*)"', val))
        cats[idx] = val
    return cats


def make_csv(desc):
    d = json.loads(Path(desc).read_text())
    cats, patches = categories(), fmini_patches()
    old = {}
    if CSV.exists():
        with CSV.open() as f:
            for r in csv.DictReader(f):
                old[(r["engine"], r["index"], r["original"])] = r
    rows = []
    for name, ei in ENGINES.items():
        eng = d["ENG"][ei]
        assert eng["name"] == name, f"engine {ei} is {eng['name']}, not {name}"
        for k, p in enumerate(eng["presets"]):
            orig = p["name"]
            if orig.upper().startswith("INIT"):
                continue                               # (the engines' INIT sounds: not a sound to try)
            cat = cats.get(ei, "")[k:k + 1]
            role = role_for(cat, orig)
            tags = tags_for(orig, role, p["env"], name, p["e"], patches[k] if name == "FMINI" else None)
            r = {"engine": name, "index": str(k), "original": orig, "display": words(orig), "role": role,
                 "tags": " · ".join(tags)}
            kept = old.get((name, str(k), orig))
            if kept:
                r.update({f: kept[f] for f in ("display", "role", "tags")})
            rows.append(r)
    with CSV.open("w", newline="") as f:
        w = csv.DictWriter(f, FIELDS)
        w.writeheader()
        w.writerows(rows)
    print(f"preset names: {len(rows)} sounds -> {CSV}")


def c_str(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"')
    return s.encode("latin-1", "replace").decode("latin-1")


def make_header(out):
    with CSV.open() as f:
        rows = list(csv.DictReader(f))
    roles = ["bass", "lead", "pad", "arp", "keys", "fx", "other"]
    names, offs, ents = [], {}, []
    for r in rows:
        disp = r["display"].strip() or r["original"]
        assert disp == disp.strip() and len(disp) <= 20, f"{r['engine']} {r['index']}: display name too long: {disp!r}"
        assert disp != disp.upper() or not re.search(r"[A-Z]{3}", disp) or all(
            w.upper() in KEEP or not w.isalpha() for w in disp.split()), f"not Title Case: {disp!r}"
        if disp not in offs:
            offs[disp] = sum(len(n.encode("latin-1")) + 1 for n in names)
            names.append(disp)
        assert r["role"] in roles, f"{r['engine']} {r['index']}: role {r['role']!r} not one of {roles}"
        tags = [t.strip() for t in r["tags"].split("·") if t.strip()]
        assert len(tags) <= 2 and all(t in TAGS for t in tags), f"{r['engine']} {r['index']}: tags {tags} (known: {TAGS[1:]})"
        t = [TAGS.index(x) for x in tags] + [0, 0]
        ents.append((ENGINES[r["engine"]], int(r["index"]), roles.index(r["role"]), t[0] | t[1] << 4, offs[disp]))
    ents.sort(key=lambda e: (e[0], e[1]))
    blob = "".join(n + "\0" for n in names)
    lines = ["/* generated by tools/gen_preset_names.py from tools/preset_names.csv (edit the CSV, not this file) */",
             "#ifndef MELODEE_RES", "#define MELODEE_RES  /* melodee.c: the resource region (res.c) */", "#endif",
             "typedef struct { uint8_t engine, preset, role, tags; uint16_t name; } rt_sound_t;",
             f"#define RT_NSOUNDS {len(ents)}u",
             "enum { RT_BASS_T, RT_LEAD_T, RT_PAD_T, RT_ARP_T, RT_KEYS_T, RT_FX_T, RT_OTHER_T };   /* rt_sound_t.role */",
             "static const char *const RT_TAG[16] = {" + ", ".join(f'"{t}"' for t in TAGS) + "};",
             "static const rt_sound_t RT_SOUNDS[RT_NSOUNDS] MELODEE_RES = {"]
    lines += [f"    {{{e}, {k}, {ro}, {tg}, {off}}}," for e, k, ro, tg, off in ents]
    lines += ["};"]
    chunks = [blob[i:i + 96] for i in range(0, len(blob), 96)]
    lines += [f"static const char RT_NAMES[{len(blob.encode('latin-1'))}] MELODEE_RES ="]
    lines += ['    "' + c_str(c).replace("\0", "\\0") + '"' for c in chunks]
    lines[-1] += ";"
    Path(out).write_text("\n".join(lines) + "\n")
    print(f"preset names: {len(ents)} sounds, {len(blob)} B of names -> {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", nargs="?", const=str(ROOT / "build/host/desc.json"), metavar="DESC_JSON")
    ap.add_argument("--header", metavar="OUT_H")
    a = ap.parse_args()
    if a.csv:
        make_csv(a.csv)
    if a.header:
        make_header(a.header)
    if not (a.csv or a.header):
        ap.error("--csv or --header")


if __name__ == "__main__":
    sys.exit(main())
