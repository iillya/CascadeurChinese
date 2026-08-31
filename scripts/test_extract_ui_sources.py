import unittest
import zlib
import contextlib
import io
import json
from pathlib import Path
import tempfile
from unittest.mock import patch
import extract_ui_sources
from extract_ui_sources import (decode_payload, text_kind, qml_records, python_records,
                                reviewable, host_source, uncompressed_qml, json_records, load_json_text,
                                normalized_key)


class SourceExtractionTests(unittest.TestCase):
    def test_offline_scan_preserves_inputs_and_keeps_uncertain_separate(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            host, project, output = base/'host', base/'project', base/'output'
            host.mkdir()
            (project/'translations').mkdir(parents=True)
            formal = project/'translations/dictionary_zh.json'
            formal.write_text('{"translations":{"Save":"保存"}}', encoding='utf-8')
            original_formal = formal.read_bytes()
            qml = b'import QtQuick\nText { text: "Save" }\nText { text: "New Label" }'
            mapping = json.dumps([{'name':'Menu', 'items':[{'name':'Menu item','id':'act.open'}]}]).encode()
            binary = host/'presenter_lib.dll'
            original_binary = b'raw'+zlib.compress(qml)+zlib.compress(mapping)
            binary.write_bytes(original_binary)
            scripts = host/'resources/scripts/python'
            scripts.mkdir(parents=True)
            (scripts/'definition.py').write_text("raise RuntimeError('must not execute')\nobj.get_property('internal_key')", encoding='utf-8')
            with patch.object(extract_ui_sources, 'ROOT', project), patch('sys.argv',
                    ['extract', '--root', str(host), '--output', str(output)]), contextlib.redirect_stdout(io.StringIO()):
                extract_ui_sources.main()
            review = json.loads((output/'review_zh.json').read_text(encoding='utf-8'))['translations']
            self.assertIn('New Label', review)
            self.assertIn('Menu item', review)
            self.assertNotIn('Save', review)
            self.assertNotIn('internal_key', review)
            self.assertEqual(binary.read_bytes(), original_binary)
            self.assertEqual(formal.read_bytes(), original_formal)

    def test_zlib_offset_and_trailing_data(self):
        text = b'import QtQuick\nText { text: "Hello" }'
        self.assertEqual(decode_payload(b'junk'+zlib.compress(text)+b'tail', 4, 'zlib'), (text, 'ok'))
        self.assertEqual(text_kind(text)[0], 'qml_js')

    def test_limits_and_truncation(self):
        packed = zlib.compress(b'a'*10000)
        self.assertEqual(decode_payload(packed, 0, 'zlib', 100)[1], 'output_limit')
        self.assertEqual(decode_payload(packed[:-2], 0, 'zlib')[1], 'incomplete_or_input_limit')

    def test_binary_is_not_qml(self):
        self.assertEqual(text_kind(b'\0import QtQuick\n{}'), (None, None))

    def test_uncompressed_qt_resource_length_and_bounds(self):
        payload = b'import QtQuick\nText { text: "Open" }'
        data = len(payload).to_bytes(4, 'big')+payload+b'junk'
        self.assertEqual(list(uncompressed_qml(data, 0, len(data)))[0][1], payload)
        self.assertEqual(list(uncompressed_qml(data[:-6], 0, len(data)-6)), [])

    def test_json_names_need_schema_not_bone_instances(self):
        doc = [{'name':'Settings', 'items':[{'name':'Show Textures', 'id':'APP/Textures'}]},
               {'name':'hand_r', 'index':1}]
        records = {r['text']:r for r in json_records(doc)}
        self.assertEqual(records['Show Textures']['role'], 'named_ui_definition')
        self.assertEqual(records['Show Textures']['identifier'], 'APP/Textures')
        self.assertEqual(records['Show Textures']['groups'], ['Settings'])
        self.assertEqual(records['hand_r']['role'], 'json_literal_unconfirmed')

    def test_json_hash_comments(self):
        self.assertEqual(load_json_text('# generated\n{"color":"#fff"}'), {'color':'#fff'})

    def test_runtime_normalization_keeps_original_separate(self):
        self.assertEqual(normalized_key('  &Global_matrix… '), 'global matrix')
        self.assertEqual(normalized_key('Ｆｉｌｅ...'), 'file')
        self.assertEqual(normalized_key('Café *'), 'cafe')

    def test_native_name_transform_recorded_not_invented(self):
        records = list(qml_records('property string name: TextUtils.fromAnyCaseString(viewProperty.propertyName)'))
        self.assertTrue(any(r['role']=='display_binding' for r in records))
        self.assertTrue(any(r['role']=='name_transform_reference' for r in records))
        self.assertFalse(any('text' in r for r in records))

    def test_review_filter_preserves_inventory_not_colors(self):
        self.assertFalse(reviewable('#cdcdcd'))
        self.assertFalse(reviewable('X'))
        self.assertTrue(reviewable('Node3d'))
        self.assertFalse(host_source('Qt6Quick.dll!.rdata@0x11'))
        self.assertTrue(host_source('presenter_lib.dll!.rdata@0x11'))

    def test_zstd_bounds_trailing_and_truncated(self):
        try:
            import zstandard
        except ImportError:
            self.skipTest('optional zstandard is not installed')
        packed = zstandard.ZstdCompressor().compress(b'text'*100)
        self.assertEqual(decode_payload(packed+b'trailing', 0, 'zstd'), (b'text'*100, 'ok'))
        self.assertEqual(decode_payload(packed, 0, 'zstd', 10)[1], 'output_limit')
        with self.assertRaises(zstandard.ZstdError):
            decode_payload(packed[:-2], 0, 'zstd')

    def test_qml_sources_comments_and_fragments(self):
        records = list(qml_records('''// text: "comment"
Text { text: "Label" }
Text { text: "Prefix " + model.name }
Text { text: qsTr("Save\\nscene") }
Text { text: qsTranslate("Context", "Open") }
property string title: model.displayName
'''))
        values = {r['text']: r['role'] for r in records if 'text' in r}
        self.assertNotIn('comment', values)
        self.assertEqual(values['Label'], 'ui_literal')
        self.assertEqual(values['Prefix '], 'ui_expression_fragment')
        self.assertEqual(values['Save\nscene'], 'translation_source')
        self.assertEqual(values['Open'], 'translation_source')
        self.assertTrue(any(r['role'] == 'display_binding' for r in records))

    def test_python_identifiers_are_not_display_labels(self):
        records = list(python_records('''class Node3d:
    global_matrix = property(lambda self: self.get_property('global_matrix'))
def make():
    group.create_regular_data('Global Matrix', 0)
    editor.set_behaviour_data(obj, 'global_matrix', data)
'''))
        self.assertEqual({r['role'] for r in records}, {'property_identifier', 'data_definition_name'})
        self.assertTrue(any(r['scope'] == 'Node3d' for r in records))
        self.assertNotIn('Global matrix', {r['text'] for r in records})


if __name__ == '__main__':
    unittest.main()
