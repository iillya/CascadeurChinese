"""Merge a manually reviewed translation batch without replacing existing keys."""
import argparse
import json
import os
import tempfile
from pathlib import Path
import re

PLACEHOLDER = re.compile(r"%(?:\d+|L\d+|n|[-+ #0]*\d*(?:\.\d+)?[sdfiu])|\{\d*\}")


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique)


def write_atomic(path, document):
    """Keep the old dictionary intact if serialization/write/replace fails."""
    data = json.dumps(document, ensure_ascii=False, indent=2) + "\n"
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("batch", type=Path)
    parser.add_argument("--dictionary", type=Path, default=Path(__file__).resolve().parents[1] / "translations/dictionary_zh.json")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    document = read(args.dictionary)
    original = document["translations"]
    batch = read(args.batch)["translations"]
    additions = {}
    for key, value in batch.items():
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"Empty/invalid translation: {key}")
        if sorted(PLACEHOLDER.findall(key)) != sorted(PLACEHOLDER.findall(value)):
            raise ValueError(f"Placeholder mismatch: {key}")
        if key not in original:
            additions[key] = value
    before = len(original)
    if args.write:
        original.update(additions)
        document["translations"] = dict(sorted(original.items()))
        write_atomic(args.dictionary, document)
    print(json.dumps({"before": before, "added": len(additions), "existing_preserved": len(batch)-len(additions), "after": before+len(additions), "written": args.write}))


if __name__ == "__main__":
    main()
