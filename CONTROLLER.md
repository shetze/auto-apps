# Auto Apps controller demonstrator (AutoSD 10)

## Partition design

- `controller-service` is a **native systemd service in the AutoSD root
  partition**, installed at `/usr/bin/controller-service` and enabled by
  `auto-apps.aib.yml` (`auto-apps-controller.service`). It is not embedded in QM.
- `controller-qm-agent` is a **native service in QM's own systemd instance**, not
  an application container. It can only operate on the single allowlisted
  `multimedia.service` unit in QM. The root controller handles `qm.service`,
  `engine.service`, and `sensors.service` via the root systemd manager.
- Engine and Sensors Quadlets in the supplied manifest reside in the root
  partition; Multimedia is inside QM. This is the actual manifest deployment,
  not a guessed layout.
- The controller observes Engine, Multimedia and Sensors over their existing
  health SOME/IP fields. It never parses the terminal or journal for health.
- The QM agent and controller use additional SOME/IP interfaces over the
  existing routing-manager socket. No arbitrary unit names, filesystem paths,
  or shell commands are accepted from a client.

AutoSD has a distinct systemd manager within the QM container. A root-only
`systemctl set-property multimedia.service` would target the wrong unit. This
agent explicitly avoids that mistake.

## User interface

Open the controller with **`C`** from radio, monitor, video or Fast. It is a
static screen, and interrupts moving-image presentation/CPU-boost leases just
like other overlays. `C` closes it and returns to the previous mode.

The header shows QM / Engine / Multimedia / Sensors side by side, including
live `MemoryMax`, `CPUWeight`, `AllowedCPUs` (effective cgroup CPU mask,
maximum 32 CPUs), service health and age, and all current synthetic
CPU/RAM/MUTEX/RT settings. Below it, a scrollable list presents:

| Setting | QM | Per-service | Change at runtime |
| --- | --- | --- | --- |
| `MemoryMax` | 20–40% of host RAM | 0=inherited or 64–4096 MiB | `systemctl set-property --runtime` |
| `CPUWeight` | 1–100, default 100 | 1–10000 | `systemctl set-property --runtime` |
| `AllowedCPUs` | CPU mask subset | CPU mask subset | `systemctl set-property --runtime` |
| Synthetic CPU | not applicable | 0–2000 ms (Sensors 0–300 ms) | immediate SOME/IP setting |
| Synthetic RAM | not applicable | 0–1024 MiB | reallocate in worker loop |
| Demo mutex | not applicable | free / held | immediate SOME/IP setting |
| RT priority | not applicable | SCHED_OTHER or FIFO 1–5 | `pthread_setschedparam` on worker |

The controller view requires a terminal at least **80×24** characters.
Use **`T`** to enable/disable local tuning, **Up/Down** to select a setting,
**`+/-`** to change the proposed value, and **Enter** to apply it. `Esc`
cancels a proposed change or exits the controller. The server publishes the
actual result (`APPLIED`, `PENDING`, `DENIED`, `OFFLINE`, `FAILED`). No optimistic
assumption is made when systemd or a service does not acknowledge the change.

The RPM and AIB configure the controller as a root-native systemd service;
there is deliberately no privileged host mount inside a QM application.

## Safety and privilege limitations

**DEMONSTRATOR ONLY. NOT SUITABLE AS A SAFETY CONTROLLER.** Do not deploy this
interactive tuner in a real road vehicle or claim it provides Freedom from
Interference. SOME/IP client IDs (`0x1002` multimedia UI and `0x1004`
controller) are routing identifiers, **not cryptographic authentication**.
A compromised QM client or routing configuration must not be trusted to
operate a privileged controller. The supplied AIB opt-in
`AUTO_APPS_CONTROLLER_TUNING=1` is intended only for a closed test network.
Disable the setting or run the controller without it for **read-only** use.
For production, implement authenticated authorization on an isolated control
plane, privilege separation, audited changes and policy-certified safeguards.

The QM `MemoryMax` controller cannot increase above the manifest's **40%**
ceiling. For any finite memory cap, the controller additionally rejects a
change if current cgroup usage would leave less than 128 MiB of headroom.
Limits are clamped, systemd commands are built from fixed allowlisted tokens,
and invoked by `execv`, never via a shell. No custom command or unit path can
be sent. Affinity can only be reduced to a subset of the currently effective
CPU set (or reset to inheritance). Cgroup changes use systemd runtime
properties and are non-persistent; an image reboot restores AIB defaults.

The controller **never automatically changes the Engine or root partition
resource budget**. An optional, conservative demo rule halves only the
*synthetic* Multimedia CPU workload after five consecutively degraded/failed
CPU-health samples, no more than once per minute. It does not alter the QM
partition boundary, real camera hardware, or the engine driving state.

RT priority starts at SCHED_OTHER / 0. Enabling SCHED_FIFO is denied unless
`AUTO_APPS_ALLOW_RT=1` is explicitly set for that particular service and the
worker is authorized by the OS (`CAP_SYS_NICE` and an appropriate RT limit).
RT scheduling is **not automatically enabled** in this image. Applying the
setting without those privileges produces `DENIED`, not a success indicator.
The 1–5 priority bound does not itself make scheduling safe under contention;
validate privileges and the Linux RT throttling policy on the actual hardware.

The root controller and QM agent require appropriate permissions to access
SOME/IP and the respective systemd managers. In SELinux enforcing mode, check
actual AVCs and adjust the site's policy narrowly. This patch does **not**
grant arbitrary SELinux access or disable enforcing mode.

## SOME/IP endpoints

| Application | Client ID | Service / instance | Operations |
| --- | --- | --- | --- |
| Controller | `0x1004` | `0x1004 / 0x0001` | `0x0001` tune, `0x8010` status (`0x4010`) |
| Controller QM Agent | `0x1005` | `0x1005 / 0x0001` | `0x0001` cgroup QM-local apply/get |
| Engine, Multimedia, Sensors | unchanged | existing IDs `0x1001`, `0x1000`, `0x1003` | `0x0008` demo knob set/get |

Controller commands are fixed **28-byte** big-endian records with magic
`ATC1`, version, request serial, target, setting, value and status. Snapshot
fields use `ATC1`, a sequence, fixed slots for four nodes, last action,
automatic intervention count and tuning gate. Unknown numeric values are
`0xffffffff`. The decoder rejects malformed lengths and invalid enums.
`status=UNKNOWN` represents a GET; `PENDING` requests a SET; status changes
are applied only after actual acknowledgements. The status FIELD is sent
once per second with `_force=true` so unchanged snapshots do not become stale.

## Running and checking on AutoSD

After building the RPM, rebuild and boot the image from `auto-apps.aib.yml`.
A tarball overlay on the project tree **does not** install systemd units into
an already deployed image. The image builder must be rerun, or the units
installed/enabled separately on a development system.

```bash
# root-partition manager
systemctl status auto-apps-controller.service qm.service engine.service sensors.service
systemctl show qm.service -p MemoryMax -p CPUWeight -p ControlGroup
journalctl -u auto-apps-controller.service -b --no-pager

# inside the QM partition, using its own systemd session
systemctl status auto-apps-qm-agent.service multimedia.service
systemctl show multimedia.service -p MemoryMax -p CPUWeight -p ControlGroup
journalctl -u auto-apps-qm-agent.service -b --no-pager

# review privilege / SELinux diagnostics if unavailable
ausearch -m AVC -ts recent
```

AutoSD's QM shell/access mechanism depends on the target image. Do not run
the QM-specific `systemctl` commands in the root manager: they name different
units. Use an actual QM shell or service-management transport such as BlueChi.

The controller's live systemd access and inter-partition SOME/IP routing have
**not** been tested on an AutoSD target in this delivery. Syntax and protocol
unit tests use local replacements for unavailable vsomeip development headers;
verify the RPM build, AIB image creation, root/QM permission model, set-property
outcomes, UI keys, automatic backoff and RT failure/success paths on the target.
