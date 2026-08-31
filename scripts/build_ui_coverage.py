"""Reclassify the full retained inventory and inspect Python without executing it.

All old strings remain in the original gzip; low-confidence strings are classified,
not silently discarded. Derived labels are hypotheses, never auto-translations.
"""
import argparse
import ast
from collections import Counter
import gzip
import json
from pathlib import Path
import re
import zipfile
from scan_ui_inventory import plausible_ui, TEXT

ROOT = Path(__file__).resolve().parents[1]
METHODS = {'add_behaviour', 'node', 'node_deep', 'get_property', 'set_property',
           'get_behaviour_settings_range', 'set_data', 'get_data'}


def display_name(value):
    value = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1 \2', value)
    value = re.sub(r'([a-z0-9])([A-Z])', r'\1 \2', value).replace('_', ' ')
    return value[:1].upper() + value[1:].lower()


def python_terms(content):
    tree = ast.parse(content)
    parents = {child: node for node in ast.walk(tree) for child in ast.iter_child_nodes(node)}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Constant) or not isinstance(node.value, str):
            continue
        value = node.value
        if not value or not re.search('[A-Za-z]', value):
            continue
        role = 'python_literal'
        current = node
        for _ in range(5):
            current = parents.get(current)
            if current is None:
                break
            if isinstance(current, ast.Call):
                method = getattr(current.func, 'attr', getattr(current.func, 'id', ''))
                if method in METHODS:
                    role = 'behaviour_or_property_reference'
                    break
            if isinstance(current, ast.ClassDef):
                if current.name == 'BehaviourName' or any('Enum' in ast.unparse(b) for b in current.bases):
                    role = 'enum_value'
                break
            if isinstance(current, (ast.Assign, ast.AnnAssign)):
                targets = current.targets if isinstance(current, ast.Assign) else [current.target]
                if any(re.search('setting|behavio|enum|label|title', ast.unparse(t), re.I) for t in targets):
                    role = 'settings_collection'
        yield value, node.lineno, role


def inventory_records(path):
    # Streaming reader for scan_ui_inventory's pretty-printed terms mapping.
    # Detect only structural indentation, never braces inside string contents.
    with gzip.open(path, 'rt', encoding='utf-8') as stream:
        key, lines = None, []
        for line in stream:
            if key is None and line.startswith('    "') and line.rstrip().endswith(': {'):
                key = json.loads(line.strip()[:-3])
                lines = ['{\n']
            elif key is not None:
                if line.rstrip() in ('    },', '    }'):
                    lines.append('}')
                    yield key, json.loads(''.join(lines))
                    key = None
                else:
                    lines.append(line)
        if key is not None:
            raise ValueError('Incomplete inventory record')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('C:/Program Files/Cascadeur'))
    parser.add_argument('--inventory', type=Path, default=ROOT/'analysis/static-ui-20260831-162207/all_terms.json.gz')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--capture', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    formal = json.loads((ROOT/'translations/dictionary_zh.json').read_text(encoding='utf-8'))['translations']
    folded = {k.casefold() for k in formal}
    candidates, counts, errors, archives = {}, Counter(), [], []

    def add(value, source, role, location=None, derived_from=None):
        if len(value) > 16000 or not re.search('[A-Za-z]', value):
            return
        item = candidates.setdefault(value, {'evidence': [], 'formal_exact': value in formal,
                                             'formal_casefold': value.casefold() in folded})
        evidence = {'source': source, 'role': role, 'location': location}
        if derived_from is not None:
            evidence['derived_from'] = derived_from
        if evidence not in item['evidence']:
            item['evidence'].append(evidence)

    with gzip.open(args.output/'classification.jsonl.gz', 'wt', encoding='utf-8') as out:
        for value, info in inventory_records(args.inventory):
            natural = plausible_ui(value)
            category = 'review_text' if natural else 'identifier_path_or_uncertain'
            counts[category] += 1
            out.write(json.dumps({'text': value, 'category': category, 'sources': info['sources']}, ensure_ascii=True)+'\n')
            if natural:
                for source in info['sources']:
                    add(value, source, 'static_text')
    counts['classified_total'] = sum(counts.values())

    def inspect(content, source):
        try:
            for value, line, role in python_terms(content):
                if plausible_ui(value) or role != 'python_literal':
                    add(value, source, role, line)
                    if role != 'python_literal' and re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', value):
                        derived = display_name(value)
                        if derived != value:
                            add(derived, source, 'derived_unconfirmed', line, value)
        except (SyntaxError, ValueError) as exc:
            errors.append({'source': source, 'error': str(exc)})

    for path in args.root.rglob('*.py'):
        if 'ChineseLauncher' in path.parts:
            continue
        try:
            inspect(path.read_text(encoding='utf-8-sig'), path.relative_to(args.root).as_posix())
            counts['python_files'] += 1
        except (OSError, UnicodeError) as exc:
            errors.append({'source': str(path), 'error': str(exc)})
    for path in args.root.rglob('*.zip'):
        with zipfile.ZipFile(path) as archive:
            for member in archive.infolist():
                if Path(member.filename).suffix.lower() not in TEXT:
                    continue
                source = path.relative_to(args.root).as_posix()+'!'+member.filename
                if member.file_size > 16*1024*1024:
                    archives.append({'source': source, 'status': 'size_limit'})
                    continue
                try:
                    content = archive.read(member).decode('utf-8-sig')
                    if member.filename.endswith('.py'):
                        inspect(content, source)
                    else:
                        from scan_ui_inventory import QUOTED
                        for match in QUOTED.finditer(content):
                            add(match.group('body'), source, 'archive_literal', match.start())
                    archives.append({'source': source, 'status': 'read'})
                except (UnicodeError, OSError, ValueError) as exc:
                    errors.append({'source': source, 'error': str(exc)})
    if args.capture:
        capture = json.loads(args.capture.read_text(encoding='utf-8-sig'))['translations']
        for value in capture:
            add(value, str(args.capture), 'runtime_capture_unreviewed')
    counts['candidate_total'] = len(candidates)
    counts['missing_casefold'] = sum(not v['formal_casefold'] for v in candidates.values())
    counts['derived_unconfirmed'] = sum(any(e['role']=='derived_unconfirmed' for e in v['evidence']) for v in candidates.values())
    for name, obj in [('candidates.json', candidates), ('review_zh.json', {'translations': {k:'' for k,v in candidates.items() if not v['formal_casefold']}}),
                      ('report.json', {'counts': dict(counts), 'errors': errors, 'archives': archives,
                       'limits': ['Qt compressed resources/QML caches not decoded', 'Scene/sample data not scanned',
                                  'Derived names are hypotheses, not confirmed visible labels', 'No automated UI traversal performed']})]:
        (args.output/name).write_text(json.dumps(obj, ensure_ascii=True, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(counts)))


if __name__ == '__main__':
    main()
