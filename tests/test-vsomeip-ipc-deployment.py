#!/usr/bin/env python3
"""Cross-check application IDs and AIB container/agent config deployment."""
import json
from pathlib import Path
import re
import unittest

try:
    import yaml
except ImportError:
    yaml = None

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = {
    'Multimedia': 0x1000,
    'Engine': 0x1001,
    'Multimedia client': 0x1002,
    'Sensors': 0x1003,
    'Controller': 0x1004,
    'Controller QM Agent': 0x1005,
}
PATH = '/usr/share/auto-apps/vsomeip-ipc.json'


class VsomeipIpcTests(unittest.TestCase):
    def test_static_ipc_configuration_has_all_unique_client_ids(self):
        cfg = json.loads((ROOT/'config/vsomeip-ipc.json').read_text())
        self.assertEqual(cfg['routing'], 'routingmanagerd')
        self.assertNotIn('unicast', cfg)
        self.assertNotIn('services', cfg)
        self.assertNotIn('service-discovery', cfg)
        self.assertEqual(len(cfg['applications']), len(EXPECTED))
        by_name = {entry['name']: int(entry['id'], 0)
                   for entry in cfg['applications']}
        self.assertEqual(by_name, EXPECTED)
        self.assertEqual(len(set(by_name.values())), len(EXPECTED))
        sources = {
            'engine-service.cpp': 'Engine',
            'mm-service.cpp': 'Multimedia',
            'mm-client.cpp': 'Multimedia client',
            'sensors-service.cpp': 'Sensors',
            'controller-service.cpp': 'Controller',
            'controller-qm-agent.cpp': 'Controller QM Agent',
        }
        for src, name in sources.items():
            self.assertIn(f'create_application("{name}")', (ROOT/src).read_text())

    @unittest.skipUnless(yaml, 'PyYAML unavailable; JSON/identity checks still run')
    def test_manifest_isolated_ipc_files_and_shared_socket(self):
        manifest = yaml.safe_load((ROOT/'auto-apps.aib.yml').read_text())
        root_files = {x['path']:x['text'] for x in manifest['content']['add_files']
                      if 'text' in x}
        qm_files = {x['path']:x['text'] for x in manifest['qm']['content']['add_files']
                    if 'text' in x}
        for path in ('/etc/containers/systemd/engine.container',
                     '/etc/containers/systemd/controller.container',
                     '/etc/containers/systemd/sensors.container'):
            self.assertIn('Environment=VSOMEIP_CONFIGURATION='+PATH,root_files[path])
            self.assertIn('Environment=SOCKET_PATH=/run/vsomeip/vsomeip-0',root_files[path])
            self.assertIn('Volume=/run/vsomeip/',root_files[path])
        multimedia = qm_files['/etc/containers/systemd/multimedia.container']
        self.assertIn('Environment=VSOMEIP_CONFIGURATION='+PATH,multimedia)
        self.assertIn('Environment=SOCKET_PATH=/run/vsomeip/vsomeip-0',multimedia)
        self.assertIn('Volume=/run/vsomeip/',multimedia)
        agent = qm_files['/etc/systemd/system/auto-apps-qm-agent.service']
        self.assertIn('VSOMEIP_CONFIGURATION=/run/auto-apps-qm-agent/vsomeip.json',agent)
        self.assertIn('ExecStartPre=',agent)
        self.assertIn('Environment=SOCKET_PATH=/run/vsomeip/vsomeip-0',agent)
        # Existing root/partition /etc/vsomeip.json must not be overwritten by
        # an IPC-only agent config. The agent preflight outputs to /run.
        self.assertIn('/etc/vsomeip.json',root_files)
        self.assertIn('/etc/vsomeip.json',qm_files)
        self.assertIn('Volume=/run/vsomeip/',
                      root_files['/etc/containers/systemd/qm.container.d/vsomeip-qm.conf'])

    def test_rpm_and_install_consistent(self):
        cmake = (ROOT/'CMakeLists.txt').read_text()
        rpm = (ROOT/'auto-apps.spec.in').read_text()
        self.assertIn('config/vsomeip-ipc.json',cmake)
        self.assertIn('%{_datadir}/auto-apps/vsomeip-ipc.json',rpm)
        self.assertIn('tests/test-vsomeip-ipc-deployment.py',cmake)


if __name__ == '__main__':
    unittest.main()
