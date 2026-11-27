#!/usr/bin/env python3
"""The mock shares product wording; its extra catalog must not drift or omit copy."""
import json
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

MOCK = Path(__file__).resolve().parents[1]
PRODUCT = MOCK.parents[1] / "modules/remote/translations/ketplus_teams_vi.ts"


def catalog(path):
    return {
        message.findtext("source"): message.findtext("translation")
        for context in ET.parse(path).getroot().findall("context")
        if context.findtext("name") == "Team"
        for message in context.findall("message")
        if message.find("translation").get("type") != "unfinished"
    }


class TeamCopyCatalogTest(unittest.TestCase):
    def test_all_fixed_copy_has_a_translation(self):
        product = catalog(PRODUCT)
        extra = catalog(MOCK / "translations/mock_teams_vi.ts")
        paths = [MOCK / "qml/screens/TeamsScreen.qml",
                 MOCK / "qml/parts/TeamReviewFields.qml",
                 MOCK / "qml/parts/TeamGoalField.qml",
                 MOCK / "qml/schedules/TeamSchedules.qml"]
        sources = set()
        for path in paths:
            text = path.read_text()
            sources.update(json.loads(match, strict=False) for match in re.findall(
                r'copy\(("(?:\\.|[^"\\])*")\)', text))
            for one, many in re.findall(r'count\([^,\n]+,\s*"([^"]+)",\s*"([^"]+)"\)', text):
                sources.update([one, many])
        self.assertGreater(len(sources), 200)
        for source in sorted(sources):
            with self.subTest(source=source):
                self.assertTrue(product.get(source) or extra.get(source), "Missing Vietnamese copy")

    def test_mock_catalog_does_not_duplicate_product_copy(self):
        product = catalog(PRODUCT)
        extra = catalog(MOCK / "translations/mock_teams_vi.ts")
        self.assertFalse(set(product) & set(extra), "Reuse product translations instead of copying them")
        for source, translated in extra.items():
            self.assertEqual(set(re.findall(r"%\d+", source)),
                             set(re.findall(r"%\d+", translated)), source)


if __name__ == "__main__":
    unittest.main()
