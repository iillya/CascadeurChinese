import unittest
from native_text_names import from_any_case, from_camel_case, from_snake_case, from_domain_object_type


class NativeTextNameTests(unittest.TestCase):
    def test_known_fields(self):
        for source, expected in [('global_matrix','Global matrix'),('AutoPhysics','Auto physics'),
                ('Node3d','Node3d'),('IKFK','Ikfk'),('IKEnabled','Ik enabled'),
                ('XMLParser','Xml parser'),('PointIKFKSettings','Point ikfk settings')]:
            self.assertEqual(from_any_case(source), expected)

    def test_no_whitespace_cleanup(self):
        self.assertEqual(from_any_case('Global Matrix'), 'Global  matrix')
        self.assertEqual(from_any_case('_Foo'), '  foo')
        self.assertEqual(from_any_case('a__b'), 'A  b')
        self.assertEqual(from_any_case(''), '')

    def test_domain_is_not_any_case(self):
        self.assertEqual(from_domain_object_type('utils::AutoPhysics'), 'Auto physics')
        self.assertEqual(from_domain_object_type('utils::some_name'), 'Some_name')
        self.assertEqual(from_domain_object_type('utils::'), '')
        self.assertEqual(from_domain_object_type('ns1::Foo'), 'Ns1:: foo')

    def test_snake_preserves_remaining_case(self):
        self.assertEqual(from_snake_case('a_BC'), 'A BC')
        self.assertEqual(from_camel_case('a_BC'), 'A_bc')

    def test_unicode_explicitly_rejected(self):
        with self.assertRaises(ValueError):
            from_any_case('ßName')


if __name__ == '__main__':
    unittest.main()
