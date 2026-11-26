#!/usr/bin/env python3
"""QM agent must use shared routingmanagerd IPC, not a refused TCP port."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / 'tools/configure-qm-agent-vsomeip.py'
spec = importlib.util.spec_from_file_location('configure_qm_agent', MODULE_PATH)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class QmConfigTests(unittest.TestCase):
    @staticmethod
    def source():
        return {
            'routing': {
                'host': {'name': 'routingmanagerd', 'unicast': '10.88.0.1', 'port': '31000'},
                'guests': {'unicast': '10.88.0.1', 'ports': [{'first':'1026','last':'65535'}]},
            },
            'unicast': '10.88.0.1',
            'logging': {'level': 'info', 'console': 'true', 'dlt': 'false'},
            'applications': [{'name': 'Controller QM Agent', 'id': '0x1005'}],
            'services': [{'service': '0x1005', 'instance': '0x01', 'unreliable': '30509'}],
            'service-discovery': {'enable': 'true', 'protocol': 'tcp'},
        }

    def test_ipc_routing_no_tcp_or_network_advertisement(self):
        with tempfile.TemporaryDirectory() as d:
            source = Path(d) / 'vsomeip.json'
            target = Path(d) / 'runtime/vsomeip.json'
            content = self.source()
            source.write_text(json.dumps(content), encoding='utf-8')
            self.assertEqual(module.create(source, target), 'routingmanagerd')
            result = json.loads(target.read_text())
            self.assertEqual(result['routing'], 'routingmanagerd')
            self.assertEqual(result['applications'], content['applications'])
            for name in ('unicast', 'services', 'service-discovery', 'device'):
                self.assertNotIn(name, result)
            self.assertNotIn('10.88.0.1', json.dumps(result))
            self.assertEqual(json.loads(source.read_text()), content)
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)

    def test_local_route_is_valid_and_security_remains(self):
        cfg = self.source()
        cfg['routing'] = 'routingmanagerd'
        cfg['security'] = {'check_credentials': 'true'}
        parsed = module.local_ipc_config(cfg)
        self.assertEqual(parsed['security'], cfg['security'])
        self.assertEqual(parsed['routing'], 'routingmanagerd')

    def test_bad_routing_host_rejected_without_overwriting_prior_configuration(self):
        with tempfile.TemporaryDirectory() as d:
            source = Path(d) / 'bad.json'
            target = Path(d) / 'target.json'
            source.write_text(json.dumps({'routing': {'host': {'name': 'other-router'}}}))
            target.write_text('original')
            with self.assertRaises(ValueError):
                module.create(source, target)
            self.assertEqual(target.read_text(), 'original')

    def test_missing_or_duplicate_agent_id_rejected(self):
        cfg = self.source()
        cfg['applications'] = [{'name':'Controller QM Agent','id':'0x1004'}]
        with self.assertRaises(ValueError):
            module.local_ipc_config(cfg)
        cfg['applications'] = [{'name':'Controller QM Agent','id':'0x1005'}] * 2
        with self.assertRaises(ValueError):
            module.local_ipc_config(cfg)

    def test_preflight_refuses_to_replace_shared_input(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'vsomeip.json'
            path.write_text(json.dumps(self.source()), encoding='utf-8')
            with self.assertRaises(ValueError):
                module.create(path, path)
            self.assertEqual(json.loads(path.read_text()), self.source())

    def test_no_network_calls_in_preflight(self):
        # The old preflight used a UDP connect to discover a QM local IP,
        # which still attempted a non-listening Root TCP host:31000.
        self.assertFalse(hasattr(module, 'local_address'))


if __name__ == '__main__':
    unittest.main()
