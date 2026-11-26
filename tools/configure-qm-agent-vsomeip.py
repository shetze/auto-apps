#!/usr/bin/python3
"""Build a QM-agent-only vsomeip configuration using its current local IPv4.

Do NOT modify /etc/vsomeip.json; it is shared with working QM applications.
The routing manager remains on the Root interface (e.g. 10.88.0.1); the
routing guest bind address must be owned by this QM network namespace.
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import socket
import sys


def local_address(manager: str, port: int) -> str:
    ipaddress.IPv4Address(manager)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect((manager, port))  # UDP connect performs routing, no packet sent
        address = sock.getsockname()[0]
    if ipaddress.IPv4Address(address).is_unspecified:
        raise RuntimeError('no QM local IPv4 address selected by kernel')
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as test:
        test.bind((address, 0))  # fail early if address cannot be locally bound
    return address


def create(input_file: Path, output_file: Path) -> str:
    cfg = json.loads(input_file.read_text(encoding='utf-8'))
    host = cfg['routing']['host']
    manager = host['unicast']
    address = local_address(manager, int(host.get('port', 31000)))
    cfg['routing'].setdefault('guests', {})['unicast'] = address
    # Top-level unicast, when configured, is the local endpoint as well.
    if 'unicast' in cfg:
        cfg['unicast'] = address
    output_file.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = output_file.with_suffix('.tmp')
    with temporary.open('w', encoding='utf-8') as fd:
        os.fchmod(fd.fileno(), 0o600)
        json.dump(cfg, fd, indent=2)
        fd.write('\n')
    os.replace(temporary, output_file)
    return address


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=Path('/etc/vsomeip.json'))
    parser.add_argument('--output', type=Path,
                        default=Path('/run/auto-apps-qm-agent/vsomeip.json'))
    args = parser.parse_args()
    try:
        local = create(args.input, args.output)
    except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f'QM vsomeip preflight failed: {exc}', file=sys.stderr)
        return 1
    print(f'QM vsomeip: local guest {local}; host {json.loads(args.input.read_text())["routing"]["host"]["unicast"]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
