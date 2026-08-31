"""Audit the reviewed source batch, optionally verify its append-only merge."""
import argparse
from collections import Counter
import gzip
import json
from pathlib import Path
import re

from extract_ui_sources import normalized_key
from merge_reviewed_ui import PLACEHOLDER, read

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--merged', action='store_true')
    args = parser.parse_args()
    batch = read(ROOT/'analysis/ui_sources_reviewed_zh.json')['translations']
    deferred = read(ROOT/'analysis/ui_sources_deferred.json')['reasons']
    original = read(ROOT/'build/dictionary_before_sources_merge.json')['translations']
    current = read(ROOT/'translations/dictionary_zh.json')['translations']
    review = read(ROOT/'analysis/ui-sources-final/review_zh.json')['translations']
    with gzip.open(ROOT/'analysis/ui-sources-final/candidates.json.gz', 'rt', encoding='utf-8') as stream:
        evidence = json.load(stream)
    assert set(batch).isdisjoint(deferred)
    assert set(review) == set(batch) | set(deferred), 'Review entry missing a decision'
    assert all(current.get(k) == v for k,v in original.items()), 'An existing translation changed'
    accepted_roles = {'ui_literal','translation_source','json_ui_literal','named_ui_group','named_ui_definition'}
    normalized_values = {}
    for key, value in batch.items():
        assert isinstance(value, str) and value.strip() and re.search('[\u3400-\u9fff]', value), key
        assert '\ufffd' not in value, key
        assert Counter(PLACEHOLDER.findall(key)) == Counter(PLACEHOLDER.findall(value)), key
        assert key.count('\n') == value.count('\n'), ('line breaks', key)
        assert Counter(re.findall(r'</?[A-Za-z][^>]*>', key)) == Counter(re.findall(r'</?[A-Za-z][^>]*>', value)), ('markup', key)
        assert Counter(re.findall(r'\d+(?:\.\d+)?', key)) == Counter(re.findall(r'\d+(?:\.\d+)?', value)), ('numbers', key)
        assert any(e['role'] in accepted_roles and e.get('host_source') for e in evidence[key]['evidence']), ('source', key)
        normalized = normalized_key(key)
        previous = normalized_values.setdefault(normalized, normalized_key(value))
        assert previous == normalized_key(value), ('conflicting normalized batch keys', key)
    expected = dict(original)
    for key, value in batch.items():
        expected.setdefault(key, value)
    if args.merged:
        assert current == expected, 'Missing additions or unrelated dictionary edits'
        assert current['Pin'] == '固定'
    report = {
        'review_entries': len(review), 'approved': len(batch), 'deferred': len(deferred),
        'before': len(original), 'expected_after': len(expected), 'merged_verified': args.merged,
        'old_values_preserved': len(original),
        'checks': ['duplicate JSON keys', 'complete classification', 'host UI evidence',
                   'placeholders', 'newlines', 'markup', 'numbers', 'normalized batch conflicts',
                   'append-only dictionary merge', 'Pin remains 固定'],
        'runtime_caveat': 'Dictionary coverage does not prove on-screen translation. Long/rich-text layouts may be filtered by the display hook.',
        'over_500_char_source_keys': [k for k in batch if len(k) > 500],
        'deferred_reasons': deferred,
    }
    (ROOT/'analysis/ui_sources_merge_report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in {'checks','runtime_caveat','deferred_reasons','over_500_char_source_keys'}}))
    print('Sources over runtime 500-character filter:', len(report['over_500_char_source_keys']))


if __name__ == '__main__':
    main()
