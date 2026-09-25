"""Prepare review candidates; never rewrite the formal dictionary automatically."""

import argparse
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
CAPTURE = pathlib.Path.home() / "Desktop" / "Cascadeur_untranslated_zh.json"
DICTIONARY = ROOT / "translations" / "dictionary_zh.json"

SHORTCUT = re.compile(
    r"^(?:(?:Ctrl|Alt|Shift)(?:\+(?:Ctrl|Alt|Shift|F\d{1,2}|[A-Z0-9=,+\-]|Space))+|"
    r"F\d{1,2}|Del|Home|PgUp|PgDown|X, Space)$"
)
INTERNAL = re.compile(
    r"^(?:Bind Group|MultiSelection Group|SingleSelection Group)(?: Additional[12])? \d+$"
)
BRANDS = {
    "AI", "Discord", "Facebook", "Instagram", "LinkedIn", "TikTok",
    "Youtube", "Python API", "FFmpeg", "Free Type", "Qt", "XYZ", "IK",
}


def eligible(text: str) -> bool:
    value = text.strip()
    if (not value or SHORTCUT.fullmatch(value) or INTERNAL.fullmatch(value) or
        all(SHORTCUT.fullmatch(part.strip()) for part in value.split(","))):
        return False
    if value in BRANDS or re.fullmatch(r"[\d.eE+\-%]+", value):
        return False
    if "<font" in value or ":/" in value or ":\\" in value or ".casc" in value:
        return False
    if "…" in value or "...y" in value or "...e" in value or value in {"C...", "S..."}:
        return False
    if re.match(r"^(?:Version:|Cascadeur version )", value):
        return False
    if value in {"Cascy", "Cascy_mesh", "auto_posing (0)", "pelvis", "Rig(0)",
                 "obj_properties_all_components", "obj_properties_all_except_guid",
                 "e5e5e5"}:
        return False
    if re.fullmatch(r"[a-z][a-z0-9_]*(?: \(\d+\))?", value):
        return False
    return bool(re.search(r"[A-Za-z]", value))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=pathlib.Path, default=CAPTURE)
    parser.add_argument("--output", type=pathlib.Path,
                        default=ROOT / "build" / "capture_review.json")
    args = parser.parse_args()
    if args.output.resolve() == DICTIONARY.resolve():
        parser.error("review output must not overwrite the formal dictionary")
    capture = json.loads(args.capture.read_text(encoding="utf-8"))["translations"]
    document = json.loads(DICTIONARY.read_text(encoding="utf-8"))
    formal = document["translations"]

    reused = {}
    for path in sorted(WORKSPACE.glob("*/translations/dictionary_zh.json")):
        if path == DICTIONARY:
            continue
        try:
            translations = json.loads(path.read_text(encoding="utf-8")).get("translations", {})
        except Exception:
            continue
        for key, value in translations.items():
            if (key in capture and key not in formal and eligible(key) and
                isinstance(value, str) and value and value != key):
                reused.setdefault(key, value)

    pending = [key for key in capture if key not in formal and key not in reused and eligible(key)]
    candidates = {key: reused.get(key, "")
                  for key in capture if key not in formal and eligible(key)}
    if args.output.exists():
        # Fail on malformed existing JSON; never discard hand-reviewed values.
        previous = json.loads(args.output.read_text(encoding="utf-8"))["translations"]
        for key, value in previous.items():
            if isinstance(value, str) and value:
                candidates[key] = value
    review = {"id": "cascadeur-review-only", "language": "zh-CN",
              "translations": dict(sorted(candidates.items(), key=lambda item: (item[0].casefold(), item[0])))}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    temporary.write_text(json.dumps(review, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(args.output)
    print(json.dumps({
        "captured": len(capture), "eligible": len(pending) + len(reused),
        "reused": len(reused), "translated": 0,
        "formal_total": len(formal),
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()
