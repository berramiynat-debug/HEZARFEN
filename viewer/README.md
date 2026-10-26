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
python -m backend.main --demo
```

Terminal 2, repository root:

```powershell
& ./viewer/build-verified/run_viewer.ps1
```

The generated launcher sets DLL, GDAL, plugin and SDK model/data paths and launches the compiled Debug executable. Keep the SDK installation and data available. To run the official smoke example, set the same runtime environment then run `viewer/build-verified/Debug/OfficialBasicViewer.exe`.

For interactive controls, start the backend without `--demo` and open its panel at `http://127.0.0.1:8000`. The panel drives backend events. The SIMDIS viewer is a read-only observer. There is no automatic switch from live data to a recording.

## Offline fallback

```powershell
& ./viewer/build-verified/run_viewer.ps1 --replay ./recordings/demo-events.jsonl
# Optional accelerated rehearsal (displayed as REPLAY):
& ./viewer/build-verified/run_viewer.ps1 --replay ./recordings/demo-events.jsonl --speed 15
```

Replay validates the complete JSONL stream first, preserves message order and relative `t_sec` timing, supports snapshot/reset boundaries, and plays once. It never generates or overwrites a recording. At EOF the final scene remains visible with **REPLAY COMPLETE**. Invalid JSONL exits with the failing line number. Supply one complete envelope per line, without a TCP prefix.

Camera: drag to rotate/pan, wheel to zoom, **Home** to recenter on the platforms, **1** perspective, **2** overhead, **Esc** exit. Ships and USVs use the SDK surface model at the received zero altitude; UAVs use the SDK airplane model at received positive altitudes. The command ship is larger than the USVs. Labels retain platform identity and current state; UAV-02 also retains its latest mission-command result.

## Protocol and rendering

No `docs/protocol.md` exists in the backend branch. The integration follows `backend/tcp_server.py` and `backend/README.md`:

- Default endpoint `127.0.0.1:5556`; `--host` and `--port` override it.
- Exactly four unsigned big-endian length bytes, followed by 1–1,048,576 UTF-8 payload bytes. Fragmented and coalesced messages are read exactly; invalid lengths or invalid messages cause an explicit stale status and reconnect.
- v1 types: SNAPSHOT, DELTA, SECURITY_EVENT, CONTACT_EVENT, LINK_EVENT, HEARTBEAT. Required fields, numeric ranges, enums, UTF-8, duplicate JSON keys and bounded nesting are validated. Full JSON parsing uses vendored nlohmann/json 3.11.3 under its included MIT license.
- Only a fresh SNAPSHOT clears **STALE DATA** after connection/reconnection. The backend automatically sends it on connect. The feed is one-way; the viewer sends no unsupported request bytes. A five-second receive timeout detects silent peers.
- Sequence numbers must increase within a connection; a reconnect permits the fresh snapshot's sequence to restart. Repeated/out-of-order events are ignored. Snapshots replace platform/contact state, restore default UP connectivity, and subsequent LINK_EVENT messages restore any current outage. SDK identifiers stay stable for retained platform IDs.
- Parsed events cross a mutex-protected queue. All datastore, preferences, HUD and scene operations run on the viewer thread. Queue overflow is reported and forces resynchronization; security events are never silently dropped to make room for telemetry.
- WGS84 latitude/longitude degrees and ellipsoid altitude meters pass through `simCore::CoordinateConverter::convertGeodeticToEcef()`. Heading is converted with the orientation to earth-centric radians. The datastore receives ECEF meters, verified through actual `simVis::PlatformNode::update()` readback. `View::lookAt()` uses **latitude, longitude** order in this SDK.
- Each received position replaces the SDK static TSPI sample (`time=-1`). The scene holds the last received position during idle/disconnect; it does not extrapolate motion or security decisions. The HUD separately preserves backend `t_sec`, `seq`, and optional `run_id`.
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
