"""Pause-aware presentation runner; reuses the unmodified backend and wire schema.

Run from the repository root: python -m viewer.presentation_backend --demo
"""
import argparse
import asyncio
from datetime import datetime
from pathlib import Path
import signal

from backend.main import DEMO, Runtime
from backend.simulator import Simulator


class PresentationRuntime(Runtime):
    def __init__(self, *args, **kwargs):
        self.paused = False
        super().__init__(*args, **kwargs)

    def checkpoint(self):
        super().checkpoint()
        self.emit('HEARTBEAT', {'running': self.sim.running and not self.paused})

    def action(self, action):
        if action == 'restart':
            self.close_replay()
            self.sim = Simulator(self.seed)
            self.time = 0.0
            self.mode = 'LIVE'
            self.scripted = True
            self.paused = False
            self.log.clear()
            self.checkpoint()
            super().action('start')
            self.emit('HEARTBEAT', {'running': True})
            return
        if action == 'pause':
            if self.mode not in {'LIVE', 'REPLAY'} or not (self.sim.running or self.mode == 'REPLAY'):
                raise ValueError('No active mission to pause')
            self.paused = True
            self.emit('HEARTBEAT', {'running': False})
            return
        if action == 'resume':
            if not self.paused:
                raise ValueError('Mission is not paused')
            self.paused = False
            self.emit('HEARTBEAT', {'running': self.sim.running})
            return
        if self.paused and action not in {'reset', 'replay'}:
            raise ValueError('Resume the paused mission before scenario actions')
        if action in {'reset', 'replay'}:
            # Preserve pause if validation of a requested replay fails.
            super().action(action)
            self.paused = False
            self.emit('HEARTBEAT', {'running': self.sim.running})
            return
        super().action(action)

    def tick(self):
        if self.paused:
            # Keep the observer connection fresh while BOTH clocks stay frozen.
            # Returning here also prevents repeated triggers at tick 150/450/550.
            self.emit('HEARTBEAT', {'running': False})
            return
        super().tick()

    def status(self):
        state = super().status()
        state.update(paused=self.paused, pause_supported=True, restart_supported=True,
                     running=self.sim.running and not self.paused)
        return state


async def serve(args):
    runtime = PresentationRuntime(args.record, args.seed, args.demo, args.replay or DEMO)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except (NotImplementedError, RuntimeError):
            pass
    try:
        tcp_port = await runtime.tcp.start(args.tcp_port)
        http_port = await runtime.panel.start(args.http_port)
        if args.replay:
            runtime.action('replay')
        print(f'HEZARFEN ready: TCP 127.0.0.1:{tcp_port}; panel http://127.0.0.1:{http_port}; pause/resume enabled', flush=True)
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
    parser = argparse.ArgumentParser(description='HEZARFEN pause-aware presentation runner')
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--tcp-port', type=int, default=5556)
    parser.add_argument('--http-port', type=int, default=8000)
    parser.add_argument('--record', type=Path)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--demo', action='store_true')
    group.add_argument('--replay', type=Path)
    args = parser.parse_args()
    if args.record is None:
        output = Path(__file__).resolve().parent / 'build-verified'
        output.mkdir(exist_ok=True)
        args.record = output / ('session-presentation-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f') + '.jsonl')
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
