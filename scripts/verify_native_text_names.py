"""Compare recovered Python rules with an independent Qt 6.5.3 reconstruction.

Default mode reconstructs the operations; --original invokes the hash-locked DLL.
The probe has no UI; no existing Cascadeur process is touched.
"""
import hashlib
import argparse
import json
import os
from pathlib import Path
import random
import string
import subprocess

from native_text_names import (DLL_SHA256, from_any_case, from_camel_case,
                               from_snake_case, from_domain_object_type)
from extract_ui_sources import normalized_key

ROOT = Path(__file__).resolve().parents[1]
HOST = Path('C:/Program Files/Cascadeur')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', action='store_true', help='Also load and call the hash-locked original DLL in a separate probe process')
    args = parser.parse_args()
    actual_hash = hashlib.sha256((HOST/'presenter_lib.dll').read_bytes()).hexdigest().upper()
    if actual_hash != DLL_SHA256:
        raise ValueError('Host version changed: re-analyze native functions before trusting these rules')
    schema = json.loads((ROOT/'analysis/ui-sources-final/property_schema.json').read_text(encoding='utf-8'))
    source_names = sorted({e['identifier'] for e in schema if e['identifier'].isascii()})
    examples = ['', 'global_matrix','AutoPhysics','IK','IKFK','IKEnabled','PointIKFKSettings',
        'XMLParser','Node3d','Node3D','Global Matrix','_Foo','a__b','A_BC',
        'utils::AutoPhysics','utils::some_name','utils::','ns1::Foo','A::B::Node3d',
        'ßName','İName','éName','中文Test','😀Test','a\rB','a\nB','a\0B']
    rng = random.Random(20260831)
    alphabet = string.ascii_letters+string.digits+'_:/ -\t\n\r\0'
    cases = list(dict.fromkeys(examples+source_names+[
        ''.join(rng.choice(alphabet) for _ in range(rng.randrange(0,65))) for _ in range(12000)]))
    work = ROOT/'build/text-utils-probe'
    work.mkdir(parents=True, exist_ok=True)
    input_file, output_file = work/'inputs.json', work/('original-results.json' if args.original else 'qt-results.json')
    input_file.write_text(json.dumps(cases,ensure_ascii=True),encoding='utf-8')
    env = dict(os.environ)
    env['PATH'] = str(HOST)+os.pathsep+env['PATH']
    # Isolate any settings initialized by dependency DLLs from user settings.
    for variable in ['APPDATA', 'LOCALAPPDATA']:
        isolated = work/variable.lower()
        isolated.mkdir(exist_ok=True)
        env[variable] = str(isolated)
    command = [str(work/'reference.exe'), str(input_file), str(output_file)]
    if args.original:
        command.append('--original')
    subprocess.run(command, env=env, cwd=work, check=True, timeout=60,
                   creationflags=subprocess.CREATE_NO_WINDOW)
    qt = json.loads(output_file.read_text(encoding='utf-8'))
    if qt['qtVersion'] != '6.5.3' or Path(qt['qtCorePath']).resolve() != (HOST/'Qt6Core.dll').resolve():
        raise ValueError('Reference probe did not load the intended host QtCore version')
    funcs = {'camel':from_camel_case,'snake':from_snake_case,'any':from_any_case,'domain':from_domain_object_type}
    mismatches, comparisons = [], 0
    for row in qt['rows']:
        if not row['input'].isascii():
            continue
        for mode, fn in funcs.items():
            comparisons += 1
            if fn(row['input']) != row[mode]:
                mismatches.append({'input':row['input'],'method':mode,'python':fn(row['input']),'qt':row[mode]})
    report = {'presenter_sha256': actual_hash, 'qtVersion':qt['qtVersion'], 'qtCorePath':qt['qtCorePath'],
        'verification_kind': 'Direct original DLL invocation in separate process' if args.original else
            'Disassembly-backed reconstruction + differential check against reconstructed Qt operations; not direct original DLL execution',
        'cases':len(cases),'comparisons':comparisons,'mismatches':mismatches,
        'unicode_policy':'Python converter rejects non-ASCII; Qt probe examples are retained, not generalized.',
        'examples':[r for r in qt['rows'] if r['input'] in examples]}
    if args.original:
        # Re-run the reconstruction on the exact same inputs, including Unicode.
        reference_file = work/'qt-results.json'
        subprocess.run([str(work/'reference.exe'), str(input_file), str(reference_file)],
                       env=env, cwd=work, check=True, timeout=60,
                       creationflags=subprocess.CREATE_NO_WINDOW)
        reference = json.loads(reference_file.read_text(encoding='utf-8'))
        native_rows, reference_rows = qt['rows'], reference['rows']
        if len(native_rows) != len(cases) or len(reference_rows) != len(cases):
            raise AssertionError('Probe result count differs from input count')
        cross_mismatches = []
        for native, reconstructed, expected in zip(native_rows, reference_rows, cases):
            if native['input'] != expected or reconstructed['input'] != expected:
                raise AssertionError('Probe reordered inputs')
            for mode in funcs:
                if native[mode] != reconstructed[mode]:
                    cross_mismatches.append({'input':expected, 'method':mode,
                        'original':native[mode], 'reconstructed':reconstructed[mode]})
        report['original_vs_qt_reconstruction'] = {
            'comparisons': len(cases)*len(funcs), 'mismatches':cross_mismatches,
            'includes_unicode_examples':True}
    report_name = 'native_text_utils_original_verification.json' if args.original else 'native_text_utils_verification.json'
    (ROOT/'analysis'/report_name).write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    if mismatches or (args.original and cross_mismatches):
        raise AssertionError(f'{len(mismatches)} mismatches; do not generate labels until resolved')
    formal = json.loads((ROOT/'translations/dictionary_zh.json').read_text(encoding='utf-8'))['translations']
    known = {normalized_key(k) for k in formal}
    generated = {}
    for e in schema:
        mode = None
        if e['role'] == 'property_identifier':
            mode = 'any'
        elif e['role'] == 'behaviour_identifier' or (e['role']=='enum_identifier' and e.get('scope')=='BehaviourName'):
            mode = 'domain'
        if not mode or not e['identifier'].isascii():
            continue
        label = funcs[mode](e['identifier'])
        if not label:
            continue
        item = generated.setdefault(label, {'formal_normalized':normalized_key(label) in known,
            'status':'verified_conversion_rule_applied_to_schema_identifier; UI visibility and metadata overrides not verified',
            'evidence':[]})
        item['evidence'].append(dict(e,conversion=mode))
    result = {'translations':{k:'' for k,v in sorted(generated.items()) if not v['formal_normalized']}}
    (ROOT/'analysis/native_name_candidates.json').write_text(json.dumps(generated,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (ROOT/'analysis/native_name_review_zh.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'cases':len(cases),'comparisons':comparisons,'mismatches':len(mismatches),
                      'schema_labels':len(generated),'not_in_dictionary':len(result['translations'])}))


if __name__ == '__main__':
    main()
