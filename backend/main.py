"""Run with python -m backend.main from the repository root (Python 3.10+)."""
import argparse
import asyncio
from collections import deque
import copy
import json
from pathlib import Path
import signal
from .control_panel import ControlPanel
from .scenarios import TIMELINE, perform
from .simulator import Simulator
from .tcp_server import TCPServer, MAX_PAYLOAD, decode

ROOT = Path(__file__).resolve().parent.parent
DEMO = ROOT / 'recordings' / 'demo-events.jsonl'


def read_recording(path):
    """Streaming reader, bounded line size; rejects corrupt or unordered recordings."""
    previous = 0
    with open(path, 'rb') as stream:
        while True:
            line = stream.readline(MAX_PAYLOAD + 2)
            if not line:
                break
            event = decode(line.rstrip(b'\n'))
            if event['seq'] <= previous:
                raise ValueError('Recording sequence must increase')
            if previous == 0 and event['type'] != 'SNAPSHOT':
                raise ValueError('Recording must begin with SNAPSHOT')
            previous = event['seq']
            yield event
    if previous == 0:
        raise ValueError('Empty recording')


class Runtime:
    def __init__(self, record, seed=42, scripted=False, replay_source=DEMO):
        self.record_path = Path(record).resolve()
        self.replay_source = Path(replay_source).resolve()
        if self.record_path == self.replay_source:
            raise ValueError('Recording output must differ from replay source')
        self.record_path.parent.mkdir(parents=True, exist_ok=True)
        # Never overwrite an earlier live capture accidentally.
        self.record = self.record_path.open('x', encoding='utf-8')
        try:
            self.audit_file = self.record_path.with_suffix('.audit.jsonl').open('x', encoding='utf-8')
        except BaseException:
            self.record.close()
            raise
        self.seed = seed
        self.sim = Simulator(seed)
        self.seq = 0
        self.time = 0.0
        self.mode = 'LIVE'
        self.scripted = scripted
        self.log = deque(maxlen=300)
        self.tcp = TCPServer(self.checkpoint)
        self.panel = ControlPanel(self)
        self.replay_iter = None
        self.next_event = None
        self.replay_clock = 0.0
        self.replay_due_time = 0.0
        self.checkpoint()
        if scripted:
            self.action('start')

    def emit(self, kind, data, t_sec=None):
        self.seq += 1
        event = dict(v=1, type=kind, seq=self.seq,
                     t_sec=self.time if t_sec is None else t_sec, data=copy.deepcopy(data))
        self.record.write(json.dumps(event, separators=(',', ':'), allow_nan=False) + '\n')
        self.record.flush()
        self.tcp.publish(event)
        if kind not in {'DELTA', 'HEARTBEAT'}:
            self.log.append(event)
        return event

    def checkpoint(self):
        # A new global sequence, broadcast and recorded, so no duplicate/stale cursor.
        self.emit('SNAPSHOT', self.sim.snapshot())
        # Snapshot schema cannot carry links: send current DOWN state immediately after it.
        for key, state in self.sim.links.items():
            if state == 'DOWN':
                self.emit('LINK_EVENT', dict(subject=key, status=state, reason='SYNTHETIC_LINK_OUTAGE'))

    def action(self, action):
        if action == 'reset':
            self.close_replay()
            self.sim = Simulator(self.seed)
            self.time = 0.0
            self.mode = 'LIVE'
            self.scripted = False
            self.checkpoint()
            self.emit('LINK_EVENT', dict(subject='UAV-03', status='UP', reason='SYNTHETIC_LINK_RESTORED'))
            return
        if action == 'replay':
            # Validate fully before changing live state; iteration is bounded in memory.
            for _ in read_recording(self.replay_source):
                pass
            self.close_replay()
            self.sim = Simulator(self.seed)
            self.mode = 'REPLAY'
            self.replay_clock = 0.0
            self.replay_iter = iter(read_recording(self.replay_source))
            self.next_event = next(self.replay_iter)
            self.replay_due_time = 0.0
            self.replay_due()
            return
        if self.mode != 'LIVE':
            raise ValueError('Reset before issuing live actions during replay')
        if action != 'start' and not self.sim.running:
            raise ValueError('Start Mission first')
        events, audits = perform(self.sim, action)
        for audit in audits:
            row = dict(t_sec=self.sim.t_sec, **audit)
            self.audit_file.write(json.dumps(row, separators=(',', ':')) + '\n')
            self.log.append(row)
        for kind, data in events:
            if kind == 'CONTACT_EVENT':
                row = dict(t_sec=self.time, contact_id=data['id'], evidence=self.sim.evidence[data['id']])
                self.audit_file.write(json.dumps(row, separators=(',', ':')) + '\n')
        self.audit_file.flush()
        for kind, data in events:
            self.emit(kind, data)

    def replay_due(self):
        while self.next_event and self.replay_due_time <= self.replay_clock + 1e-8:
            event = self.next_event
            self.time = event['t_sec']
            kind, data = event['type'], event['data']
            if kind == 'SNAPSHOT':
                self.sim.platforms = {p['id']: copy.deepcopy(p) for p in data['platforms']}
                self.sim.contacts = {c['id']: copy.deepcopy(c) for c in data['contacts']}
                self.sim.links = dict.fromkeys(self.sim.platforms, 'UP')
            elif kind == 'DELTA':
                self.sim.platforms.update({p['id']: copy.deepcopy(p) for p in data['platforms']})
            elif kind == 'CONTACT_EVENT':
                self.sim.contacts[data['id']] = copy.deepcopy(data)
                self.sim.evidence[data['id']] = [{'finding': 'See companion .audit.jsonl for recorded sensor observations; wire event preserves status only'}]
            elif kind == 'SECURITY_EVENT':
                if data['subject'] in self.sim.platforms:
                    self.sim.platforms[data['subject']]['security_state'] = data['security_state']
            elif kind == 'LINK_EVENT':
                self.sim.links[data['subject']] = data['status']
            elif kind == 'HEARTBEAT':
                self.sim.running = data['running']
            self.emit(kind, data, event['t_sec'])
            self.next_event = next(self.replay_iter, None)
            if self.next_event:
                self.replay_due_time += max(0, self.next_event['t_sec'] - event['t_sec'])
        if self.next_event is None:
            self.mode = 'REPLAY_COMPLETE'
            self.sim.running = False
            self.close_replay()

    def tick(self):
        if self.mode == 'REPLAY':
            self.replay_clock = round(self.replay_clock + .2, 1)
            self.replay_due()
        elif self.mode == 'LIVE':
            events = self.sim.step()
            self.time = self.sim.t_sec
            for kind, data in events:
                self.emit(kind, data)
            if self.scripted and self.sim.running and self.sim.tick in TIMELINE:
                self.action(TIMELINE[self.sim.tick])
            if self.sim.tick % 5 == 0:
                self.emit('HEARTBEAT', {'running': self.sim.running})

    def status(self):
        return dict(mode=self.mode, t_sec=self.time, running=self.sim.running,
                    **self.sim.snapshot(), links=self.sim.links, evidence=self.sim.evidence,
                    log=list(self.log), quarantine_enabled=False)

    def close_replay(self):
        if self.replay_iter:
            self.replay_iter.close()
        self.replay_iter = None
        self.next_event = None

    def close_files(self):
        self.close_replay()
        self.record.close()
        self.audit_file.close()


async def serve(args):
    runtime = Runtime(args.record, args.seed, args.demo, args.replay or DEMO)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except (NotImplementedError, RuntimeError):
            pass  # asyncio.run still handles Ctrl-C on Windows.
    try:
        await runtime.tcp.start(args.tcp_port)
        await runtime.panel.start(args.http_port)
        if args.replay:
            runtime.action('replay')
        print(f'HEZARFEN ready: TCP 127.0.0.1:{args.tcp_port}; panel http://127.0.0.1:{args.http_port}', flush=True)
        deadline = loop.time() + .2
        while not stop.is_set():
            try:
                await asyncio.wait_for(stop.wait(), max(0, deadline - loop.time()))
            except asyncio.TimeoutError:
                runtime.tick()
                deadline = max(deadline + .2, loop.time())
    finally:
        await runtime.panel.close()
        await runtime.tcp.close()
        runtime.close_files()


def main():
    parser = argparse.ArgumentParser(description='HEZARFEN deterministic software demonstrator')
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--tcp-port', type=int, default=5556)
    parser.add_argument('--http-port', type=int, default=8000)
    parser.add_argument('--record', type=Path, default=None)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--demo', action='store_true', help='Run the 150-second scripted presentation')
    group.add_argument('--replay', type=Path, help='Replay an offline JSONL recording')
    group.add_argument('--generate-demo', action='store_true', help='Generate a deterministic 150-second recording without wall-clock delay')
    args = parser.parse_args()
    if args.generate_demo:
        runtime = Runtime(args.record or DEMO, args.seed, True, ROOT / 'recordings' / 'unused-source.jsonl')
        try:
            for _ in range(750):
                runtime.tick()
        finally:
            runtime.close_files()
        print('Generated 150 seconds of HEZARFEN events')
        return
    if args.record is None:
        from datetime import datetime
        args.record = ROOT / 'recordings' / ('session-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f') + '.jsonl')
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
