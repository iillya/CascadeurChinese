"""Read-only real-host preflight plus mixed/missing Qt fixture rejection.

Does not install, change associations, or write to either real host directory.
Run after build_inno.bat and test_inno.bat (for the inert host fixture).
"""
import ctypes
import json
from pathlib import Path
import shutil
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    support = ctypes.WinDLL(str(ROOT / 'build/inno/support.dll'))
    support.CheckTarget.argtypes = [ctypes.c_wchar_p, ctypes.c_int, ctypes.c_wchar_p, ctypes.c_uint]
    support.CheckTarget.restype = ctypes.c_int
    records = []

    def check(path, accepted, label):
        message = ctypes.create_unicode_buffer(1024)
        result = bool(support.CheckTarget(str(path), True, message, len(message)))
        records.append({'case': label, 'accepted': result, 'message': message.value})
        assert result == accepted, records[-1]
        print('PASS:', label)

    old = Path('E:/Cascadeur')
    check(old, True, 'real Cascadeur 2024.1.0 / Qt 6.5.1 preflight')
    check(Path('C:/Program Files/Cascadeur'), True, 'real Cascadeur 2026.1.2 / Qt 6.5.3 preflight')
    fixture = ROOT / 'build/compatibility-test' / ('installer-' + uuid.uuid4().hex[:10])
    fixture.mkdir(parents=True)
    inert = ROOT / 'build/inno/test-host.exe'
    modules = ['Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Qml.dll', 'Qt6Quick.dll']
    for name in ['cascadeur.exe', *modules]:
        shutil.copyfile(inert, fixture / name)
    check(fixture, True, 'consistent 6.5.3 fixture')
    shutil.copyfile(old / modules[0], fixture / modules[0])
    check(fixture, False, 'mixed 6.5.1 Core / 6.5.3 components rejected')
    for name in modules[1:]:
        shutil.copyfile(old / name, fixture / name)
    check(fixture, True, 'consistent 6.5.1 fixture')
    (fixture / modules[-1]).rename(fixture / 'Qt6Quick.dll.saved')
    check(fixture, False, 'missing Quick rejected')
    (fixture / 'Qt6Quick.dll.saved').rename(fixture / modules[-1])
    shutil.copyfile(inert, fixture / 'Qt5Core.dll')
    check(fixture, False, 'Qt5 coexistence rejected')
    (fixture / 'Qt5Core.dll').rename(fixture / 'Qt5Core.dll.saved')
    shutil.copyfile(ROOT.parent / '_ThirdParty/Qt/6.6.0/msvc2019_64/bin/Qt6Core.dll', fixture / modules[0])
    check(fixture, False, 'unsupported Qt 6.6 rejected')
    (ROOT / 'build/compatibility-test/installer-checks.json').write_text(
        json.dumps(records, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
