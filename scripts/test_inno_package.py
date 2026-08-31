"""Read-only packer contract tests. Live install smoke tests are opt-in."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("inno_package", ROOT / "source/inno/package.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class InnoPackageTests(unittest.TestCase):
    def test_generated_dictionary_policy(self):
        files, code = PACKAGE.includes([
            ("translations\\dictionary_zh.json", Path("C:/payload/dictionary_zh.json"), "a" * 64),
            ("CascadeurChineseHook.dll", Path("C:/payload/Hook.dll"), "b" * 64),
        ])
        self.assertIn("uninsneveruninstall", files)
        self.assertIn("ShouldInstallDictionary", files)
        self.assertIn(".inno\\defaults", files)
        self.assertIn("SetArrayLength(PayloadNames, 2)", code)
        self.assertNotIn("recursesubdirs", files)

    def test_reject_inno_injection(self):
        for path in ['bad"path', "bad\npath", "{app}/bad", "bad\0path"]:
            with self.assertRaises(ValueError):
                PACKAGE.iss_string(path)
        self.assertEqual(PACKAGE.pascal_string("user's folder"), "'user''s folder'")

    def test_template_safety_contract(self):
        script = (ROOT / "source/inno/CascadeurChinese.iss").read_text(encoding="utf-8")
        self.assertIn("CloseApplications=no", script)
        self.assertNotIn("[UninstallDelete]", script)
        self.assertNotIn("DelTree(", script)
        self.assertNotIn("powershell.exe", script.lower())
        self.assertIn("CascadeurChineseInstaller", script)
        self.assertIn("RestoreLegacy", script)
        self.assertIn("uninstallonly", script)

    def test_all_tasks_default_selected(self):
        script = (ROOT / "source/inno/CascadeurChinese.iss").read_text(encoding="utf-8")
        tasks = script.split("[Tasks]", 1)[1].split("[Files]", 1)[0]
        self.assertEqual(tasks.count('Name: "'), 3)
        self.assertNotIn("unchecked", tasks)
        self.assertIn("UsePreviousTasks=no", script)


if __name__ == "__main__":
    unittest.main()
