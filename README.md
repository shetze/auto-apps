# Auto Apps multimedia demo

This directory contains a set of applications that demonstrate how
typical automotive applications use SOME/IP to talk to each
other. These apps work both natively and in a container.

## Controller / resource tuning (AutoSD 10)

The SOME/IP `controller-service` runs as a **Root `ipc_t` Podman container**, in
parallel with Engine and Sensors. The restricted native
`controller-host-agent` exposes only fixed, bounded systemd resource operations
through a private Unix socket; it never participates in SOME/IP. The native
`controller-qm-agent` reads and updates Multimedia container resources in the
QM systemd manager. Its agent-specific vsomeip configuration is generated at
startup from the **current QM IP address** without altering the shared
`/etc/vsomeip.json`.

Press **`C`** in `multimedia-client` to open the Controller; **`T`** enters
interactive tuning. Per-knob read status now distinguishes unknown, denied,
offline, and timed-out values; `CPUWeight=idle` and default weights are
represented correctly. Rebuild both Controller and Multimedia client together:
the status snapshot wire format changed to `ATC2`. See
**[CONTROLLER.md](CONTROLLER.md)** for deployment, key bindings, safety and
verification. The Controller is a **demonstrator, not a safety controller**.

## The apps

### engine-service

The simulated engine follows a **60-second cycle** starting in Reverse:

| Phase | Duration | Elapsed time |
| --- | ---: | ---: |
| Reverse | 10 s | 0–10 s |
| Forward (Drive) | 20 s | 10–30 s |
| Fast | 10 s | 30–40 s |
| Forward (Drive) | 15 s | 40–55 s |
| Parking | 5 s | 55–60 s |

The engine transitions back to Reverse after Parking, unless the Parking
switch was enabled. With `P Parking: [ON]`, the engine **holds indefinitely at
the next Parking phase**, including when the client disconnects; switching it
`OFF` while parked starts a fresh **Reverse 10s → Forward 20s** sequence
immediately. Turning it off before Parking simply lets the current cycle run.
The engine owns the hold state, not the UI. Phase deadlines use a monotonic
clock and are anchored to the start of the cycle rather than to completion of
CPU/RAM demo work, avoiding accumulated timing drift. Very long scheduler or
memory stalls may still delay the publication of a transition.

The authoritative four-mode SOME/IP field uses event `0x8003` in eventgroup
`0x4008` of service `0x1001`; values are `Drive=0`, `Reverse=1`, `Fast=2`,
`Parking=3`. It is sent immediately on changes and refreshed every second.
The Parking switch uses idempotent method `0x0002` (0=OFF, 1=ON) and status
field `0x8004` in the same eventgroup. The existing reverse boolean field
(`0x8001`, eventgroup `0x4002`) and `get_reverse` method remain compatible
with sensors-service. Only Reverse activates the simulated rear camera.

With `ENGINE_DEMO_CPU_MS` set to the peak CPU budget per cycle, Drive, Reverse
and Parking use half the CPU work; Fast uses the full budget. A common work
period, instead of a shorter busy loop, creates a visible ~50%/~100%
single-core utilization contrast (subject to scheduler interference).

### multimedia-service

This is a service that emulates a radio, regularly publishing
information about the current song, radio station and volume. It
accepts requests to turn on/off, switch channel, and change volume.
If the engine service is available it subscribes to the direction events:
Reverse temporarily lowers volume to 30%, and Fast increases the normal
volume by **10 percentage points** (capped at 100%). Automatic changes do
not accumulate; on Drive the original base volume is restored. A missing mode
heartbeat also retires the volume boost after four seconds. Manual volume
changes during Reverse become the new base volume.

`RADIO_DEMO_CPU_MS` sets the full CPU-time budget per work cycle. Radio-only
playback uses half of this budget, while *actually visible* moving video or
live rear camera frames use the full budget. The client announces video
presentation using SOME/IP method `0x0006` (`RADIO_SET_VIDEO_ACTIVE_METHOD_ID`)
every second. The service drops the video load after three seconds without
a fresh presentation update. Browsing the video library, a Fast warning and
unavailable camera frames do not count as video playback. If neither radio
nor moving video is playing, no synthetic CPU workload is generated.

### sensors-service

This independent simulation service listens to engine direction and generates
rear-view camera frames plus four ultrasonic distances over reliable SOME/IP.
It requires no actual camera or ultrasonic hardware. It additionally publishes
an independent **1 Hz health heartbeat** (`0x8004`, eventgroup `0x4009`) using
the same 48-byte `demo_health` protocol as Engine and Multimedia. The metrics
come from the actual sensors worker thread (thread CPU time, elapsed time,
frame deadline misses and retained camera payload buffer capacity); this is
not whole-process RSS. An optional synthetic sensors CPU/RAM/MUTEX
workload is now separately adjustable through the Controller; its default is
zero, leaving the simulation workload unchanged. See below for the
wire format, configuration and safety limitations.

### multimedia-client

This is a command line program that displays the current state of the
multimedia service, as well as allowing you to control radio playback. The keyboard controls
are displayed on the screen. `V` opens a full-screen local ASCII/ANSI video
library with a minimum display size of 100x30 characters. Select animations
with the arrow keys and Enter. Playback loops until `X` returns to the video
selection; `V` returns directly to radio, and `Q` quits. `ESC` returns to the
selection while playing and to radio from the selection. With Reverse active, the camera preempts the monitor or video library,
but not the basic radio view. In **Fast**, an open video library or an active
animation is replaced by a **static safety notice** (no moving images); the
animation pauses and resumes at its previous frame when Drive returns.
`X`, `V` or Escape can close the blocked video view. If no video view is open,
the normal radio display remains visible and the automatic volume boost is
applied by multimedia-service. The client keeps Fast video blocked if the
last known Fast field becomes stale rather than assuming driving speed is low.

When the interference monitor is visible (`M`), `P` toggles its `Parking`
switch. The current setting is acknowledged by the Engine's SOME/IP field;
`[WAIT]` indicates an outstanding request and `[--]` means the state is not
yet known. The command is available even if a Reverse camera or Fast safety
warning overlays the expanded monitor. The compact monitor uses fixed-width
value columns for **Multimedia / Engine / Sensors**, independent of terminal
width. It reports the sensors worker's health and performance alongside its
camera state and (when fresh) all four ultrasonic distances. At least 70x17
terminal characters are needed for the first table rows; 70x23 displays the
complete table and explanation. If engine mode updates stop for over four
seconds, the last known mode remains visible marked `stale` rather than
flashing `? DIR`. A stale Reverse state does not automatically activate a
camera, and a stale Fast state does not unblock video.

## Building

The apps depend on Boost, vsomeip3, ncursesw and libcaca (the development
package is `libcaca-devel` on Fedora/EPEL). Build with CMake:

```
 $ cmake .
 $ make
```

There is also a makefile that allows building rpms and srpms:

```
 $ make -f Makefile.rpm srpm
 $ make -f Makefile.rpm rpm
```

## ASCII/ANSI videos

libcaca decodes multi-frame `.caca`, modern ANSI/UTF-8 `.ansi` / `.ans`
(form-feed + newline frame separators), single-frame text, and directories of
sorted ANSI frames. The multimedia-client renders the decoded frames into its own
full-screen ncursesw window, without starting another terminal application.
The input size is **not** fixed at 100x30: the terminal must provide at least
100x30 characters **and** be at least as large as the selected animation's
native dimensions. Smaller videos are centred. Files are loaded on selection;
input size and frame count are bounded.

The default library is `${CMAKE_INSTALL_DATADIR}/auto-apps/animations`
(e.g. `/usr/share/auto-apps/animations` in an RPM installation). The bundled
`city-traffic.ansi` is a complete 10-second demo, 100x30, 10 FPS.

To use a different local library, set:

```sh
export MM_VIDEO_DIR="$HOME/animations"
multimedia-client
```

A frame directory can contain `0000.ansi`, `0001.ansi`, etc. Playback defaults
to 10 FPS, but a plain text `.fps` sidecar containing an integer between 1 and
60 overrides this. For a file, use `my-video.caca.fps`; for a directory, use
`animation.fps`. `RADIO_VIDEO_DIR` remains accepted as a legacy override. `MM_CLIENT_LOG_FILE` configures the client log destination (legacy `RADIO_CLIENT_LOG_FILE` remains accepted). See `animations/README.md` for details.

In video mode, the multimedia service's SOME/IP connection and radio telemetry continue running.
Video playback does not issue volume, station or engine commands. It only
sends a leased presentation-state signal used to scale the service's synthetic
CPU workload; it is not a real audio/video decoder on multimedia-service. Older
Durdraw `.dur` files should first be exported to ANSI; the native `.dur`
format is not supported.

## Simulated rear camera and parking sensors

`sensors-service` is a **simulation only**: it does not open a camera device or
read real hardware. It listens to the engine's reverse field event via SOME/IP
and publishes a deterministic rear-view ASCII/ANSI animation together with four
matching ultrasonic distance readings. Engine-service remains authoritative for
reverse; sensors-service never changes vehicle direction.

The multimedia-client uses the following display priority:

| Engine | UI state | Visible content |
| --- | --- | --- |
| Forward / Unknown | Radio, monitor closed | Radio |
| Reverse | Radio, monitor closed | Radio (no camera) |
| Forward / Unknown | Monitor expanded (`M`) | Radio + interference monitor |
| Reverse | Monitor expanded (`M`) | Full-screen simulated rear camera |
| Forward / Unknown | Video library/player (`V`) | Local animation / browser |
| Reverse | Video library/player (`V`) | Full-screen simulated rear camera |

Local animation playback **pauses**, rather than restarting, while the camera
has priority. On leaving reverse, the same frame and the selection state are
restored. While reverse is active, `V` may close an open video library and `M`
may collapse an open monitor; `X` cannot dismiss the automatic camera overlay.
`Q` still exits. The simple radio display never forces the camera open.

Four versioned, independent SOME/IP field/eventgroups are provided by service
`0x1003`, instance `0x0001`:

| Event | Eventgroup | Meaning / wire format |
| --- | --- | --- |
| `0x8001` | `0x4005` | `CAM1`: camera frame with width, height, epoch, sequence, printable ASCII/color cells |
| `0x8002` | `0x4006` | `ULT1`: four ultrasonic distances in mm, validity mask, epoch, sequence |
| `0x8003` | `0x4007` | `CST1`: inactive/active/fault, epoch and sequence (1 Hz heartbeat) |
| `0x8004` | `0x4009` | `demo_health` v1: CPU/RAM health, real sensors-worker CPU share, frame overrun, buffer bytes, health sequence (1 Hz) |

**Transport:** The sensor instance uses reliable SOME/IP (TCP port `30511`)
configured in both `/etc/vsomeip.json` templates in `auto-apps.aib.yml`.
A 100x30 camera event contains ~6 KiB of data; it is intentionally **not**
transmitted as a fragmented UDP datagram. The ultrasound event is 5 Hz, and
camera frames are sent only while the engine reports Reverse. Subscribers
should use a sufficiently current vsomeip configuration and compatible event
definitions. Frames use bounded 2-byte cells (printable ASCII + 4-bit foreground
and background ANSI color indexes), rather than transmitting unsafe control
sequences. The client converts them to a libcaca canvas and uses the existing
ncursesw video renderer. Each sensor value is a simulation and must not be used
for real driver assistance or safety functions.

By default the camera runs at **100x30, 10 FPS**. The independent sensors-service
process supports the following environment overrides (restart to take effect):

```sh
SENSORS_CAMERA_WIDTH=160 SENSORS_CAMERA_HEIGHT=50 SENSORS_CAMERA_FPS=15 sensors-service
```

Camera dimensions may be 100..240 columns and 30..80 rows; FPS may be 1..30.
The terminal must fit both the 100x30 minimum and the native camera dimensions.
The overlay displays ultrasonic measurements in millimeters; values under
1500 mm are yellow and under 500 mm red (demo thresholds).

On service loss, fault status, expired status heartbeat (3 s), expired frame
(1.2 s), or expired ultrasonic sample (2 s), the client displays an explicit
unavailable/stale indication. It never reuses the last frame as live video.
A camera epoch prevents frames from an earlier Reverse period from being replayed.
The Sensors health event is subscribed independently of the camera, so its
performance remains visible while Forward/Fast/Parking have disabled frames.
CPU share is measured for the sensors worker thread, not all vsomeip helper
threads. Allocated RAM denotes the retained camera-frame payload buffer
capacity, not the resident memory of the entire service. The monitor marks
old health telemetry stale after four seconds without a valid heartbeat.

The CMake target, RPM manifest, container entrypoint and AIB QM deployment all
include `sensors-service`. A standalone `sensors.container` Quadlet is also
provided. When deploying on AutoSD, ensure the runtime SOME/IP JSON contains
application `Sensors` (`0x1003`) and the reliable service configuration. Run
`ctest --output-on-failure` after building; `sensors-test` exercises the wire
format, simulation, state transitions, freshness handling and larger frames.

## Parking-cycle and Sensors-health verification

Build and test with `cmake -S . -B build && cmake --build build &&
ctest --test-dir build --output-on-failure`. `demo-modes-test` exercises the
five phases and exact 60-second schedule, anchored deadline catch-up,
Parking hold/release including the immediate Reverse transition, legacy
reverse compatibility, Fast video blocking and CPU/volume rules.
`sensors-health-test` exercises the real-worker telemetry sampler, missed
frame deadlines, sequence monotonicity and `demo_health` wire format.
`demo-health-test` covers Sensors availability, telemetry aging and recovery.

On the target system, run all services with the updated binary and SOME/IP
configurations; expand the monitor with `M` and press `P`. The label should
change from `[OFF]` through `[WAIT]` to `[ON]` when acknowledged. When the
cycle reaches Parking it should remain there; pressing `P` again should
resume Reverse immediately, followed by 20 seconds Forward. Watch the
`ENGINE: CPU demo metrics` and Sensors health fields for CPU/performance
samples. The specified 60-second period is the requested simulation schedule,
not a real vehicle-control or functional-safety implementation.
