#!/usr/bin/env python3
"""Create a conservative UI-only shortlist from the static string scan."""

import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTEXT = os.path.join(ROOT, "translations", "term_context_zh.json")
OUT = os.path.join(ROOT, "translations", "ui_candidates_zh.json")

BINARY_SUFFIXES = (".exe", ".dll")
RESOURCE_EXTENSIONS = re.compile(
    r"\.(?:svg|png|jpe?g|gif|ico|ttf|otf|ini|json|qml|js|py|dll|exe|casc|fbx|dae|obj|wav|mp3)$",
    re.I,
)
INTERNAL_PREFIXES = (
    "expression for ", "qrc:", "file:", "http:", "https:", "qt.",
    "cannot assign ", "unable to assign ", "module ", "plugin ",
)
LEGAL_WORDS = re.compile(
    r"\b(?:copyright|license|licensed|trademark|rights reserved|gnu|gpl|lgpl|ffmpeg|freetype|about qt|version info|companyname|fileversion|productversion|originalfilename|internalname)\b",
    re.I,
)
CODE_WORDS = re.compile(
    r"\b(?:on[A-Z]\w+|viewModel|sourceComponent|objectName|property|regularExpression|undefined|nullptr|true|false)\b"
)


def repeated_noise(text):
    compact = re.sub(r"\s+", "", text)
    if re.search(r"(.)\1{3,}", compact):
        return True
    if len(compact) >= 8 and len(set(compact.lower())) <= 3:
        return True
    return False


def natural_words(text):
    words = re.findall(r"[A-Za-z]+(?:['-][A-Za-z]+)?", text)
    if not words:
        return False
    letters = sum(ch.isalpha() for ch in text)
    if letters / max(1, len(text)) < 0.62:
        return False
    # Long consonant-like tokens and random all-capital runs are binary noise.
    for word in words:
        if len(word) >= 7 and not re.search(r"[AEIOUYaeiouy]", word):
            return False
        if len(word) >= 4 and word.isupper() and word not in {
            "AI", "API", "CPU", "GPU", "FPS", "FBX", "IK", "FK", "PRO", "UEFN"
        }:
            return False
    return True


def is_ui_candidate(text):
    text = text.strip()
    if not 2 <= len(text) <= 140 or "\n" in text or "\r" in text:
        return False
    lower = text.lower()
    if lower.startswith(INTERNAL_PREFIXES):
        return False
    if RESOURCE_EXTENSIONS.search(text) or LEGAL_WORDS.search(text) or CODE_WORDS.search(text):
        return False
    if "_" in text or "{" in text or "}" in text or "<" in text or ">" in text:
        return False
    if re.search(r"[\\/]", text) or re.search(r"%\d|::|\$\{|=>|:=", text):
        return False
    if repeated_noise(text) or not natural_words(text):
        return False
    punctuation = sum(not (c.isalnum() or c.isspace() or c in ".,:;!?()'&+-…") for c in text)
    if punctuation:
        return False
    # Dotted action identifiers such as Timeline.Create cycle are internal IDs.
    if re.match(r"^[A-Za-z][A-Za-z0-9]*\.[A-Za-z]", text):
        return False
    # Static Qt/QML metadata contains thousands of lower-camel property names.
    # For single-token candidates retain only normal title-case labels or a
    # small set of acronyms; runtime sniffing remains the authority for the rest.
    if " " not in text:
        if not (re.fullmatch(r"[A-Z][a-z]{1,23}", text) or
                text in {"AI", "IK", "FK", "FBX", "FPS", "PRO", "UEFN"}):
            return False
    elif any(re.search(r"[a-z][A-Z]", word) for word in text.split()):
        return False
    return True


def main():
    with open(CONTEXT, encoding="utf-8") as fh:
        terms = json.load(fh)["terms"]
    selected = {}
    for term, info in terms.items():
        sources = info.get("sources", [])
        if info.get("translated") or not any(s.lower().endswith(BINARY_SUFFIXES) for s in sources):
            continue
        if is_ui_candidate(term):
            selected[term] = ""
    selected = dict(sorted(selected.items(), key=lambda item: item[0].casefold()))
    document = {
        "$schema": "sp-translation-v1",
        "id": "cascadeur-static-ui-candidates",
        "language": "zh-CN",
        "description": "软件目录静态扫描后二次清洗的高置信 UI 候选（待人工确认）",
        "translations": selected,
    }
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(document, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    print(f"high-confidence UI candidates: {len(selected)}")
    print(f"wrote: {OUT}")


if __name__ == "__main__":
    main()
