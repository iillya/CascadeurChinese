"""Offline evidence inventory: compressed Qt payloads, QML bindings and Python schema.

Never executes host scripts, loads host DLLs, edits host files or merges translations.
Payload offsets are file offsets, NOT claimed qrc paths. Binary QML caches are
retained separately; arbitrary binary data is never treated as decoded QML.
"""
import argparse
import ast
from collections import Counter
import gzip
import hashlib
import json
import mmap
from pathlib import Path
import re
import unicodedata
import zlib

from scan_ui_inventory import BINARY, pe_regions

ROOT = Path(__file__).resolve().parents[1]
MAX_OUTPUT = 16 * 1024 * 1024
MAX_INPUT = 8 * 1024 * 1024
SIGNATURE = re.compile(rb'\x78[\x01\x5e\x9c\xda]|\x28\xb5\x2f\xfd')
PLAIN_QML = re.compile(rb'\x00[\x00-\xff]{3}(?=(?:\xef\xbb\xbf)?(?:import |pragma |//|/\*|\r?\n))')
TOKEN = re.compile(r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[A-Za-z_$][\w$]*|[^\s]''', re.S)
DISPLAY = {'text', 'title', 'label', 'headerText', 'toolTip', 'tooltip', 'placeholderText',
           'displayText', 'description', 'caption', 'statusTip', 'accessibleName',
           'header', 'textName', 'componentTextName', 'settingsName'}
PROPERTY_CALLS = {'get_property', 'set_property', 'get_behaviour_data', 'get_behaviour_setting',
                  'set_behaviour_data', 'set_behaviour_setting', 'set_behaviour_asset',
                  'set_behaviour_model_objects_to_range'}


def decode_payload(data, offset, codec, max_output=MAX_OUTPUT, max_input=MAX_INPUT):
    block = data[offset:offset + max_input]
    if codec == 'zlib':
        decoder = zlib.decompressobj()
        output = decoder.decompress(block, max_output + 1)
        if len(output) > max_output:
            return None, 'output_limit'
        if not decoder.eof:
            return None, 'incomplete_or_input_limit'
        return output, 'ok'
    try:
        import zstandard
    except ImportError:
        return None, 'zstd_dependency_missing'
    size = zstandard.frame_content_size(block)
    if size > max_output:
        return None, 'output_limit'
    output = zstandard.ZstdDecompressor().decompress(block, max_output_size=max_output,
                                                    allow_extra_data=True)
    return output, 'ok'


def text_kind(payload):
    try:
        text = payload.decode('utf-8-sig')
    except UnicodeError:
        return None, None
    if '\0' in text or any(ord(c) < 32 and c not in '\t\n\r' for c in text):
        return None, None
    if re.search(r'^\s*(?:pragma\s+\w+|import\s+[\w."/]+)', text, re.M) and '{' in text:
        return 'qml_js', text
    if text.lstrip().startswith(('{', '[')):
        try:
            json.loads(text)
            return 'json', text
        except ValueError:
            pass
    return 'text', text


def uncompressed_qml(data, start, end):
    """Conservative Qt length-prefixed, uncompressed QML payload carving."""
    for match in PLAIN_QML.finditer(data, start, end):
        offset = match.end()
        size = int.from_bytes(match[0], 'big')
        if not 16 <= size <= MAX_OUTPUT or offset + size > end:
            continue
        payload = data[offset:offset+size]
        kind, content = text_kind(payload)
        if kind == 'qml_js':
            yield offset, payload, content


def json_records(document, path='', groups=()):
    if isinstance(document, dict):
        name = document.get('name')
        group = isinstance(name, str) and isinstance(document.get('items'), list)
        leaf = isinstance(name, str) and isinstance(document.get('id'), str)
        for key, value in document.items():
            at = path+'/'+key
            if isinstance(value, str):
                role = 'json_literal_unconfirmed'
                if key in DISPLAY:
                    role = 'json_ui_literal'
                if key == 'name' and (group or leaf):
                    role = 'named_ui_group' if group else 'named_ui_definition'
                yield {'text': value, 'role': role, 'json_path': at,
                       'groups': list(groups), 'identifier': document.get('id')}
            else:
                yield from json_records(value, at, groups+(name,) if group else groups)
    elif isinstance(document, list):
        for i, value in enumerate(document):
            if isinstance(value, str):
                yield {'text': value, 'role': 'json_literal_unconfirmed', 'json_path': path+'/'+str(i)}
            else:
                yield from json_records(value, path+'/'+str(i), groups)


def load_json_text(text):
    # USD plugInfo uses full-line # comments. Preserve all quoted data verbatim.
    return json.loads(re.sub(r'(?m)^\s*#[^\n]*', '', text))


def string_value(token):
    try:
        value = ast.literal_eval(token)
        return value if isinstance(value, str) else None
    except (ValueError, SyntaxError):
        return None  # Preserve undecodable spelling in the original payload.


def reviewable(text):
    # Keep these in evidence inventory, but not in the natural-language shortlist.
    return (bool(re.search('[A-Za-z]{2}', text)) and
            not re.fullmatch(r'#[0-9a-fA-F]{3,8}', text) and
            not re.search(r'^(?:qrc:|:/|https?://)|\.(?:png|svg|qml|dll)$', text))


def normalized_key(text):
    """Mirror translation_policy.h for coverage only; never rename source keys."""
    text = text.replace('\xa0', ' ').replace('\u3000', ' ')
    text = text.translate({ord(c): None for c in '\u00ad\u200b\u200c\u200d\ufeff'})
    text = ''.join(chr(ord(c)-0xfee0) if 0xff01 <= ord(c) <= 0xff5e else c for c in text)
    text = text.translate(str.maketrans({'‘':"'", '’':"'", '“':'"', '”':'"', '–':'-', '—':'-'}))
    text = ' '.join(text.split())
    while text.endswith('…'):
        text = text[:-1]
    while text.endswith('...'):
        text = text[:-3]
    if text.endswith(' *'):
        text = text[:-2]
    text = text.replace('_', ' ').replace('&', '')
    text = ''.join(c for c in unicodedata.normalize('NFD', text)
                   if unicodedata.category(c) not in {'Mn','Mc','Me'})
    return ' '.join(unicodedata.normalize('NFC', text).casefold().split())


def host_source(source):
    first = source.split('!')[0].replace('\\', '/').lower()
    return (first.startswith('resources/scripts/') or
            ('/' not in first and not first.startswith(('qt', 'avcodec', 'avformat', 'avutil',
             'avfilter', 'swresample', 'swscale', 'torch', 'python', 'onnx', 'usd', 'lib',
             'msv', 'vc', 'catboost', 'icu', 'vcruntime'))))


def qml_records(text):
    tokens = [m for m in TOKEN.finditer(text) if not m[0].startswith(('//', '/*'))]
    for i, token in enumerate(tokens):
        raw = token[0]
        if raw[0] not in '\"\'':
            continue
        value = string_value(raw)
        if not value:
            continue
        role = 'qml_literal_unconfirmed'
        before = [m[0] for m in tokens[max(0, i - 6):i]]
        if len(before) >= 2 and before[-2:] in (['qsTr', '('], ['QT_TR_NOOP', '(']):
            role = 'translation_source'
        elif len(before) >= 2 and before[-1] == ':' and before[-2] in DISPLAY:
            following = tokens[i + 1] if i + 1 < len(tokens) else None
            terminal = following is None or following[0] in {';', '}'}
            if following and '\n' in text[token.end():following.start()] and following[0] not in {'+', '?', '.', '['}:
                terminal = True
            role = 'ui_literal' if terminal else 'ui_expression_fragment'
        elif len(before) >= 4 and before[-4] in {'qsTranslate', 'QT_TRANSLATE_NOOP'} and before[-3] == '(' and before[-1] == ',':
            role = 'translation_source'
        yield {'text': value, 'role': role, 'line': text.count('\n', 0, token.start()) + 1,
               'offset': token.start(), 'context': text[max(0, token.start()-100):token.end()+100]}
    # Binding expressions are evidence, not translatable strings. Keep even when
    # they contain no literal: model roles and helper calls often generate labels.
    for match in re.finditer(r'(?m)^\s*(?:property\s+(?:alias|string|var)\s+)?([\w.]+)\s*:\s*([^\n]+)', text):
        target, expression = match.groups()
        if target.split('.')[-1] in DISPLAY or 'TextUtils.from' in expression:
            yield {'role': 'display_binding', 'target': target, 'expression': expression.strip(),
                   'line': text.count('\n', 0, match.start()) + 1}
    for match in re.finditer(r'(?m)^.*(?:replace\s*\(|toUpperCase\s*\(|toLowerCase\s*\(|capitalize|displayName|display_name|splitName|TextUtils\.from).*$' ,text):
        yield {'role': 'name_transform_reference', 'expression': match[0].strip(),
               'line': text.count('\n', 0, match.start()) + 1}


def python_records(text):
    tree = ast.parse(text)
    parents = {child: node for node in ast.walk(tree) for child in ast.iter_child_nodes(node)}
    def scope(node):
        parts = []
        while node in parents:
            node = parents[node]
            if isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                parts.append(node.name)
        return '.'.join(reversed(parts))
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        method = getattr(node.func, 'attr', getattr(node.func, 'id', ''))
        args = node.args
        index, role = 0, None
        if method in PROPERTY_CALLS:
            index = 1 if method.startswith(('set_behaviour_', 'get_behaviour_')) else 0
            role = 'property_identifier'
        elif method in {'create_regular_data', 'create_setting', 'create_group'}:
            role = 'data_definition_name'
        elif method in {'add_behaviour', 'get_behaviour_by_name'}:
            index = 1 if method == 'add_behaviour' and len(args) > 1 else 0
            role = 'behaviour_identifier'
        elif method == 'add_link_by_standard_description_name':
            role = 'editor_description_reference'
        if role and len(args) > index and isinstance(args[index], ast.Constant) and isinstance(args[index].value, str):
            yield {'text': args[index].value, 'role': role, 'scope': scope(node),
                   'line': node.lineno, 'expression': ast.get_source_segment(text, node)}
        # Explicit enum list values are retained without calling constructors.
        if 'enum' in method.lower():
            for arg in args:
                if isinstance(arg, (ast.List, ast.Tuple)):
                    for item in arg.elts:
                        if isinstance(item, ast.Constant) and isinstance(item.value, str):
                            yield {'text': item.value, 'role': 'enum_definition_value',
                                   'scope': scope(node), 'line': item.lineno}
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and any('Enum' in ast.unparse(b) for b in node.bases):
            for member in node.body:
                if isinstance(member, (ast.Assign, ast.AnnAssign)) and isinstance(member.value, ast.Constant) and isinstance(member.value.value, str):
                    yield {'text': member.value.value, 'role': 'enum_identifier',
                           'scope': node.name, 'line': member.lineno}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('C:/Program Files/Cascadeur'))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--capture', type=Path)
    parser.add_argument('--reuse-payloads', type=Path, help='Re-index a completed extraction; verify payload SHA256')
    parser.add_argument('--binary', action='append', help='Optional relative binary path; default all binaries')
    args = parser.parse_args()
    root = args.root.resolve()
    output = args.output.resolve()
    if output == root or root in output.parents:
        parser.error('Output must not be inside the host installation')
    output.mkdir(parents=True, exist_ok=False)
    (output/'payloads').mkdir()
    formal = json.loads((ROOT/'translations/dictionary_zh.json').read_text(encoding='utf-8'))['translations']
    folded = {k.casefold() for k in formal}
    normalized = {normalized_key(k) for k in formal}
    capture = json.loads(args.capture.read_text(encoding='utf-8-sig'))['translations'] if args.capture else {}
    counts, errors, resources, candidates, bindings = Counter(), [], [], {}, []
    def add(record, source):
        record = dict(record, source=source)
        record['host_source'] = host_source(source)
        if 'text' not in record:
            bindings.append(record)
            return
        value = record.pop('text')
        if not value or not any(c.isalpha() for c in value):
            return
        item = candidates.setdefault(value, {'formal_exact': value in formal,
            'formal_casefold': value.casefold() in folded, 'runtime_observed': value in capture,
            'formal_normalized': normalized_key(value) in normalized,
            'evidence': []})
        if record not in item['evidence']:
            item['evidence'].append(record)
    def inspect(text, kind, source):
        if kind == 'qml_js':
            for record in qml_records(text):
                add(record, source)
        elif kind == 'json':
            for record in json_records(load_json_text(text)):
                add(record, source)
    def payload_record(payload, codec, source):
        digest = hashlib.sha256(payload).hexdigest()
        kind, content = text_kind(payload)
        filename = digest + ('.qml' if kind == 'qml_js' else '.json' if kind == 'json' else '.txt' if kind == 'text' else '.bin')
        destination = output/'payloads'/filename
        if not destination.exists():
            destination.write_bytes(payload)
        counts['payload_'+str(kind or 'binary')] += 1
        resources.append({'source': source, 'codec': codec, 'bytes': len(payload),
                          'kind': kind or 'binary', 'sha256': digest, 'file': 'payloads/'+filename})
        if content:
            inspect(content, kind, source)
    if args.reuse_payloads:
        reused_root = args.reuse_payloads.resolve()
        manifest = json.loads((reused_root/'resources.json').read_text(encoding='utf-8'))
        for entry in manifest:
            stored = (reused_root/entry['file']).resolve()
            if reused_root not in stored.parents:
                raise ValueError('Payload path escaped extraction directory')
            payload = stored.read_bytes()
            if hashlib.sha256(payload).hexdigest() != entry['sha256']:
                raise ValueError('Payload SHA256 mismatch: '+str(stored))
            payload_record(payload, entry['codec'], entry['source'])
        previous = json.loads((reused_root/'report.json').read_text(encoding='utf-8'))
        errors.extend(previous['errors'])
        for key, value in previous['counts'].items():
            if key.startswith(('zlib_', 'zstd_')) or key in {'binary_files','uncompressed_qml'}:
                counts[key] = value
    paths = sorted(root.rglob('*'))
    for path in paths:
        if not path.is_file() or any('chinese' in p.lower() for p in path.relative_to(root).parts):
            continue
        relative = path.relative_to(root).as_posix()
        suffix = path.suffix.lower()
        try:
            if suffix in BINARY and not args.reuse_payloads and (not args.binary or relative in args.binary):
                counts['binary_files'] += 1
                if not path.stat().st_size:
                    continue
                with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                    for start, size, section in pe_regions(data):
                        for offset, payload, _ in uncompressed_qml(data, start, start+size):
                            counts['uncompressed_qml'] += 1
                            payload_record(payload, 'none', f'{relative}!{section}@0x{offset:x}')
                        for match in SIGNATURE.finditer(data, start, start + size):
                            offset = match.start()
                            codec = 'zlib' if match[0][0] == 0x78 else 'zstd'
                            counts[codec+'_signatures'] += 1
                            try:
                                payload, status = decode_payload(data, offset, codec)
                            except Exception as exc:
                                # Most signatures in machine data are accidental.
                                counts[codec+'_invalid_frames'] += 1
                                if codec == 'zstd' and type(exc).__name__ != 'ZstdError':
                                    errors.append({'source': relative, 'offset': offset, 'error': str(exc)})
                                continue
                            if status != 'ok':
                                counts[codec+'_'+status] += 1
                                if status != 'incomplete_or_input_limit':
                                    errors.append({'source': relative, 'offset': offset, 'status': status})
                                continue
                            source = f'{relative}!{section}@0x{offset:x}'
                            payload_record(payload, codec, source)
                print(f'binary={counts["binary_files"]} payloads={len(resources)} candidates={len(candidates)} {relative}', flush=True)
            elif suffix in {'.qml', '.js', '.json'} and not args.binary:
                if path.stat().st_size > MAX_OUTPUT:
                    errors.append({'source': relative, 'status': 'text_size_limit'})
                    continue
                content = path.read_text(encoding='utf-8-sig')
                counts['loose_text_files'] += 1
                inspect(content, 'json' if suffix == '.json' else 'qml_js', relative)
            elif suffix == '.py' and relative.startswith('resources/scripts/'):
                content = path.read_text(encoding='utf-8-sig')
                for record in python_records(content):
                    add(record, relative)
                counts['python_schema_files'] += 1
        except (OSError, ValueError, SyntaxError) as exc:
            errors.append({'source': relative, 'error': str(exc)})
    strong = {'ui_literal', 'translation_source', 'json_ui_literal', 'named_ui_group', 'named_ui_definition'}
    review = {k: '' for k,v in candidates.items() if not v['formal_normalized'] and reviewable(k)
              and any(e['role'] in strong and e['host_source'] for e in v['evidence'])}
    counts.update({'unique_candidates': len(candidates), 'review_missing': len(review),
                   'binding_or_transform_records': len(bindings), 'decoded_payloads': len(resources),
                   'formal_exact': sum(v['formal_exact'] for v in candidates.values()),
                   'formal_normalized': sum(v['formal_normalized'] for v in candidates.values()),
                   'runtime_observed': sum(v['runtime_observed'] for v in candidates.values())})
    definitions = [{'text': k, **e} for k,v in candidates.items() for e in v['evidence']
                   if e['role'] in {'named_ui_group', 'named_ui_definition'} and e['host_source']]
    schema = [{'identifier': k, **e} for k,v in candidates.items() for e in v['evidence']
              if e['role'] in {'property_identifier', 'behaviour_identifier', 'data_definition_name',
                              'enum_identifier', 'enum_definition_value', 'editor_description_reference'}]
    counts['named_ui_definitions'] = len(definitions)
    counts['schema_records'] = len(schema)
    transform_calls = [b for b in bindings if 'TextUtils.from' in b.get('expression', '')]
    counts['native_name_transform_references'] = len(transform_calls)
    reports = {'resources.json': resources, 'bindings.json': bindings,
               'named_ui_definitions.json': definitions, 'property_schema.json': schema,
               'native_name_transforms.json': {'references': transform_calls,
                   'status': 'Call sites verified in embedded QML; native function implementation not decoded. No synthetic labels emitted.'},
               'review_zh.json': {'id': 'cascadeur-source-review-only', 'translations': dict(sorted(review.items()))},
               'report.json': {'root': str(root), 'counts': dict(counts), 'errors': errors,
                   'reused_payloads': str(args.reuse_payloads) if args.reuse_payloads else None,
                   'coverage_note': 'Normalization match is dictionary coverage, not proof that text renders translated. Review candidates still require human review.',
                   'limits': ['Compressed payload carving does not reconstruct qrc paths or prove all resources found.',
                              'Binary QML caches retained, not decompiled; missing source remains a gap.',
                              'Payloads capped at 16 MiB decoded / 8 MiB encoded.',
                              'QML literal/binding extraction is lexical, not a full JS evaluator.',
                              'Python identifiers and data definition names are NOT assumed UI labels.',
                              'No host code executed, no scene data read, no formal dictionary writes.']}}
    for name, document in reports.items():
        (output/name).write_text(json.dumps(document, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    with gzip.open(output/'candidates.json.gz', 'wt', encoding='utf-8') as stream:
        json.dump(candidates, stream, ensure_ascii=False, indent=2)
    print(json.dumps(dict(counts)))


if __name__ == '__main__':
    main()
