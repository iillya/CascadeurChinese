"""Validate the reviewed native-name batch and its append-only dictionary merge."""
import argparse
import json
from pathlib import Path
from merge_reviewed_ui import read, PLACEHOLDER
from extract_ui_sources import normalized_key

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--merged', action='store_true')
    args = parser.parse_args()
    review = read(ROOT/'analysis/native_name_review_zh.json')['translations']
    batch = read(ROOT/'analysis/native_name_translated_zh.json')['translations']
    deferred = read(ROOT/'analysis/native_name_deferred.json')
    evidence = read(ROOT/'analysis/native_name_candidates.json')
    before_doc = read(ROOT/'build/dictionary_before_native_name_merge.json')
    current_doc = read(ROOT/'translations/dictionary_zh.json')
    before = before_doc['translations']
    if set(batch) & set(deferred) or set(batch) | set(deferred) != set(review):
        raise ValueError('Every candidate must be translated or explicitly deferred, exactly once')
    known = {normalized_key(k): v for k, v in before.items()}
    normalized_batch = {}
    for key, value in batch.items():
        if not isinstance(value, str) or not value.strip() or value == key:
            raise ValueError(f'Empty or untranslated value: {key}')
        if sorted(PLACEHOLDER.findall(key)) != sorted(PLACEHOLDER.findall(value)):
            raise ValueError(f'Placeholder mismatch: {key}')
        if not evidence[key]['evidence'] or not all(e['host_source'] for e in evidence[key]['evidence']):
            raise ValueError(f'Missing host schema evidence: {key}')
        norm = normalized_key(key)
        if norm in known and known[norm] != value:
            raise ValueError(f'Existing normalized translation conflict: {key}')
        if norm in normalized_batch and normalized_batch[norm] != value:
            raise ValueError(f'Batch normalized translation conflict: {key}')
        normalized_batch[norm] = value
    expected = dict(before_doc)
    expected['translations'] = dict(before)
    expected['translations'].update({k:v for k,v in batch.items() if k not in before})
    if current_doc != (expected if args.merged else before_doc):
        raise ValueError('Dictionary differs from expected append-only state')
    if expected['translations'].get('Pin') != '固定':
        raise ValueError('Pin must remain 固定')
    report = {'reviewed':len(review), 'translated':len(batch), 'deferred':len(deferred),
              'before':len(before), 'after':len(expected['translations']),
              'merged_verified':args.merged, 'original_values_preserved':len(before),
              'deferred_reasons':deferred,
              'boundary':'Display dictionary only. Schema names are not all confirmed visible; no scene data, input values or command IDs are modified.'}
    (ROOT/'analysis/native_name_merge_report.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
