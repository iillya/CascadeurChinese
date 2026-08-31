"""Offline source/dictionary checks; no host scripts or installers are executed."""
import ast
from pathlib import Path
import unittest
from merge_reviewed_ui import read

ROOT = Path(__file__).resolve().parents[1]


class RepositoryIntegrityTests(unittest.TestCase):
    def test_python_syntax(self):
        for folder in (ROOT/'scripts', ROOT/'source'):
            for path in folder.rglob('*.py'):
                with self.subTest(path=path.name):
                    ast.parse(path.read_text(encoding='utf-8-sig'), filename=str(path))

    def test_formal_dictionary(self):
        entries = read(ROOT/'translations/dictionary_zh.json')['translations']
        self.assertTrue(entries)
        for source, target in entries.items():
            with self.subTest(source=source):
                self.assertTrue(source.strip())
                self.assertIsInstance(target, str)
                self.assertTrue(target.strip())
        self.assertEqual(entries['Pin'], '固定')
        self.assertEqual(entries['Fix'], '修复')
        self.assertEqual(entries['Space'], '空间')


if __name__ == '__main__':
    unittest.main()
