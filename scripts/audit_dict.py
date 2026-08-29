#!/usr/bin/env python3
"""Full audit of the translation dictionary: flag suspicious entries."""

import json
import os
import re
import sys

sys.stdout.reconfigure(encoding="utf-8")
MAIN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "translations", "dictionary_zh.json")

KNOWN_ACRONYM = {
    "IK", "RGB", "UV", "AO", "SSS", "LOD", "PBR", "SDK", "CASC", "UE", "UE4",
    "FBX", "XML", "AI", "CPU", "GPU", "ID", "FX", "HDR", "HDRI", "MDI", "FPS",
    "GI", "API", "GL", "OpenGL", "VFX", "Cinema", "IX", "NURBS", "SubD",
    "Substance", "Unity", "YouTube", "Twitter", "Tx", "Cf", "Alpha", "FCurve",
}


def cjk(v):
    return bool(re.search(r"[\u4e00-\u9fff]", v))


def lower_leftover(v):
    return [w for w in re.findall(r"\b[a-z]{2,}\b", v)
            if w.lower() not in ("alpha", "beta")]


def upper_leftover(v):
    return [w for w in re.findall(r"\b[A-Za-z]{2,}\b", v)
            if w.isupper() and w not in KNOWN_ACRONYM]


def main():
    d = json.load(open(MAIN, encoding="utf-8"))["translations"]
    issues = {}
    issues["no-cjk"] = [k for k, v in d.items() if not cjk(v)]
    issues["value==key"] = [k for k, v in d.items() if v.strip() == k]
    issues["lower-english"] = [k for k, v in d.items() if cjk(v) and lower_leftover(v)]
    issues["upper-english"] = [k for k, v in d.items() if cjk(v) and upper_leftover(v)]
    byval = {}
    for k, v in d.items():
        if cjk(v):
            byval.setdefault(v, []).append(k)
    issues["dup-value"] = [(v, ks) for v, ks in byval.items() if len(ks) > 1]
    dropped = []
    for k, v in d.items():
        if not cjk(v):
            continue
        if "-&gt;" not in k and "->" not in k and "_" not in k:
            continue
        # normalize the arrow/entity so "&gt;" doesn't count as a phantom word "gt"
        kk = k.replace("-&gt;", "→").replace("->", "→").replace("&gt;", "→")
        kw = [w for w in re.findall(r"[A-Za-z]+", kk) if len(w) >= 2]
        cj = len(re.findall(r"[\u4e00-\u9fff]", v))
        nk = len(re.findall(r"\d+", kk))
        nv = len(re.findall(r"\d+", v))
        if kw and cj < len(kw) and nv >= nk:
            dropped.append(k)
    issues["structured-key-dropped"] = dropped
    issues["html-missing"] = [k for k, v in d.items()
                              if k.startswith("<") and not v.startswith("<")]

    print("dictionary entries:", len(d))
    for name, lst in issues.items():
        print(f"\n=== {name}: {len(lst)} ===")
        if name == "dup-value":
            for v, ks in lst[:40]:
                print(f"   {v[:44]}  <=  {', '.join(k[:26] for k in ks)}")
        else:
            for s in lst[:40]:
                print(f"   {s[:56]}  =>  {d.get(s,'')[:32]}")


if __name__ == "__main__":
    main()
