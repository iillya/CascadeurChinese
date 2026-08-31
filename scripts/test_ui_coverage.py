import gzip
import json
from pathlib import Path
import tempfile
import unittest
from build_ui_coverage import display_name, python_terms, inventory_records
from scan_ui_inventory import plausible_ui


class CoverageTests(unittest.TestCase):
    def test_slash_label(self):
        self.assertTrue(plausible_ui('IK/FK behaviour'))
        self.assertFalse(plausible_ui('C:/folder/file.qml'))

    def test_behaviour(self):
        rows = list(python_terms("point.add_behaviour('Dynamic', dynamic_name='Point ik fk settings')"))
        self.assertIn(('Point ik fk settings', 1, 'behaviour_or_property_reference'), rows)

    def test_settings(self):
        rows = list(python_terms("fk_ik_fk_settings = ['IK/FK behaviour', 'Collision in relaxation']"))
        self.assertTrue(all(role == 'settings_collection' for _, _, role in rows))

    def test_derivation(self):
        self.assertEqual(display_name('AutoPhysicsApply'), 'Auto physics apply')
        self.assertEqual(display_name('FulcrumPoint'), 'Fulcrum point')
        self.assertEqual(display_name('max_speed'), 'Max speed')

    def test_enum(self):
        rows = list(python_terms("class BehaviourName(enum.StrEnum):\n    FULCRUM_POINT='FulcrumPoint'"))
        self.assertEqual(rows[0][2], 'enum_value')

    def test_stream(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'terms.gz'
            expected = {'IK/FK behaviour': {'sources': ['x.py'], 'odd': '{ test }'}}
            with gzip.open(path, 'wt', encoding='utf-8') as stream:
                json.dump({'terms': expected}, stream, indent=2)
            self.assertEqual(dict(inventory_records(path)), expected)


if __name__ == '__main__':
    unittest.main()
