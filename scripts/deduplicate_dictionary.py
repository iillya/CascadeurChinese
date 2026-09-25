#!/usr/bin/env python3
"""Remove dictionary keys whose normalized equivalent already exists with the
same translation.

Only normalized duplicates with identical translations are removed. Context-
specific strings that normalize to the same key but have different Chinese
translations are always preserved because they may correspond to different
visible English strings.

Usage:
    python scripts/deduplicate_dictionary.py
"""

import argparse
import json
import pathlib
import sys
import unicodedata

ROOT = pathlib.Path(__file__).resolve().parents[1]
DICTIONARY = ROOT / "translations" / "dictionary_zh.json"


def normalized_key(text):
    text = text.replace("\xa0", " ").replace("\u3000", " ")
    text = text.translate({ord(c): None for c in "\u00ad\u200b\u200c\u200d\ufeff"})
    text = "".join(
        chr(ord(c) - 0xFEE0) if 0xFF01 <= ord(c) <= 0xFF5E else c for c in text
    )
    text = text.translate(
        str.maketrans({"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": "-"})
    )
    text = " ".join(text.split())
    while text.endswith("…"):
        text = text[:-1]
    while text.endswith("..."):
        text = text[:-3]
    if text.endswith(" *"):
        text = text[:-2]
    text = text.replace("_", " ").replace("&", "")
    text = "".join(
        c for c in unicodedata.normalize("NFD", text)
        if unicodedata.category(c) not in {"Mn", "Mc", "Me"}
    )
    return " ".join(unicodedata.normalize("NFC", text).casefold().split())


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
    parser.add_argument("--dictionary", type=pathlib.Path, default=DICTIONARY)
    args = parser.parse_args()

    if args.dictionary != DICTIONARY:
        # Keep the script safe: write to a standalone output path instead of
        # silently rewriting another dictionary.
        parser.error("Only the default formal dictionary may be rewritten in place")

    doc = load_dictionary()
    translations = doc["translations"]

    groups = {}
    for source, target in translations.items():
        groups.setdefault(normalized_key(source), []).append((source, target))

    kept = dict(translations)
    removed = 0
    conflicted = 0

    for normalized, pairs in groups.items():
        if len(pairs) < 2:
            continue
        targets = {target for _, target in pairs}
        if len(targets) != 1:
            conflicted += len(pairs)
            continue
        # Keep the shortest spelling; ties keep the first sorted key.
        representative = min(
            pairs, key=lambda pair: (len(pair[0]), pair[0].casefold())
        )
        for source, _ in pairs:
            if source != representative[0]:
                del kept[source]
                removed += 1

    doc["translations"] = kept
    save_dictionary(doc)
    print(f"Original entries: {len(translations)}")
    print(f"Removed normalized duplicates with same translation: {removed}")
    print(f"Preserved context-distinct normalized entries: {conflicted}")
    print(f"Dictionary total after dedupe: {len(kept)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
