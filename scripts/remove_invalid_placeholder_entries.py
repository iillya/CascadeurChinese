#!/usr/bin/env python3
"""Remove dictionary entries whose translation uses placeholders not present in
the source.

These are unsafe: if the English source has no %1 while the Chinese translation
contains %1, rendering that entry would show literal placeholder fragments.
Without the original source template the translation cannot be corrected with
confidence, so the entry is dropped and the UI falls back to English.

Usage:
    python scripts/remove_invalid_placeholder_entries.py
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DICTIONARY = ROOT / "translations" / "dictionary_zh.json"

PLACEHOLDER = re.compile(r"%(?:\d+|L\d+|n|[-+ #0]*\d*(?:\.\d+)?[sdfiu])|\{\d*\}")


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
    doc = load_dictionary()
    translations = doc["translations"]
    bad = []
    for source, target in translations.items():
        source_placeholders = PLACEHOLDER.findall(source)
        target_placeholders = PLACEHOLDER.findall(target)
        if sorted(source_placeholders) != sorted(target_placeholders):
            bad.append((source, target, source_placeholders, target_placeholders))

    for source, _, _, _ in bad:
        del translations[source]

    save_dictionary(doc)
    print(f"Invalid placeholder entries removed: {len(bad)}")
    print(f"Dictionary total after removal: {len(translations)}")
    for source, target, src_ph, tgt_ph in bad:
        print(f"  {source!r}")
        print(f"    source placeholders: {src_ph}")
        print(f"    target placeholders: {tgt_ph}")
        print(f"    translation: {target!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
