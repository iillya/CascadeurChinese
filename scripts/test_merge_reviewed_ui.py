import unittest
import json
from merge_reviewed_ui import unique, PLACEHOLDER, write_atomic
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch


class MergeTests(unittest.TestCase):
    def test_atomic_write_preserves_on_replace_failure(self):
        with TemporaryDirectory() as folder:
            path = Path(folder) / 'dictionary.json'
            path.write_text('original', encoding='utf-8')
            with patch('merge_reviewed_ui.os.replace', side_effect=PermissionError):
                with self.assertRaises(PermissionError):
                    write_atomic(path, {'translations': {'File': '文件'}})
            self.assertEqual(path.read_text(encoding='utf-8'), 'original')
            self.assertEqual(list(Path(folder).iterdir()), [path])
            write_atomic(path, {'translations': {'File': '文件'}})
            self.assertEqual(json.loads(path.read_text(encoding='utf-8'))['translations']['File'], '文件')

    def test_duplicate_rejected(self):
        with self.assertRaises(ValueError):
            json.loads('{"a":"x","a":"y"}', object_pairs_hook=unique)

    def test_placeholders(self):
        self.assertEqual(PLACEHOLDER.findall('Open %1 of %2: %s'), ['%1', '%2', '%s'])
        self.assertEqual(PLACEHOLDER.findall('打开 %1，共 %2：%s'), ['%1', '%2', '%s'])

    def test_unique(self):
        self.assertEqual(unique([('Open', '打开')]), {'Open': '打开'})


if __name__ == '__main__':
    unittest.main()
