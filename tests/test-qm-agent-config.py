#!/usr/bin/env python3
"""Unit tests for the QM agent's isolated, runtime-generated vsomeip config."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

MODULE_PATH = Path(__file__).resolve().parents[1] / 'tools/configure-qm-agent-vsomeip.py'
spec = importlib.util.spec_from_file_location('configure_qm_agent', MODULE_PATH)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class QmConfigTests(unittest.TestCase):
    def test_guest_address_changes_but_not_original(self):
        with tempfile.TemporaryDirectory() as d:
            source = Path(d) / 'vsomeip.json'
            target = Path(d) / 'runtime/vsomeip.json'
            source_data = {
                'routing': {'host': {'unicast': '10.88.0.1', 'port': '31000'},
                            'guests': {'unicast': '10.88.0.1', 'ports': [{'first':'1026','last':'65535'}]}},
                'applications': [{'name': 'Controller QM Agent', 'id': '0x1005'}],
                'service-discovery': {'enable': 'true'},
            }
            source.write_text(json.dumps(source_data), encoding='utf-8')
            with mock.patch.object(module, 'local_address', return_value='10.88.0.3'):
                self.assertEqual(module.create(source, target), '10.88.0.3')
            result = json.loads(target.read_text())
            self.assertEqual(result['routing']['guests']['unicast'], '10.88.0.3')
            self.assertEqual(result['routing']['host']['unicast'], '10.88.0.1')
            self.assertEqual(result['routing']['guests']['ports'], source_data['routing']['guests']['ports'])
            self.assertEqual(result['applications'], source_data['applications'])
            self.assertEqual(json.loads(source.read_text()), source_data)
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)

    def test_actual_loopback_local_address(self):
        self.assertEqual(module.local_address('127.0.0.1', 31000), '127.0.0.1')

    def test_invalid_manager_does_not_override_config(self):
        with tempfile.TemporaryDirectory() as d:
            source = Path(d) / 'bad.json'
            target = Path(d) / 'target.json'
            source.write_text(json.dumps({'routing': {'host': {'unicast': 'bad.ip'}}}))
            with self.assertRaises(ValueError):
                module.create(source, target)
            self.assertFalse(target.exists())


if __name__ == '__main__':
    unittest.main()
