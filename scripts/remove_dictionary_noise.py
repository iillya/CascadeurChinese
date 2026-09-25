#!/usr/bin/env python3
"""Remove noise entries from the formal CJK dictionary.

Noise here means entries whose translated value contains no CJK at all. Such
entries cannot contribute Chinese text: brand names, pure acronyms, axes
(X/Y/Z), FPS/FPS counters and internal setting identifiers remain the same in
English, so keeping them only bloats the dictionary and may shadow legitimate
later additions.

Usage:
    python scripts/remove_dictionary_noise.py
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DICTIONARY = ROOT / "translations" / "dictionary_zh.json"

CJK = re.compile(r"[\u4e00-\u9fff]")


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
    removed = []
    for source, target in list(translations.items()):
        if not CJK.search(target):
            removed.append((source, target))

    for source, _ in removed:
        del translations[source]

    save_dictionary(doc)
    print(f"Removed no-CJK noise entries: {len(removed)}")
    print(f"Dictionary total after noise removal: {len(translations)}")
    for source, target in removed:
        print(f"  {source!r} => {target!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
