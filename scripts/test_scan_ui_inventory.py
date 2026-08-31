import unittest

from scan_ui_inventory import QUOTED, UI_CONTEXT, plausible_ui, retain


class ScannerTests(unittest.TestCase):
    def test_literals_and_context(self):
        text = 'text: "Open..."; toolTip: "Keep %1 items"; identifier: "node_id"'
        matches = list(QUOTED.finditer(text))
        self.assertEqual([m.group("body") for m in matches], ["Open...", "Keep %1 items", "node_id"])
        self.assertTrue(UI_CONTEXT.search(text[:matches[1].start()]))

    def test_wide_retention(self):
        for text in ("IK", "X-Ray", "Open...", "Keep %1", "path/to/file", "internal_name"):
            self.assertTrue(retain(text))
        self.assertFalse(retain("123"))

    def test_review_is_separate(self):
        self.assertTrue(plausible_ui("Open..."))
        self.assertTrue(plausible_ui("Keep %1 items"))
        self.assertFalse(plausible_ui("internal_name"))


if __name__ == "__main__":
    unittest.main()
