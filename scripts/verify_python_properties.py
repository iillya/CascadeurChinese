"""Validate this reviewed batch and record static/runtime/screenshot evidence."""
import json
from pathlib import Path
from merge_reviewed_ui import read, PLACEHOLDER

ROOT = Path(__file__).resolve().parents[1]


def main():
    before = read(ROOT/'build/dictionary_before_python_properties.json')['translations']
    formal = read(ROOT/'translations/dictionary_zh.json')['translations']
    batch = read(ROOT/'analysis/python_properties_reviewed_zh.json')['translations']
    coverage = read(ROOT/'analysis/ui-coverage-v2/candidates.json')
    capture = read(Path('C:/Users/win10/Desktop/Cascadeur_untranslated_zh.json'))['translations']
    assert all(formal.get(k) == v for k,v in before.items())
    assert set(formal)-set(before) == set(batch)
    evidence = {}
    for key,value in batch.items():
        assert formal[key] == value
        assert sorted(PLACEHOLDER.findall(key)) == sorted(PLACEHOLDER.findall(value))
        sources = [e for k,v in coverage.items() if k.casefold()==key.casefold() for e in v['evidence']]
        if key in capture:
            sources.append({'source': 'Cascadeur_untranslated_zh.json', 'role': 'user_runtime_capture'})
        if key in {'Fulcrum state','Fulcrum','Enforce','Collision radius','Max speed',
                   'IK/FK behaviour','Enforce FK in interpolation','Collision in relaxation'}:
            sources.append({'source': 'codex-clipboard-9132e873-a408-49f1-adde-95207d732966.png',
                            'role': 'user_screenshot', 'note': 'Case variants may differ from displayed capitalization'})
        assert sources, key
        evidence[key] = sources
    earlier = read(ROOT/'analysis/runtime_capture_reviewed_zh.json')['translations']
    assert all(k in capture and formal[k]==v for k,v in earlier.items())
    folded = {k.casefold() for k in formal}
    remaining = [k for k in capture if k.casefold() not in folded]
    report = {'added': len(batch), 'formal_total': len(formal), 'old_values_preserved': len(before),
              'earlier_capture_batch_verified': len(earlier), 'evidence': evidence,
              'capture_unmerged': remaining,
              'note': 'Unmerged includes shortcuts, names, brands, units, truncations and ambiguous labels. Not all are translatable UI.'}
    (ROOT/'analysis/python_properties_merge_report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in {'evidence','capture_unmerged','note'}}))
    print('Unmerged capture entries:', len(remaining))


if __name__ == '__main__':
    main()
