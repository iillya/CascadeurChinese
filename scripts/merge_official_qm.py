#!/usr/bin/env python3
"""Extract source/translation pairs from an official Qt .qm file and merge
missing entries into translations/dictionary_zh.json.

The script never overwrites existing non-empty translations. It only adds keys
that are absent, or fills keys whose current value is empty.

Usage:
    python scripts/merge_official_qm.py
"""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]
DICTIONARY = ROOT / "translations" / "dictionary_zh.json"
OFFICIAL_QM = pathlib.Path(
    r"C:\Program Files\Cascadeur\resources\translations\app_zh_CN.qm"
)

LCONVERT_CANDIDATES = [
    ROOT.parent / "_ThirdParty" / "Qt" / "6.5.3" / "msvc2019_64" / "bin" / "lconvert.exe",
    ROOT.parent / "_ThirdParty" / "Qt" / "6.6.0" / "msvc2019_64" / "bin" / "lconvert.exe",
    ROOT.parent / "_ThirdParty" / "Qt" / "5.15.2" / "msvc2019_64" / "bin" / "lconvert.exe",
    ROOT / "third_party" / "qt6sdk" / "bin" / "lconvert.exe",
]


def find_lconvert():
    for candidate in LCONVERT_CANDIDATES:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        "lconvert.exe not found. Pass --lconvert or install a Qt SDK with lconvert."
    )


def qm_to_ts(qm: pathlib.Path, ts: pathlib.Path, lconvert: pathlib.Path):
    subprocess.run(
        [str(lconvert), "-i", str(qm), "-o", str(ts)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )


def parse_ts(ts: pathlib.Path):
    root = ET.parse(str(ts)).getroot()
    entries = []  # (source, translation, context)
    for context in root.findall("context"):
        name = context.findtext("name", default="")
        for message in context.findall("message"):
            source = message.findtext("source", default="")
            translation = message.findtext("translation", default="")
            if not source or not translation or not translation.strip():
                continue
            if translation.strip().startswith("???"):
                continue
            entries.append((source, translation.strip(), name))
    return entries


def load_dictionary():
    doc = json.loads(DICTIONARY.read_text(encoding="utf-8"))
    if not isinstance(doc, dict) or "translations" not in doc:
        raise ValueError("Dictionary is missing the translations object")
    return doc


def save_dictionary(doc):
    doc["translations"] = dict(
        sorted(doc["translations"].items(), key=lambda item: item[0].casefold())
    )
    path = DICTIONARY.with_suffix(DICTIONARY.suffix + ".tmp")
    path.write_text(
        json.dumps(doc, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    path.replace(DICTIONARY)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qm", type=pathlib.Path, default=OFFICIAL_QM)
    parser.add_argument("--lconvert", type=pathlib.Path)
    args = parser.parse_args()

    if not args.qm.exists():
        print(f"[ERROR] QM file not found: {args.qm}")
        return 1
    lconvert = args.lconvert or find_lconvert()

    with tempfile.TemporaryDirectory() as temporary:
        ts = pathlib.Path(temporary) / "app_zh_CN.ts"
        print(f"Converting {args.qm} -> {ts}")
        qm_to_ts(args.qm, ts, lconvert)
        entries = parse_ts(ts)

    doc = load_dictionary()
    translations = doc["translations"]

    added = []
    filled = []
    skipped = []
    conflicts = []
    seen = {}

    for source, translation, context in entries:
        if source in seen and seen[source] != translation:
            conflicts.append((source, seen[source], translation, context))
            continue
        seen[source] = translation

        if source in translations:
            current = translations[source]
            if current and current.strip():
                skipped.append(source)
            else:
                translations[source] = translation
                filled.append(source)
        else:
            translations[source] = translation
            added.append(source)

    save_dictionary(doc)
    print(f"Total .qm entries: {len(entries)}")
    print(f"Added new entries: {len(added)}")
    print(f"Filled empty entries: {len(filled)}")
    print(f"Skipped existing non-empty: {len(skipped)}")
    print(f"Conflict duplicates (kept first): {len(conflicts)}")
    if conflicts:
        for source, first, other, ctx in conflicts[:20]:
            print(f"  CONFLICT {source!r}: {first!r} vs {other!r} [{ctx!r}]")
    print(f"Dictionary total after merge: {len(translations)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
