"""Read-only PE audit: resolve every direct Qt import against the selected host."""
import argparse
import json
import re
from pathlib import Path
import pefile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, default=Path('C:/Program Files/Cascadeur'))
    parser.add_argument('--dll', type=Path, default=ROOT / 'build/out/CascadeurChineseHook.dll')
    args = parser.parse_args()
    checked, errors = [], []
    exports = {}
    with pefile.PE(str(args.dll), fast_load=True) as image:
        image.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
        if image.FILE_HEADER.Machine != 0x8664:
            errors.append('Hook is not AMD64')
        for module in image.DIRECTORY_ENTRY_IMPORT:
            name = module.dll.decode('ascii')
            if not name.lower().startswith('qt'):
                continue
            path = args.host / name
            # QtGui/QtQuick exceed pefile's default 8192-export cap. Truncated
            # export parsing would falsely report valid imports as missing.
            with pefile.PE(str(path), fast_load=True, max_symbol_exports=65536) as host:
                host.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
                names = {e.name for e in host.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
                exports[name.lower()] = names
                ordinals = {e.ordinal for e in host.DIRECTORY_ENTRY_EXPORT.symbols}
                if len(names) != host.DIRECTORY_ENTRY_EXPORT.struct.NumberOfNames:
                    errors.append(name + ': incomplete export table; cannot certify imports')
                if host.FILE_HEADER.Machine != 0x8664:
                    errors.append(name + ': not AMD64')
                for entry in module.imports:
                    if entry.name is not None:
                        supported = entry.name in names
                        symbol = entry.name.decode('ascii')
                    else:
                        supported = entry.ordinal in ordinals
                        symbol = '#' + str(entry.ordinal)
                    checked.append({'module': name, 'symbol': symbol, 'present': supported})
                    if not supported:
                        errors.append(name + ':' + symbol)
    dynamic = re.findall(r'"(\?[^"\s]+)"', (ROOT/'source/hook.cpp').read_text(encoding='utf-8'))
    for symbol in dynamic:
        module = 'qt6quick.dll' if 'QQuickTextNode' in symbol else 'qt6gui.dll'
        if symbol.encode('ascii') not in exports.get(module, set()):
            errors.append(module + ':' + symbol)
    print(json.dumps({'qt_imports': len(checked), 'dynamic_hook_symbols': len(dynamic), 'missing': errors,
                      'note': 'Symbol/architecture check only; not proof of private Qt ABI or host UI behaviour.'}))
    if errors:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
