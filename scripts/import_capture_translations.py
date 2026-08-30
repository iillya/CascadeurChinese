import concurrent.futures
import json
import pathlib
import re
import time
import urllib.parse
import urllib.request

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


def translate(text: str) -> str:
    # Repair a mojibake sequence present in the host's captured tooltips only
    # for translation input; the exact English dictionary key is preserved.
    query = text.replace("��", "'")
    url = (
        "https://translate.googleapis.com/translate_a/single?client=gtx&sl=en&tl=zh-CN&dt=t&q="
        + urllib.parse.quote(query)
    )
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=20) as response:
                payload = json.loads(response.read().decode("utf-8"))
            result = "".join(part[0] for part in payload[0] if part and part[0]).strip()
            if result and result != text:
                return result
        except Exception:
            if attempt == 3:
                return ""
            time.sleep(0.5 * (attempt + 1))
    return ""


def main() -> None:
    capture = json.loads(CAPTURE.read_text(encoding="utf-8"))["translations"]
    document = json.loads(DICTIONARY.read_text(encoding="utf-8"))
    formal = document["translations"]

    reused = {}
    for path in WORKSPACE.rglob("*_zh.json"):
        if path == DICTIONARY:
            continue
        try:
            translations = json.loads(path.read_text(encoding="utf-8")).get("translations", {})
        except Exception:
            continue
        for key, value in translations.items():
            if key in capture and key not in formal and eligible(key) and value and value != key:
                reused.setdefault(key, value)

    pending = [key for key in capture if key not in formal and key not in reused and eligible(key)]
    generated = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
        futures = {executor.submit(translate, key): key for key in pending}
        for future in concurrent.futures.as_completed(futures):
            key = futures[future]
            value = future.result()
            if value and value != key and "�" not in value:
                generated[key] = value

    formal.update(reused)
    formal.update(generated)
    document["translations"] = dict(sorted(formal.items(), key=lambda item: item[0].casefold()))
    DICTIONARY.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n",
                          encoding="utf-8")
    print(json.dumps({
        "captured": len(capture), "eligible": len(pending) + len(reused),
        "reused": len(reused), "translated": len(generated),
        "formal_total": len(formal),
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()
