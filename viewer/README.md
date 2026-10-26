# HEZARFEN | SIMDIS Mission Visualization

Real C++20 visualization using the U.S. Naval Research Laboratory SIMDIS SDK. The viewer consumes backend events; it does not make security decisions. Live TCP is the default. Offline replay runs in the same native SIMDIS application and is explicitly labeled **REPLAY**. All displayed missions are synthetic demonstrations.

## Build on this Windows machine

Validated dependencies: Windows 10 x64 (10.0.19045), Visual Studio 2026 / MSVC 19.51.36248, CMake 4.3.4, SIMDIS SDK 1.25.0 (ABI 25), OpenSceneGraph 3.6.5, osgEarth, installed SDK data. The local SDK has **Debug libraries only**. CMake restricts the configuration to Debug to keep the application and SDK C++ runtimes compatible.

From the repository root in PowerShell:

```powershell
cmake -S viewer -B viewer/build-verified -G "Visual Studio 18 2026" -A x64 -DCMAKE_GENERATOR_INSTANCE=D:/VisualStudio
cmake --build viewer/build-verified --config Debug --target OfficialBasicViewer --parallel 4
cmake --build viewer/build-verified --config Debug --parallel 4
```

The `OfficialBasicViewer` target compiles the **unmodified** installed NRL `Examples/BasicViewer/BasicViewer.cpp`. It is optional when SDK source is unavailable. No external SDK source or installation is modified. `VSI::simVis` and `VSI::simUtil` are the actual imported SDK targets.

Override installation paths on another machine:

```powershell
cmake -S viewer -B viewer/build-verified -DSIMDIS_SDK_DIR=C:/SDK -DSIMDIS_DATA_DIR=C:/SIMDIS_SDK-Data -DSIMDIS_THIRD_PARTY_DIR=C:/sdk-dependencies -DSIMDIS_SOURCE_DIR=C:/simdissdk
```

`SIMDIS_THIRD_PARTY_DIR` contains dependency/version subdirectories, as in the local SDK distribution. Alternatively supply dependency prefixes using `CMAKE_PREFIX_PATH`. Use a generator matching the installed compiler. The native Winsock transport targets Windows; a Linux/macOS transport is not included.

## Run live first

Terminal 1, repository root:

```powershell
python -m viewer.presentation_backend --demo
```

Terminal 2, repository root:

```powershell
& ./viewer/build-verified/run_viewer.ps1
```

The generated launcher sets DLL, GDAL, plugin and SDK model/data paths and launches the compiled Debug executable. Keep the SDK installation and data available. To run the official smoke example, set the same runtime environment then run `viewer/build-verified/Debug/OfficialBasicViewer.exe`.

For interactive controls, start `viewer.presentation_backend` without `--demo` and open its panel at `http://127.0.0.1:8000`. The backend panel drives scenario events. The SIMDIS viewer observes the unchanged TCP feed. Its pause/resume buttons use a separate loopback HTTP control channel. There is no automatic switch from live data to a recording.

## Duraklat / Devam et / Yeniden baslat

Use `python -m viewer.presentation_backend --demo` for the presentation, then launch the viewer normally. **Yeniden baslat** in the SIMDIS window starts a fresh live 150-second automatic mission, including when the previous mission has finished or is paused. It resets positions, contacts, trust and links, clears pause and re-enables the complete automatic event schedule. The observer stays connected; message sequence numbers continue increasing and prior recordings remain preserved. This is a fresh simulation run, not playback of a recording. The header has **Duraklat** and **Devam et** buttons. Pause freezes the actual mission clock, all platform motion, scheduled security scenarios and outage restoration timers. Resume continues from that exact simulation time. Camera controls remain usable while paused; the feed stays connected and sends `HEARTBEAT.running=false` so the scene remains current rather than stale.

The presentation runner subclasses the existing backend Runtime from `viewer/`; it does not modify `backend/`, security rules, or the TCP v1 schema. Pause/resume/restart metadata is exposed only through the existing loopback HTTP service. Restart is enabled only when the runner advertises `restart_supported`; older runners need a restart to load the new code. The C++ WinHTTP client runs off the render thread, waits for actual backend state, and displays PAUSED after acknowledgement. HTTP port defaults to 8000; use `--control-port` in the viewer to match a custom runner `--http-port`.

`python -m backend.main` remains compatible with the viewer, but does not support pause/resume: these buttons are disabled until the pause-aware runner is used. Completed missions cannot be resumed as if paused. While paused, scenario injection buttons are rejected; Reset or selecting a new replay clears pause. Pause does not change trust, contacts, routes or the security audit.

In **offline viewer replay**, the same buttons pause the local replay reader. Relative event delays resume without catching up or skipping events. No backend is required in that mode. A recording's simulation timestamps exclude paused wall-clock time; a later replay does not reproduce the duration spent paused by a presenter.

Additional checks:

```powershell
python -m unittest viewer.tests.test_presentation_backend -v
python -m viewer.tests.pause_check
```

The second command launches the real native client and SDK viewer on temporary ports, checks pause/resume/restart acknowledgement and frozen coordinates, and creates `evidence/pause.png` with `evidence/pause.json`.

## Offline fallback

```powershell
& ./viewer/build-verified/run_viewer.ps1 --replay ./recordings/demo-events.jsonl
# Optional accelerated rehearsal (displayed as REPLAY):
& ./viewer/build-verified/run_viewer.ps1 --replay ./recordings/demo-events.jsonl --speed 15
```

Replay validates the complete JSONL stream first, preserves message order and relative `t_sec` timing, supports snapshot/reset boundaries, and plays once. It never generates or overwrites a recording. At EOF the final scene remains visible with **REPLAY COMPLETE**. Invalid JSONL exits with the failing line number. Supply one complete envelope per line, without a TCP prefix.

Camera: drag to rotate/pan, wheel to zoom, **Home** to recenter on the platforms, **1** perspective, **2** overhead, **Esc** exit. Ships and USVs use the SDK surface model at the received zero altitude; UAVs use the SDK airplane model at received positive altitudes. The command ship is larger than the USVs. Labels retain platform identity and current state; UAV-02 also retains its latest mission-command result.

## Dear ImGui mission monitor

The presentation UI is rendered with Dear ImGui **inside this C++ SIMDIS viewer**, owned by `feat/simdis-viewer`. The backend remains unchanged. The native 3D map and SDK models remain visible beside a dark mission monitor.

- Select a platform row and press **Focus** to inspect its actual position. Hover a row for role, WGS84 coordinates, heading and the latest command decision.
- **Mission**, **3D** and **2D** control the native SDK camera. Actions are applied on the next viewer update, not during UI drawing.
- The platform table shows independent security and link states. Contacts retain backend status and provenance. Expand **Security legend** for all supported colors.
- The scrollable event list preserves backend order. Search by platform, event ID, result, reason or evidence; **Follow** keeps new events in view. These controls only change the observer display.
- UI mouse/keyboard capture prevents camera gestures while interacting with a panel. Clicking the exposed SIMDIS scene returns normal camera control.

CMake compiles the SDK dependency's Dear ImGui 1.92.4 sources and OpenGL3 backend. The default is `${SIMDIS_THIRD_PARTY_DIR}/ImGui/1.92.4`; override `-DIMGUI_SOURCE_DIR=<imgui-source-directory>` if needed. No ImGui DLL or Python UI is used. ImGui's license remains with the installed dependency source. Missing sources produce an explicit configure error.

The application report includes `ui=Dear ImGui` and the number of UI frames rendered. Framebuffer captures are taken **after** ImGui rendering, so evidence includes the actual panels.

## Protocol and rendering

No `docs/protocol.md` exists in the backend branch. The integration follows `backend/tcp_server.py` and `backend/README.md`:

- Default endpoint `127.0.0.1:5556`; `--host` and `--port` override it.
- Exactly four unsigned big-endian length bytes, followed by 1–1,048,576 UTF-8 payload bytes. Fragmented and coalesced messages are read exactly; invalid lengths or invalid messages cause an explicit stale status and reconnect.
- v1 types: SNAPSHOT, DELTA, SECURITY_EVENT, CONTACT_EVENT, LINK_EVENT, HEARTBEAT. Required fields, numeric ranges, enums, UTF-8, duplicate JSON keys and bounded nesting are validated. Full JSON parsing uses vendored nlohmann/json 3.11.3 under its included MIT license.
- Only a fresh SNAPSHOT clears **STALE DATA** after connection/reconnection. The backend automatically sends it on connect. The feed is one-way; the viewer sends no unsupported request bytes. A five-second receive timeout detects silent peers.
- Sequence numbers must increase within a connection; a reconnect permits the fresh snapshot's sequence to restart. Repeated/out-of-order events are ignored. Snapshots replace platform/contact state, restore default UP connectivity, and subsequent LINK_EVENT messages restore any current outage. SDK identifiers stay stable for retained platform IDs.
- Parsed events cross a mutex-protected queue. All datastore, preferences, ImGui and scene operations run on the viewer thread. Queue overflow is reported and forces resynchronization; security events are never silently dropped to make room for telemetry.
- WGS84 latitude/longitude degrees and ellipsoid altitude meters pass through `simCore::CoordinateConverter::convertGeodeticToEcef()`. Heading is converted with the orientation to earth-centric radians. The datastore receives ECEF meters, verified through actual `simVis::PlatformNode::update()` readback. `View::lookAt()` uses **latitude, longitude** order in this SDK.
- Each received position replaces the SDK static TSPI sample (`time=-1`). The scene holds the last received position during idle/disconnect; it does not extrapolate motion or security decisions. The mission monitor separately preserves backend `t_sec`, `seq`, and optional `run_id`.
- SDK model overrides, label colors and native circle highlights show Trusted green, Watch yellow, Restricted orange, Quarantined red, Revoked gray. The current backend produces only Trusted/Watch/Restricted. The viewer never invents a quarantine or revocation.
- Link UP/DOWN is an independent label/roster field and never changes security color/state. Contact status/source come directly from the backend. The latest events show sequence, time, event ID, result, scope, reason and evidence references; historical audit details remain in backend recordings.
- Actual backend restrictions use `Restricted` state with backend decision results such as `REJECTED`; the viewer does not require the illustrative `APPLIED` result to display an explicit state. It supports that result for compatible recordings as well.

## Checks and evidence

```powershell
ctest --test-dir viewer/build-verified -C Debug --output-on-failure
python -m unittest discover -s backend/tests -v
python viewer/tests/integration_check.py
```

The last command opens actual graphics windows. It starts its own backend, exercises HTTP scenario controls, validates six active SDK nodes and their ECEF positions, disconnects and restarts the backend, then tests all 913 recorded messages in offline mode. Ports 5556/8000 must be free. It writes temporary backend recordings/logs under ignored `viewer/build-verified/`; it never edits `backend/` or `recordings/`.

`--capture <file.png> --capture-at <seconds> --quit-after <seconds> --report <file.json>` provide reproducible rendering evidence. Captures read the actual OpenGL framebuffer after HUD drawing. Reports include SDK IDs, active flags, SDK position readback, backend state, counts and recent provenance. Evidence reflects a synthetic run and is not an operational experiment.

See [VALIDATION.md](VALIDATION.md) for results, encountered build errors and limitations. The [evidence](evidence/) directory contains genuine native application captures and reports.

## Branch ownership

All deliverable changes are under `viewer/` on `feat/simdis-viewer`, based on backend commit `4f0bae2`. `backend/` and `recordings/` are inherited unchanged. Main is untouched. SDK binaries/resources are external dependencies and are not committed.
