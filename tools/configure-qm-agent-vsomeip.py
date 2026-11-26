#!/usr/bin/python3
"""Create a QM-agent-only vsomeip IPC configuration.

The deployed QM /etc/vsomeip.json may describe a TCP routing host which is
*not* listening (observed 10.88.0.1:31000 -> ECONNREFUSED). Other working
AutoSD demo applications use routingmanagerd's shared Unix socket instead.
This script retains application identities and security policy but disables
network routing and network service offers for the native QM agent. It never
rewrites the shared /etc/vsomeip.json.
"""
import argparse
import json
import os
from pathlib import Path
import sys

ROUTER = 'routingmanagerd'
AGENT = 'Controller QM Agent'
AGENT_ID = 0x1005


def local_ipc_config(config: dict) -> dict:
    """Return a standalone local IPC configuration, without TCP routing."""
    routing = config.get('routing')
    name = routing.get('host', {}).get('name') if isinstance(routing, dict) else routing
    if name != ROUTER:
        raise ValueError(f'expected routing manager {ROUTER!r}, got {name!r}')
    applications = config.get('applications', [])
    if not isinstance(applications, list):
        raise ValueError('applications must be an array')
    names = set()
    ids = set()
    for application in applications:
        if not isinstance(application, dict):
            raise ValueError('application must be a JSON object')
        app_name = application.get('name')
        raw_id = application.get('id')
        if not isinstance(app_name, str) or not isinstance(raw_id, (str, int)):
            raise ValueError('application name and ID are required')
        app_id = int(raw_id, 0) if isinstance(raw_id, str) else raw_id
        if app_name in names or app_id in ids or not (1 <= app_id <= 0xffff):
            raise ValueError('duplicate or invalid SOME/IP client identity')
        names.add(app_name)
        ids.add(app_id)
        if app_name == AGENT and app_id != AGENT_ID:
            raise ValueError('Controller QM Agent must use client ID 0x1005')
    if AGENT not in names:
        raise ValueError('Controller QM Agent must have client ID 0x1005')
    # Take only the client-side settings we need. Do not unintentionally retain
    # TCP port definitions or service discovery from the partition manifest.
    # Keep any explicit security options so an IPC-only change cannot disable
    # an existing vsomeip security policy.
    result = {
        'routing': ROUTER,
        'logging': config.get('logging', {'level': 'warning', 'console': 'true', 'dlt': 'false'}),
        'applications': applications,
    }
    for key in ('security', 'security-update-whitelist', 'routing-credentials',
                'check_credentials', 'permissions-uds', 'network'):
        if key in config:
            result[key] = config[key]
    return result


def create(input_file: Path, output_file: Path) -> str:
    if input_file.resolve() == output_file.resolve():
        raise ValueError('refusing to overwrite the shared vsomeip configuration')
    config = json.loads(input_file.read_text(encoding='utf-8'))
    result = local_ipc_config(config)
    output_file.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = output_file.with_suffix('.tmp')
    try:
        with temporary.open('w', encoding='utf-8') as fd:
            os.fchmod(fd.fileno(), 0o600)
            json.dump(result, fd, indent=2)
            fd.write('\n')
        os.replace(temporary, output_file)
    finally:
        # On failure the previous active configuration is left untouched.
        temporary.unlink(missing_ok=True)
    return ROUTER


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=Path('/etc/vsomeip.json'))
    parser.add_argument('--output', type=Path,
                        default=Path('/run/auto-apps-qm-agent/vsomeip.json'))
    args = parser.parse_args()
    try:
        manager = create(args.input, args.output)
    except (OSError, RuntimeError, ValueError, KeyError, TypeError,
            json.JSONDecodeError) as exc:
        print(f'QM vsomeip IPC preflight failed: {exc}', file=sys.stderr)
        return 1
    print(f'QM vsomeip IPC: routing={manager}; client=0x1005; '
          f'socket={os.environ.get("SOCKET_PATH", "/run/vsomeip/vsomeip-0")}; '
          'no TCP routing requested', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
