"""Read-only, broad UI string inventory. Never edits runtime dictionaries."""
import argparse
import ast
from collections import Counter
from datetime import datetime
import json
import gzip
import mmap
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
BINARY = {".exe", ".dll", ".pyd", ".rcc"}
TEXT = {".qml", ".js", ".py", ".json", ".ui", ".xml", ".ini", ".cfg", ".txt", ".html", ".htm"}
ASCII = re.compile(rb"[\x20-\x7e]{2,}")
UTF16 = re.compile(rb"(?:[\x20-\x7e]\x00){2,}")
UTF8 = re.compile(rb"[\x20-\x7e\x80-\xff]{2,}")
QUOTED = re.compile(r'''(?P<q>["'])(?P<body>(?:\\.|(?! (?P=q))[^\\])*?)(?P=q)''', re.X | re.S)
UI_CONTEXT = re.compile(r"(?:\b(?:text|title|label|tooltip|toolTip|placeholderText|description|caption|statusTip)\s*[:=]\s*|\b(?:qsTr|qsTranslate|QT_TR_NOOP|tr)\s*\([^\n]*?)$", re.I)
RESOURCE = re.compile(r"(?:^[A-Za-z]:[\\/]|://|^[./\\]+|\\|\.(?:dll|exe|py|png|svg|qml|json|casc|fbx|obj)\b|^\?)", re.I)
INTERNAL = re.compile(r"(?:^expression for |\b(?:QObject|QQuick|std::|nullptr|assert|Traceback|Copyright|License|compiler)\b|[{}]|::|[a-z][A-Z])")


def retain(text):
    if not 1 < len(text) <= 16384 or not any(c.isalpha() for c in text):
        return False
    # Definite compiler/linker symbols, not natural-language UI. Keep ordinary
    # identifiers in the broad pool; only drop ABI decoration and template dumps.
    if text.startswith(("?", "_Z", ".?", "__imp_")) or "@@" in text:
        return False
    if "::" in text and ("<" in text or "(" in text):
        return False
    if " " not in text and len(text) > 160:
        return False
    return True


def plausible_ui(text):
    """A review shortlist, NOT a runtime translation policy."""
    if not 2 <= len(text) <= 1500 or not re.search(r"[A-Za-z]", text):
        return False
    if RESOURCE.search(text) or INTERNAL.search(text) or "\ufffd" in text:
        return False
    if re.search(r"[\x00-\x08\x0b\x0c\x0e-\x1f]", text):
        return False
    if len(re.findall(r"[A-Za-z]", text)) < len(text) * 0.45:
        return False
    if re.search(r"(.)\1{5,}", text) or re.fullmatch(r"[a-z0-9_]+", text):
        return False
    if " " not in text.strip() and not re.fullmatch(r"[A-Z][a-z]{1,25}(?:[.!?…:]|\.\.\.)?", text.strip()):
        return False
    return True


def pe_regions(data):
    """Scan data/resource sections, not executable instructions or zero fill."""
    if data[:2] != b"MZ" or len(data) < 64:
        return [(0, len(data), "raw")]
    pe = struct.unpack_from("<I", data, 60)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("invalid PE header")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional
    regions = []
    for i in range(count):
        pos = table + 40 * i
        if pos + 40 > len(data):
            raise ValueError("truncated PE section table")
        name = data[pos:pos + 8].rstrip(b"\0").decode("ascii", "replace")
        size, start = struct.unpack_from("<II", data, pos + 16)
        flags = struct.unpack_from("<I", data, pos + 36)[0]
        if size and not flags & 0x20000000 and start + size <= len(data):
            regions.append((start, size, name))
    return regions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(r"C:\Program Files\Cascadeur"))
    parser.add_argument("--output", type=Path, default=ROOT / "analysis" / datetime.now().strftime("static-ui-%Y%m%d-%H%M%S"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    formal = json.loads((ROOT / "translations/dictionary_zh.json").read_text(encoding="utf-8"))["translations"]
    terms = {}
    files = []
    errors = []
    excluded = Counter()

    def add(text, relative, kind, location, ui=False):
        text = text.strip(" \r\n\t\x00")
        if not retain(text):
            return
        item = terms.setdefault(text, {"sources": set(), "kinds": set(), "examples": [], "ui_context": False})
        item["sources"].add(relative)
        item["kinds"].add(kind)
        if len(item["examples"]) < 4:
            item["examples"].append({"file": relative, "location": location, "kind": kind})
        item["ui_context"] |= ui

    def literals(content, relative, kind, base=0):
        for match in QUOTED.finditer(content):
            body = match.group("body")
            if not body:
                continue
            try:
                value = ast.literal_eval(match.group(0)) if "\\" in body else body
            except (ValueError, SyntaxError):
                value = body
            if isinstance(value, str):
                prefix = content[max(0, match.start() - 120):match.start()]
                add(value, relative, kind, base + match.start(), bool(UI_CONTEXT.search(prefix)))

    paths = sorted(args.root.rglob("*"))
    for path in paths:
        if not path.is_file():
            continue
        relative = path.relative_to(args.root).as_posix()
        if "chineselauncher" in {p.lower() for p in path.relative_to(args.root).parts}:
            excluded["localizer_files"] += 1
            continue
        suffix = path.suffix.lower()
        if suffix not in BINARY | TEXT:
            excluded[suffix or "no_extension"] += 1
            continue
        before = len(terms)
        try:
            if suffix in BINARY:
                with path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                    for start, size, section in pe_regions(data):
                        block = data[start:start + size]
                        for match in ASCII.finditer(block):
                            add(match[0].decode("ascii"), relative, "ascii", f"{section}+0x{match.start():x}")
                        for match in UTF16.finditer(block):
                            add(match[0].decode("utf-16-le"), relative, "utf16", f"{section}+0x{match.start():x}")
                        for match in UTF8.finditer(block):
                            if not any(b >= 128 for b in match[0]):
                                continue
                            try:
                                value = match[0].decode("utf-8")
                            except UnicodeDecodeError:
                                continue
                            add(value, relative, "utf8", f"{section}+0x{match.start():x}")
                        # Embedded uncompressed QML/JS strings retain their UI context.
                        if b"qsTr(" in block or b"text:" in block or b"toolTip:" in block:
                            literals(block.decode("utf-8", "replace"), relative, "embedded_literal", start)
            else:
                raw = path.read_bytes()
                encoding = "utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
                content = raw.decode(encoding, "replace")
                literals(content, relative, "text_literal")
                if suffix in {".ini", ".cfg", ".txt"}:
                    for line, value in enumerate(content.splitlines(), 1):
                        add(value, relative, "text_line", line)
            files.append({"path": relative, "bytes": path.stat().st_size, "new_unique": len(terms) - before})
        except (OSError, ValueError, struct.error) as exc:
            errors.append({"file": relative, "error": str(exc)})
        if len(files) % 100 == 0:
            print(f"scanned={len(files)} unique={len(terms)}", flush=True)

    ordered = sorted(terms, key=lambda key: (key.casefold(), key))
    review = {}
    for key in ordered:
        info = terms[key]
        info["sources"] = sorted(info["sources"])
        info["kinds"] = sorted(info["kinds"])
        info["translation"] = formal.get(key, "")
        # Prioritize explicit UI properties, plus wide-string labels from host
        # binaries. Third-party labels stay in the full pool, not auto-merged.
        host_source = any("/" not in name and not name.lower().startswith(
            ("qt", "torch", "av", "python", "onnx", "catboost", "usd", "msv", "vc")) for name in info["sources"])
        script_source = any(name.endswith((".py", ".json", ".qml", ".ui", ".xml")) for name in info["sources"])
        if key not in formal and plausible_ui(key) and (
            info["ui_context"] or script_source or (host_source and set(info["kinds"]) & {"ascii", "utf16", "utf8"})):
            review[key] = ""
    def write(name, document):
        opener = gzip.open if name.endswith(".gz") else open
        with opener(args.output / name, "wt", encoding="utf-8", newline="\n") as stream:
            json.dump(document, stream, ensure_ascii=True, indent=2)
            stream.write("\n")
    write("ui_context.json", {key: terms[key] for key in review})
    write("ui_review_zh.json", {"$schema": "sp-translation-v1", "id": "cascadeur-static-review",
          "language": "zh-CN", "translations": review})
    summary = {"root": str(args.root), "scanned_files": len(files), "unique_retained": len(terms),
               "exact_formal_matches": sum(key in formal for key in terms), "review_candidates": len(review),
               "excluded_by_type": dict(excluded), "errors": errors, "files": files}
    write("scan_report.json", summary)
    write("all_terms.json.gz", {"root": str(args.root), "terms": {key: terms[key] for key in ordered}})
    print(json.dumps({k: v for k, v in summary.items() if k not in {"files", "excluded_by_type"}}, ensure_ascii=False))
    print(f"output={args.output}")


if __name__ == "__main__":
    main()
