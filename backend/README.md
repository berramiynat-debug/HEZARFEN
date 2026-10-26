# HEZARFEN backend

Python 3.10+ standard library only. Run commands from the repository root. No installation, network connection, cloud service or external hardware is needed at runtime.

## Launch

```sh
python -m backend.main
```

Open http://127.0.0.1:8000 and press **Start Mission**. The TCP viewer connects to **127.0.0.1:5556**. On Windows, `py -3` may replace `python`. Stop with Ctrl-C. Never run two backends on the same ports.

Automatic 150-second presentation:

```sh
python -m backend.main --demo
```

Offline replay of the bundled actual event stream:

```sh
python -m backend.main --replay recordings/demo-events.jsonl
```

Tests and full real-time rehearsal:

```sh
python -m unittest discover -s backend/tests -v
python -m backend.tests.live_check
```

The rehearsal takes about 150 seconds and requires ports 5556 and 8000 to be free. The unit/integration suite uses ephemeral ports. The rehearsal's SIGTERM check targets POSIX; use Ctrl-C to check shutdown on Windows.

Generate a new deterministic capture without waiting in real time:

```sh
python -m backend.main --generate-demo --record recordings/new-demo.jsonl
```

Output paths must not already exist. The bundled recording is intentionally protected from accidental replacement. Every live run automatically creates a unique `recordings/session-*.jsonl` and matching `.audit.jsonl` file. Those transient session files are git-ignored. The explicit `--record` option is available for named captures. Do not point output at the replay source.

## 150-second presentation script

| Simulation time | Action | Observable result |
|---|---|---|
| 0:00 | Start Mission | Six platforms move. CMD-01 task assignment passes receiver authorization. |
| 0:30 | Unauthorized Command | UAV-02 route change to USV-01 is rejected; USV-01 heading and trajectory remain unchanged. UAV-02 becomes Watch. |
| 1:00 | Inject Unverified Contact | Contact appears as Unverified. UAV-02 report and two independent non-corroborating synthetic observations appear in panel evidence and audit file. |
| 1:30 | Repeat Policy Violation | A second distinct forbidden command triggers Restricted; a subsequent task request is rejected by the narrow restriction. Safety telemetry is accepted. |
| 1:50 | Link Outage | UAV-03 connectivity DOWN, with no trust downgrade or malicious finding. |
| 2:10 | Automatic Restore | UAV-03 link returns UP after 20 simulation seconds. |
| 2:10–2:30 | Mission Continuity | All six physical trajectories continue, contact stays Unverified, UAV-02 remains narrowly restricted. |
| 2:30 | End | Motion stops; HEARTBEAT reports running=false. Reset prepares a new interactive mission. |

`--demo` executes this schedule automatically. In manual mode the presenter presses scenario buttons at these times; the outage still automatically restores after 20 simulation seconds. **Repeat Policy Violation** injects two distinct forbidden attempts, so it also works when used without the earlier scenario. Manual actions during `--demo` are permitted but will alter that capture; use the automated run without extra clicks for a repeatable presentation.

## Local security policy

The configured identity-to-role table is authoritative inside this software model. Identity authentication, cryptographic signatures and real network participants are not implemented. Valid configured identity does not imply command authority.

* **RBAC-01:** Only COMMAND_AUTHORITY may issue ROUTE_CHANGE, TASK_ASSIGNMENT or RETURN_TO_BASE. All other roles may send SAFETY_TELEMETRY, CONTACT_REPORT and REQUEST_TASK. Unknown identities and unknown message kinds fail closed.
* Receiver `Simulator.receive()` calls `SecurityEngine.authorize()` before any route mutation. Denied messages cannot change a route. Accepted route changes and return commands update the simple heading model; task assignment is accepted and audited without modeling a full task planner.
* **LOCAL-01:** One forbidden-command attempt moves its sender to Watch. Contact reports and link loss never do this.
* **LOCAL-02:** Two distinct forbidden-command message IDs move the sender to Restricted. Retransmission of one ID is not a second attempt. This is a local demonstration policy, not proof of compromise, collusion or a cryptographic detection guarantee. It restricts nonessential MISSION_COMMAND traffic, including normally permitted REQUEST_TASK, at the receiver. Safety telemetry and contact reports still pass authorization. Identity and IDs are generated locally, so “independent” means separate simulated messages, not externally verified attribution.
* Restrictions persist until explicit operator Reset. Other platforms continue their missions.
* **Quarantine is disabled.** There is no state selector, action or automatic transition to it. No cryptographically verified equivocation proof is implemented or tested.
* Contact status and sender trust are separate. The included observations do not corroborate CONTACT-01, which stays Unverified. There is no automatic confirmation mechanism or sensor-fusion implementation; this deliberately cannot promote unsupported reports to Confirmed. The wire parser recognizes Confirmed for consumer interoperability, but the simulator never generates it.

## Viewer integration contract

No existing `docs/protocol.md` was present in the initially empty repository. This implementation follows the supplied v1 contract without extending its keys. No `viewer/` files are created or modified.

Each TCP frame: four-byte unsigned big-endian payload byte length, then UTF-8 JSON. Payload maximum 1 MiB. Envelope keys are exactly `v`, `type`, `seq`, `t_sec`, `data`; v=1. Types: SNAPSHOT, DELTA, SECURITY_EVENT, CONTACT_EVENT, LINK_EVENT, HEARTBEAT.

Platform fields are exactly `id`, `kind`, `role`, `lat_deg`, `lon_deg`, `alt_m`, `heading_deg`, `security_state`. Kinds: SHIP (CMD-01), USV, UAV. Security states: Trusted, Watch, Restricted. Coordinates are synthetic WGS84 latitude/longitude near the Sea of Marmara; sea-level assets use altitude 0, UAVs positive altitude. The model does not perform coast/bathymetry collision checking.

SNAPSHOT contains all six platforms and current contacts. DELTA carries changed full platform objects at 5 Hz while running. SECURITY_EVENT carries the exact requested decision fields. The audit sidecar adds message ID, sender, receiver, policy rule, command and decision without modifying the TCP schema. CONTACT_EVENT carries the requested contact fields only; evidence details are in the HTTP panel and sidecar. LINK_EVENT status is UP or DOWN. HEARTBEAT has only `running`.

All publication occurs on one asyncio event loop. Each connection/reconnection triggers a fresh SNAPSHOT, recorded and broadcast to all clients with a new global sequence number. A current DOWN link is then re-emitted because the fixed snapshot schema has no link field. Viewers should initialize link state to UP on SNAPSHOT, then apply subsequent LINK_EVENT messages. They must replace their platform/contact state on SNAPSHOT, and order by `seq` rather than event IDs. `t_sec` can return to zero on Reset or Replay; sequence numbers never decrease within one backend process.

The TCP channel is an **outbound monitoring feed**, not a platform command input API. Unexpected inbound bytes close that connection. Commands in this demonstration are internal messages dispatched by scenario actions to the receiving application. Simulated UAV-03 link loss affects its logical connectivity indicator; the observer feed still shows ground-truth motion. This is not an emulated radio outage of the monitoring TCP connection.

Reconnects need not replay old decisions: current trust/contact states are in the fresh snapshot, while historical decisions remain in recordings. A maximum of 16 viewer clients is supported. Each client has a 128-frame bounded queue and a two-second drain timeout; lagging clients disconnect and must reconnect for current state. HTTP is loopback-only, capped at 32 active requests, with bounded headers and a two-second read timeout; POST controls check Host and Origin.

## Recording and replay

The JSONL file records every published envelope in order, including connection snapshots and security decisions. Each line contains the JSON payload, without the TCP length prefix. A consumer can frame each line using its UTF-8 byte count. The supplied `demo-events.jsonl` is produced by running exactly 750 simulation ticks through the same engine; it is not a handwritten scenario fixture. Its companion audit contains receiver decisions and observation evidence.

Replay consumes recordings incrementally, emits the original types/data/timestamps in order and regenerates monotonically increasing delivery sequence numbers. This permits new connection snapshots without sequence collisions. Hence replay is payload/timing deterministic, not byte-identical sequence numbering across different connection histories. Relative timing is preserved across Reset boundaries. The initial idle snapshot and connection checkpoints are additional transport events. During replay, live scenario actions are disabled; Reset returns to live mode. The Replay button selects the bundled recording (or the explicit CLI replay source), not the most recent mutable capture.

The panel shows contact status during replay and points to the companion audit for sensor evidence. Receiver audit history is not embedded into the wire stream; replay does not re-execute policy decisions or claim to reauthenticate messages. Replay files are local operator inputs and are schema/order-validated before playback.

## Limits and handoff

Implemented: deterministic motion, receiver authorization, scoped restriction, unsupported contacts, recoverable logical outage, independent mission continuity, local HTTP controls, recorded ordered TCP feed and offline replay.

Not implemented: real military radios, physical sensors, ns-3, radio propagation, actual cryptographic identity verification, equivocation proof, quarantine, sensor-fusion confirmation, collision avoidance, a full mission planner, or SIMDIS rendering. The 5 Hz feed uses wall-clock pacing with deterministic fixed steps; under host overload it slows rather than skipping simulation steps. Record writes are flushed per event, but are not fsync-level crash durability guarantees. Restart after disk exhaustion or malformed replay errors; such errors must not be hidden during rehearsal.

The Viewer/SIMDIS developer must still connect their actual viewer and verify presentation rendering. There was no viewer in this repository to test. This backend is a synthetic demonstrator, not an operational security system.
