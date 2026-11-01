"""Offline structural tests; do not contact public MQTT or ThingSpeak."""
import ast
import json
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class TestRepository(unittest.TestCase):
    def test_python_syntax(self):
        for path in sorted((ROOT / 'gateway').glob('*.py')):
            with self.subTest(file=path.name):
                ast.parse(path.read_text(encoding='utf-8'))

    def test_wokwi_diagrams_parse(self):
        for path in sorted((ROOT / 'esp32-nodes').glob('*/diagram.json')):
            with self.subTest(file=path.name):
                diagram = json.loads(path.read_text(encoding='utf-8'))
                self.assertIn('parts', diagram)
                self.assertIn('connections', diagram)

    def test_topics_match(self):
        prefix = 'motor-lab/CHANGE_TO_A_UNIQUE_NONPERSONAL_ID'
        for path in sorted((ROOT / 'esp32-nodes').glob('*/*.ino')):
            self.assertIn(prefix, path.read_text(encoding='utf-8'))
        self.assertIn(prefix, (ROOT / 'gateway/gateway.py').read_text(encoding='utf-8'))

    def test_only_example_cloud_config(self):
        self.assertFalse((ROOT / 'gateway/config.json').exists())
        sample = json.loads((ROOT / 'gateway/config.example.json').read_text(encoding='utf-8'))
        self.assertEqual(sample['TS_WRITE_KEY'], 'REPLACE_WITH_YOUR_API_KEY')
        self.assertEqual(sample['TB_NODE1_ID'], '0')
        self.assertEqual(sample['TB_NODE2_ID'], '0')

if __name__ == '__main__':
    unittest.main()
