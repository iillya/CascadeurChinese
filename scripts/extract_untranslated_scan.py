#!/usr/bin/env python3
"""
Static scan for potentially-translatable strings in Cascadeur binaries and
sample .casc scenes.

* Binaries are scanned for UTF-16LE (Qt QString) and ASCII strings.
* .casc samples are scanned conservatively for readable quoted/text fragments.
* Everything is compared against the current dictionary; strings not already
  translated are written out as untranslated candidates, and each term is tagged
  with where it came from (binary / .casc / runtime tour) as a context reference.

Usage:
    python scripts/extract_untranslated_scan.py
"""

import gzip
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DICT = os.path.join(ROOT, "translations", "dictionary_zh.json")
OUT_UNTRANSLATED = os.path.join(ROOT, "translations", "untranslated_scan_zh.json")
OUT_CONTEXT = os.path.join(ROOT, "translations", "term_context_zh.json")
RUNTIME_CAPTURE = os.path.join(ROOT, "translations", "untranslated_captured2_zh.json")
IGNORE = {"122.casc"}

MAX_BIN = 200 * 1024 * 1024        # skip a .casc if it decompresses absurdly big


def find_cascadeur_root():
    """Return the current Cascadeur installation and sample root."""
    for env in ("ProgramFiles", "ProgramW6432"):
        pf = os.environ.get(env)
        if not pf or not os.path.isdir(pf):
            continue
        base = os.path.join(pf, "Cascadeur")
        if not os.path.isdir(base):
            continue
        if os.path.isfile(os.path.join(base, "cascadeur.exe")):
            return base, os.path.join(base, "samples")
    return (r"C:\Program Files\Cascadeur", r"C:\Program Files\Cascadeur\samples")


CASCADEUR, CASC_SAMPLES = find_cascadeur_root()


def _keep_phrase(p):
    """True if p looks like a real UI string (English phrase / short label)."""
    p = p.strip()
    if not (3 <= len(p) <= 110):
        return False
    # Only printable ASCII English UI strings; anything with CJK / high-byte
    # chars is a misaligned UTF-16 decode (mojibake) and must be dropped.
    if any(ord(c) < 0x20 or ord(c) > 0x7E for c in p):
        return False
    if not re.search(r"[A-Za-z]", p):
        return False
    if re.search(r"[\\/]", p):            # file paths / URL-ish
        return False
    if re.search(r"_node_\(", p) or re.search(r"\(\s*[XYZ]?\s*:\s*[-\d]", p):   # node positions
        return False
    if re.search(r"[XYZ]\s*:[-\d]", p):    # coordinates like X:-10.7
        return False
    if sum(c.isdigit() for c in p) > 0.4 * len(p) and " " in p:   # number-heavy
        return False
    if re.fullmatch(r"[0-9a-fA-F]{6,8}", p.strip()):   # hex colour values
        return False
    if p.lower() in ("true", "false", "null", "none", "utf-8", "utf8", "base64"):
        return False
    if re.search(r"\b0x[0-9a-fA-F]{3,}\b", p):
        return False
    # no-space, mostly-alnum long runs = base64 / hex / hashes
    if " " not in p and len(p) > 34 and re.fullmatch(r"[A-Za-z0-9+/=]+", p):
        return False
    if " " in p:
        return True
    # single word: small label or asset-style name (contains _ or . or -)
    return re.fullmatch(r"[A-Za-z][A-Za-z0-9_.\-()%]{1,27}", p) is not None


def utf16_phrases(data):
    """ASCII strings stored as UTF-16LE (Qt QString literals), alignment-agnostic.
    Matches runs of (printable ASCII, 0x00) so it is not thrown off by mixed
    ASCII/UTF-16 data in the binary."""
    out = set()
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){3,}", data):
        s = m.group(0)[::2].decode("ascii", "ignore")
        if _keep_phrase(s):
            out.add(s.strip())
    return out


def scan_binary(path):
    with open(path, "rb") as fh:
        data = fh.read()
    return utf16_phrases(data)


_QUOTED = re.compile(rb'"([^"\n]{3,90})"')
_TEXT = re.compile(rb">([^<>\n]{3,90})<")


def scan_casc(path):
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
        data = gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw
    except Exception:
        return set()
    if len(data) > MAX_BIN:
        return set()
    found = set()
    for m in _QUOTED.finditer(data):
        s = m.group(1).decode("utf-8", "ignore").strip()
        if _keep_phrase(s):
            found.add(s)
    for m in _TEXT.finditer(data):
        s = m.group(1).decode("utf-8", "ignore").strip()
        if _keep_phrase(s):
            found.add(s)
    return found


def find_casc_files(root):
    files = []
    for base, _, names in os.walk(root):
        for n in names:
            if n.endswith(".casc") and n not in IGNORE:
                files.append(os.path.join(base, n))
    return files


def load_dict():
    d = {}
    try:
        d.update(json.load(open(DICT, encoding="utf-8"))["translations"])
    except Exception:
        pass
    return d


def main():
    d = load_dict()
    covered = set(d.keys())
    print(f"dictionary entries: {len(covered)}")

    # 1) binaries
    bins = [
        "cascadeur.exe", "presenter_lib.dll", "tools.dll",
        "domain_scene.dll", "domain_timeline.dll", "domain_tools.dll",
    ]
    term_src = {}   # term -> set of sources
    for b in bins:
        p = os.path.join(CASCADEUR, b)
        if not os.path.isfile(p):
            continue
        for w in scan_binary(p):
            term_src.setdefault(w, set()).add(b)
    print(f"binary strings: {len(term_src)}")

    # 2) .casc files
    scenes = find_casc_files(CASC_SAMPLES)
    for sp in scenes:
        for w in scan_casc(sp):
            rel = os.path.relpath(sp, CASC_SAMPLES)
            term_src.setdefault(w, set()).add(os.path.basename(sp))
    print(f".casc files scanned: {len(scenes)}")
    print(f"total terms: {len(term_src)}")

    # 3) runtime capture
    if os.path.isfile(RUNTIME_CAPTURE):
        for k in json.load(open(RUNTIME_CAPTURE, encoding="utf-8"))["translations"]:
            term_src.setdefault(k, set()).add("runtime-tour")

    # noise filter
    NUM = re.compile(r"^[0-9][0-9.,%xX\s\-+eE]*$")
    AXIS = {"+X","+Y","+Z","-X","-Y","-Z","-X -Y -Z","-X -Y Z","-X Y -Z","-X Y Z",
            "X -Y -Z","X -Y Z","X Y -Z","X Y Z","RGB","X-YZ (Z-up LH)","X-ZY (Y-up RH)",
            "XZY (Y-up LH)","YXZ","YZX","ZXY","ZYX"}
    def noise(s):
        if not s: return True
        if NUM.match(s): return True
        if s in AXIS or s in ("...","<",">"): return True
        if re.fullmatch(r"(Ctrl\+|Alt\+|Shift\+)[A-Za-z0-9+]+", s): return True
        if s in ("Del","F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12","Tab","Esc","Enter","Space","None"): return True
        if s.startswith("<!DOCTYPE") or ".casc" in s: return True
        if re.match(r"^&?[0-9]+\s+C:", s) or s.startswith("C:"): return True
        if s.lower() in ("vfx","v10","cascadeur","none","cm","m","in","ft","mm"): return True
        return False

    untranslated = {}
    for term, srcs in term_src.items():
        t = term.strip()
        if t in covered or t.replace("&", "") in covered: continue
        if noise(t): continue
        # Binary-only terms: keep only multi-word phrases; single tokens from
        # binaries are code/hex/module noise.  .casc / runtime terms keep as-is.
        if all(not s.endswith(".casc") and s != "runtime-tour" for s in srcs):
            if " " not in t:
                continue
        untranslated[t] = {"sources": sorted(srcs)}

    # context reference (existing terms: where they appear)
    context = {}
    for term, srcs in term_src.items():
        context[term] = {"translated": term in covered or term.replace("&","") in covered,
                         "sources": sorted(srcs)}

    json.dump({"$schema":"sp-translation-v1","id":"cascadeur-scan","language":"zh-CN",
               "description":"静态扫描未翻译词条(待填写)","translations":{k:"" for k in untranslated}},
              open(OUT_UNTRANSLATED,"w",encoding="utf-8"), ensure_ascii=False, indent=2)
    json.dump({"$schema":"term-context","terms":context},
              open(OUT_CONTEXT,"w",encoding="utf-8"), ensure_ascii=False, indent=2)

    print(f"\n=== 未翻译候选(已滤噪声): {len(untranslated)} ===")
    # source breakdown
    from collections import Counter
    c = Counter()
    for s in untranslated.values():
        for src in s["sources"]:
            c[src if src not in ("runtime-tour",) else src] += 1
    print("top sources:", c.most_common(8))
    print(f"\nwrote: {OUT_UNTRANSLATED}")
    print(f"wrote: {OUT_CONTEXT}")


if __name__ == "__main__":
    main()
