# Auto Apps Controller – AutoSD 10 runtime resource demonstrator

The Controller is **demo-only**, not a safety-certified controller or an
alternative to AutoSD partition/SELinux isolation. SOME/IP client IDs and the
`tuning_enabled` flag are not authentication. Do not deploy writable tuning
outside a trusted, isolated demonstration setup.

## Architecture

| Process/unit | Placement | Responsibility |
| --- | --- | --- |
| `controller.service` (Quadlet) | Root `ipc_t` container | SOME/IP health, bounded tuning decisions, 1 Hz `ATC3` snapshots |
| `auto-apps-controller-host-agent.service` | Native Root systemd | Read/adjust only `qm.service`, `engine.service`, `sensors.service` |
| `controller-qm-agent.service` (Quadlet) | QM `qm_container_ipc_t` container | SOME/IP endpoint `0x1005` for multimedia cgroup requests |
| `auto-apps-qm-resource-agent.service` | Native QM systemd | Read/adjust only `multimedia.service` |

Neither SOME/IP container has a host systemd bus mounted. The two native
helpers use the same restricted `controller-host-agent` binary, in different
systemd namespaces and explicitly separate scopes:

- Root helper: default `AUTO_APPS_AGENT_SCOPE=root`; private `SOCK_SEQPACKET`
  socket `/run/ipc/auto-apps-controller/host.sock`.
- QM helper: `AUTO_APPS_AGENT_SCOPE=qm`; private `SOCK_SEQPACKET` socket
  `/run/auto-apps-qm-agent/qm.sock`.

Both allow only three specific cgroup knobs (MemoryMax, CPUWeight, AllowedCPUs),
validate target and values, check the connecting peer has UID 0, and accept
neither filesystem paths nor arbitrary systemctl arguments. The helper returns
actual values from `systemctl show` after successful writes. For QM, all
resource operations run under the **QM systemd manager**, not the Root manager.
The QM SOME/IP proxy uses the packaged `/usr/share/auto-apps/vsomeip-ipc.json`
with the proven UDS route, not the old, refused `10.88.0.1:31000` TCP endpoint.
The global `/etc/vsomeip.json` files are not modified. The older optional
`configure-qm-agent-vsomeip.py` utility remains available for diagnostics, but
is **not** in the QM agent's normal startup path.

**Migration:** The older native `auto-apps-qm-agent.service` must be absent or
disabled on an upgraded target, to prevent duplicate client ID `0x1005`.
Only the new QM Quadlet offers that SOME/IP service. The old native Root
`auto-apps-controller.service` likewise must not run beside `controller.service`.

AutoSD uses `ipc_t` for the Root container, `qm_container_ipc_t` for the QM
container, and separately managed systemd partitions. The QM proxy mounts the QM-local `/run/auto-apps-qm-agent` directory read-only;
the native QM helper owns `/run/auto-apps-qm-agent/qm.sock`. The QM systemd
manager creates this **private** directory with
`RuntimeDirectory=auto-apps-qm-agent` and mode `0700`. It is deliberately
**outside `/run/ipc`**, which AutoSD shares between the Root and QM partitions.
No Root-side tmpfiles entry or additional QM partition bind mount is needed
for this channel. Root/QM SOME/IP and DLT continue to use the existing shared
`/run/ipc` setup unchanged.

The QM resource helper uses `Type=notify`: it signals `READY=1` only after
`bind()` and `listen()` succeed. The QM Quadlet has `Requires=` and `After=`
on this helper, avoiding the earlier race where Podman attempted to mount a
nonexistent directory. `RuntimeDirectoryPreserve=restart` retains the same
inode during helper restarts; the helper's existing stale-socket checks still
apply. This avoids both observed startup failure classes:
`226/NAMESPACE` / Podman `125` (missing directory) and attempts to bind in
the Root-shared `ipc_var_run_t` directory (`Permission denied`).

**SELinux warning:** This change reduces cross-partition exposure, but it does
not prove the automotive SELinux policy permits the native QM helper to create
its socket or `qm_container_ipc_t` to connect to it. Diagnose any remaining
AVCs and add only targeted policy permissions; do not use `--privileged`,
`chmod 777`, or a host D-Bus mount. Target-system validation is required.

## Monitor and manual tuning

In `multimedia-client`, press `C` for the Controller view, `T` to enable edit,
Up/Down to select an option, `+/-` to change the proposal, Enter to submit and
Esc to cancel. Confirmation is asynchronous; `APPLIED` appears only after a
matching service/helper response. Live measurements have separate per-knob
statuses (`UNKNOWN`, `OFFLINE`, `TIMEOUT`, `DENIED`, `UNSUPPORTED`).

**CPUWeight includes `idle` below numeric `1`.** For example the QM row
allows `11 -> 1 -> idle` with `-`, and `idle -> 1` with `+`. The wire protocol
encodes `idle` as `0`; the systemd agent translates `0` into the literal
`CPUWeight=idle` (never `CPUWeight=0`). Numeric weights are bounded to
1–10000 for services and 1–100 for QM. A missing systemd value is displayed
as `default (100)`; `MemoryMax=infinity` is `unlimited`. CPUWeight and
memory/affinity bounds are independent from synthetic demo workload budgets.

The `Controller` snapshot protocol is **ATC3** (previously ATC2). It now
includes baseline/current synthetic Multimedia CPU ms, last automatic action
and cause, reductions, restorations and pending state. The Multimedia Client
must be rebuilt with the Controller. The existing fixed 28-byte `ATC1`
command protocol and SOME/IP service/client IDs are unchanged.

## Automatic synthetic CPU regulation

The Controller may adjust **only the Multimedia service's synthetic CPU-work
budget**. Never Engine workload, QM MemoryMax, CPUWeight, affinity, RAM,
mutex or RT scheduling automatically. It requires:

1. Opt-in `AUTO_APPS_CONTROLLER_TUNING=1` and a confirmed, current Multimedia
   CPU budget from the SOME/IP GET method.
2. Five consecutive fresh CPU-health samples reporting a measured CPU
   *deadline overrun* (not just `DEGRADED` due to RAM/mutex problems).
3. No current manual or automatic CPU-write request and at least 60 seconds
   since the last confirmed reduction. Samples with >=75% off-CPU time are
   treated as **ambiguous blocking/throttling**, not proven CPU demand, and
   cannot trigger automatic reduction.

Each reduction halves the current budget down to a minimum of **20 ms**, one
step per acknowledgement. A timeout invalidates the assumed current budget,
forces a new GET, and backs off instead of repeating writes blindly.

The policy captures the baseline from the first confirmed GET. Optionally
`AUTO_APPS_MM_CPU_BASELINE_MS=150` defines the desired initial target, even
if a previous Controller already reduced the running Multimedia process.
The supplied AIB manifest uses this value, matching
`RADIO_DEMO_CPU_MS=150`. A successful **manual** Multimedia CPU change becomes
the new baseline and cancels the current recovery interval.

When CPU **and available RAM health** remain normal and the last CPU deadline
miss is zero for **90 consecutive seconds**, the Controller restores at most
one doubling step toward (never beyond) that baseline. Each further step
requires another stable interval. Incoming RAM failure, CPU warnings without
measured overruns, missing health or a stale/unknown CPU GET all block new
automatic actions. Successful reductions and restorations are counted
separately; previous/confirmed budget and cause are shown in the display.
Every proposal, ACK/denial and timeout is recorded in `controller.service`
logs with the old and new CPU budgets.

This is a bounded **synthetic load experiment**, not a guarantee that actual
CPU utilization will meet any target under all workloads.

## Build and deployment

Unpack the change overlay at the project root, commit if building via
`Makefile.rpm` (`git archive HEAD`), and rebuild the RPM, application container
image, and AIB image from `auto-apps.aib.yml`.

Root checks:

```sh
systemctl status controller.service auto-apps-controller-host-agent.service
journalctl -b -u controller.service -u auto-apps-controller-host-agent.service --no-pager -n 100
podman exec systemd-controller sh -c 'echo "$VSOMEIP_CONFIGURATION"'
systemctl show qm.service engine.service sensors.service -p CPUWeight -p MemoryMax -p ActiveState
```

QM checks, **inside `podman exec -it qm /bin/bash` from Root**:

```sh
systemctl status controller-qm-agent.service auto-apps-qm-resource-agent.service multimedia.service
journalctl -b -u controller-qm-agent.service -u auto-apps-qm-resource-agent.service --no-pager -n 100
systemctl show auto-apps-qm-resource-agent.service -p Type -p ActiveState -p SubState -p NRestarts -p RuntimeDirectory
ls -ldZ /run/auto-apps-qm-agent
ls -lZ /run/auto-apps-qm-agent/qm.sock /run/vsomeip/vsomeip-0
findmnt -T /run/auto-apps-qm-agent -o TARGET,SOURCE,FSTYPE,OPTIONS
systemctl show multimedia.service -p CPUWeight -p MemoryMax -p ControlGroup -p ActiveState
```

For the client ID, look for `VSOMEIP identity: Controller QM Agent
client=0x1005`. The Root Controller should subsequently report QM agent
SOME/IP `AVAILABLE`. The previously `OFFLINE` Multimedia cgroup fields
should show `default (100)`, `unlimited`, and the effective cpuset (if the
local `systemctl show`/cpuset data is accessible). First verify **read-only**
values before testing any write. SELinux AVCs may still require specific
local policy rules for native-helper socket access on the target; no extra
kernel capabilities or blanket SELinux permissive mode are assumed here.

If the QM resource helper still fails, first inspect its **new** journal
entries. `HOST-AGENT: bind: Permission denied` after this path change is not
necessarily a mount error: check the process label, directory label and AVC
records. If the helper becomes active but the controller still reports
`OFFLINE`, check that the QM proxy can connect to the *same* socket inode in
its read-only bind mount and that SELinux allows the connect operation.
Do not silently treat an `active` Quadlet state as an end-to-end link test.

The QM native helper's write gate is `AUTO_APPS_HOST_AGENT_TUNING`; set to
`0` to disable QM-side systemd writes. The Root helper has the same separate
gate; both are explicitly `1` in this **demo** manifest. `SCHED_FIFO` stays
service-specific and requires additional explicit authorization.

## Validation limits

Local unit and configuration tests cover idle CPUWeight, mixed manual/auto
budget behavior, stable recovery, timeouts, and the Root/QM application/unit
layout. The actual AutoSD 10 image, its SOME/IP routing, systemd manager
access, SELinux permissions, and running cgroups must be validated on target.
