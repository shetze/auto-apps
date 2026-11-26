# Auto Apps Controller – root/QM integration (AutoSD 10 demonstrator)

## What changed

The AutoSD root-partition Controller must participate in SOME/IP as a Podman
**`ipc_t` container** (like Engine). It cannot run `systemctl` against the host
because the host systemd private socket and system bus are not inside the
container. The previous native controller unit and direct `systemctl` calls
were therefore not suitable for this deployment.

The image now contains three distinct components:

| Component | Where it runs | Role |
| --- | --- | --- |
| `controller.service` | Root Podman Quadlet, `SecurityLabelType=ipc_t` | SOME/IP health/status, UI, bounded tuning orchestration |
| `auto-apps-controller-host-agent.service` | Native root systemd | Inspect and adjust `qm.service`, `engine.service`, `sensors.service` cgroups |
| `auto-apps-qm-agent.service` | Native QM systemd | Inspect and adjust `multimedia.service` inside QM |

The native host agent does **not** join SOME/IP. The root controller and host
agent communicate via an **AF_UNIX SOCK_SEQPACKET** socket at
`/run/ipc/auto-apps-controller/host.sock`. Only the controller container has
this host-directory bind mount (read-only). The host agent checks peer UID,
accepts only fixed-size versioned 28-byte commands, and only allows the three
fixed Root units and the three cgroup knobs. It never executes arbitrary
commands or accepts arbitrary paths from SOME/IP. `systemctl` is executed
without a shell.

> **Not a safety controller.** This is a closed-network functional and resource
> interference demonstration, not certified FFI or protection against a
> malicious controller or routing-manager participant. SOME/IP client IDs are
> *not* authentication. Runtime changes to cgroup controls and RT settings
> require additional authorization and validation on the target platform.

## SOME/IP IPC routing and fixed client IDs

### Why the QM Agent was offline

On the observed AutoSD installation the native QM agent originally attempted
TCP routing to `10.88.0.1:31000`. First it failed with `EADDRNOTAVAIL (99)`
because the QM partition owns `10.88.0.3`, not `10.88.0.1`. After correcting
the local address it failed with `ECONNREFUSED (111)`: no usable TCP listener
was accepting connections at the configured Root address/port. This is a
**transport configuration mismatch**, not a lack of systemd privileges.

The working Engine and Multimedia applications use the `routingmanagerd`
**Unix-domain IPC** path exposed under `/run/vsomeip/vsomeip-0`. Rather than
introducing a second, currently nonfunctional TCP transport, this change makes
all demo SOME/IP clients explicitly use that existing routing mode.

- The RPM installs `/usr/share/auto-apps/vsomeip-ipc.json` with
  `"routing": "routingmanagerd"` and explicit client identifiers.
- Root Engine, Sensors and Controller Quadlets, and the QM Multimedia Quadlet
  set `VSOMEIP_CONFIGURATION` to this packaged file. `multimedia-client`
  launched with `podman exec` inherits the Multimedia container environment.
- The QM native agent retains its isolated startup preflight:

  ```sh
  /usr/bin/python3 /usr/libexec/auto-apps/configure-qm-agent-vsomeip.py \
    --input /etc/vsomeip.json \
    --output /run/auto-apps-qm-agent/vsomeip.json
  ```

  It now changes **only the agent's private configuration** to local
  `routingmanagerd` IPC and removes TCP host/guest addresses, network service
  offers and network service discovery. It preserves the configured application
  IDs and any supported security settings. No IP discovery, no hard-coded
  `10.88.0.3`, and no dependency on TCP port 31000 remain in the agent's
  generated configuration. It is regenerated on each agent start (mode `0600`).
- The Root and QM `/etc/vsomeip.json` files are **not edited**. Root/partition
  unit ownership and the restricted host-agent socket remain unchanged.

The explicit SOME/IP **client** IDs (independent of service IDs) are:

| Application | Client ID |
| --- | --- |
| Multimedia | `0x1000` |
| Engine | `0x1001` |
| Multimedia client | `0x1002` |
| Sensors | `0x1003` |
| Controller | `0x1004` |
| Controller QM Agent | `0x1005` |

All six applications now log their **actual** ID on startup and refuse to run
if it differs from the expected ID; this makes misdeployment visible instead
of silently generating `DENIED` replies. Engine and Multimedia continue to
reply `DENIED` for unexpected requesters, and Sensors and QM Agent now also
return that explicit status instead of dropping the request (`TIMEOUT`).
Client IDs are **not proof of identity or a security boundary**; privilege
separation and SELinux authorization still apply.

**Target limitation:** The change is based on the existing local IPC path; an
AutoSD 10 runtime check must confirm that the native QM agent can access the
shared socket under its SELinux domain and that this socket reaches the same
SOME/IP routing fabric as Engine, Controller, and Multimedia. If the QM agent
cannot reach the socket, inspect its unit's environment, socket mounts and
AVCs before introducing new capabilities or a TCP listener. A second routing
manager is not automatically a bridge between two independent SOME/IP domains.

## Controller display

`C` opens/closes the Controller view. `T` enables editing (if the deployment
tuning gate is enabled); Up/Down chooses a knob, `+/-` changes a proposal,
Enter submits it, and Esc cancels. Nothing is treated as successfully applied
until an acknowledgement arrives.

The v2 Controller status snapshot (`ATC2`) now carries a separate read status
for **each of the seven settings on each of four nodes**. It displays:

- A measured value if available (`APPLIED`/OK, e.g. `QM CPUWeight: idle`);
- `default (100)` for unset systemd `CPUWeight`; `idle` for `CPUWeight=idle`;
- `unlimited` for `MemoryMax=infinity`;
- `-- (OFFLINE)`, `-- (TIMEOUT)`, `-- (DENIED)`, `-- (N/A)`,
  `-- (PENDING)` or `-- (UNKNOWN)` as appropriate;
- `n/a` for synthetic workload knobs on the QM partition.

Engine/Multimedia/Sensors synthetic CPU/RAM/MUTEX/RT values are still read via
`DEMO_TUNE_METHOD_ID` over SOME/IP. Each GET now has its own request serial,
4-second deadline and status. Reordered/stale responses are ignored. Failed
GETs and timeouts can therefore be diagnosed without confusing them with zero
values. The client must be rebuilt together with the Controller because the
snapshot wire format changes from ATC1 to ATC2. The individual tuning command
format remains ATC1 (version 1) and the service IDs remain unchanged.

Unknown read values cannot be edited; this avoids applying arbitrary default
limits (such as decreasing QM memory to 20%) when monitoring is unavailable.

## Supported runtime operations

| Knob | Root host agent | QM agent | Service SOME/IP |
| --- | --- | --- | --- |
| `MemoryMax` | QM, Engine, Sensors | Multimedia | — |
| `CPUWeight` | QM, Engine, Sensors | Multimedia | — |
| Effective CPU affinity / `AllowedCPUs` | QM, Engine, Sensors | Multimedia | — |
| Synthetic CPU, RAM, mutex | — | — | Engine, Multimedia, Sensors |
| RT thread priority | — | — | Engine, Multimedia, Sensors |

QM memory tuning is bounded to 20–40% of host RAM and cannot raise the
configured 40% ceiling. Per-service memory caps are 0 (unlimited) or 64–4096
MiB and require 128 MiB headroom before tightening a finite cap. CPU affinity
is restricted to the effective parent set and a 32-CPU protocol mask. The
existing conservative QM-only synthetic CPU mitigation is retained.
`SCHED_FIFO` requires service-specific opt-in and appropriate kernel privileges.

Both the Container Controller (`AUTO_APPS_CONTROLLER_TUNING=1`) and native Host
agent (`AUTO_APPS_HOST_AGENT_TUNING=1`) are enabled in the supplied demo AIB
manifest. For read-only deployments, disable **both** gates, plus any QM-side
write authorization. Neither gate replaces proper authentication.

## Deployment / verification

The changed AIB manifest and RPM must be rebuilt into the AutoSD image. **Do
not hand-edit shared QM `/etc/vsomeip.json` or restart all services just to
fix the agent's IPC mode.** A project overlay does not update a running image.

New image (Root context):

```sh
systemctl status controller.service auto-apps-controller-host-agent.service
systemctl status qm.service engine.service sensors.service
ls -ldZ /run/ipc/auto-apps-controller
ls -lZ /run/ipc/auto-apps-controller/host.sock
journalctl -b -u auto-apps-controller-host-agent.service -u controller.service --no-pager
podman ps --format '{{.Names}} {{.Status}}'
```

New image (inside **QM** systemd namespace):

```sh
systemctl status auto-apps-qm-agent.service
journalctl -b -u auto-apps-qm-agent.service --no-pager -n 100
python3 -c 'import json; d=json.load(open("/run/auto-apps-qm-agent/vsomeip.json")); print(d["routing"])'
ip -br -4 addr
systemctl show multimedia.service -p MemoryMax -p CPUWeight -p ControlGroup
```

The QM-agent preflight should print `routing=routingmanagerd`,
`client=0x1005`, and `no TCP routing requested`. All application journals
should include an entry such as `VSOMEIP identity: Controller client=0x1004`.
The previous `Cannot bind to client port 31493` and `Connection refused
10.88.0.1:31000` errors should be absent. If the QM agent still fails to
register, inspect `/run/vsomeip/vsomeip-0` inside QM, the routing manager
logs and SELinux AVCs. If it registers but Multimedia cgroup values remain
unavailable, inspect the result of `systemctl show multimedia.service` from
inside QM (not Root).

### Target-system acceptance checks

On the Root system:

```sh
systemctl status controller.service auto-apps-controller-host-agent.service
podman exec systemd-controller sh -c 'echo "$VSOMEIP_CONFIGURATION"; grep -A25 applications /usr/share/auto-apps/vsomeip-ipc.json'
journalctl -b -u controller.service -u engine.service -u sensors.service --no-pager | grep 'VSOMEIP identity'
```

Inside QM (enter from Root with `podman exec -it qm /bin/bash`):

```sh
systemctl status auto-apps-qm-agent.service multimedia.service
python3 -c 'import json; d=json.load(open("/run/auto-apps-qm-agent/vsomeip.json")); print(d["routing"], d["applications"])'
ls -lZ /run/vsomeip/vsomeip-0
journalctl -b -u auto-apps-qm-agent.service --no-pager -n 80
```

The Controller display should receive **QM Multimedia cgroup** values and
**Engine/Multimedia/Sensors demo CPU/RAM/mutex/RT** GET replies, rather than
`OFFLINE`/`DENIED`/`TIMEOUT`. If the target system differs, the startup and
DENIED logs now identify the actual client IDs and the configured transport.
Verify **read-only values first** before enabling real tuning operations.

On an **existing** image with the former native controller unit, stop and
disable `auto-apps-controller.service` before switching to the Quadlet-based
`controller.service`. Never run both applications with client ID `0x1004`.
The rebuilt AIB image does not install the obsolete native unit.

SELinux is kept enforcing by the manifest. The new Unix socket is deliberately
private and no host systemd D-Bus socket is mounted inside the Controller
container. On a real AutoSD target, verify that the `ipc_t` Controller is
allowed to connect to the host agent's socket; if AVCs occur, grant only the
required socket `connectto` and path permissions, not general systemd access.
The precise site-specific labels and policy transition cannot be validated in
the current build environment. Rootful Podman with host UID 0 is expected;
rootless or remapped UIDs fail the Host agent's peer check.

## Tests and known limits

This change includes tests for the versioned controller status/CPUWeight
handling, Host-agent Unix socket protocol, QM UDS-only configuration generation
and explicit vsomeip application identity / AIB deployment consistency. The
previous demo modes, health, and sensors unit tests are unchanged.

**Target validation still required:** Build against real `vsomeip3-devel`,
check Quadlet generation and SELinux rules on AutoSD 10, ensure the native QM agent registers over the shared IPC socket, and test actual `systemctl set-property` and
`CAP_SYS_NICE` outcomes. Static syntax tests with a stub are not equivalent
to a target integration test.
