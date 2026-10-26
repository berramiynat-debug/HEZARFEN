import asyncio
import copy
from pathlib import Path
import tempfile
import unittest

from backend.main import DEMO
from backend.simulator import Simulator
from backend.tcp_server import read_frame
from viewer.presentation_backend import PresentationRuntime


class PauseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.runtime = PresentationRuntime(Path(self.temp.name) / 'session.jsonl', scripted=True)

    def tearDown(self):
        self.runtime.close_files()
        self.temp.cleanup()

    def test_pause_at_script_boundary_preserves_motion_trust_and_decision_count(self):
        r = self.runtime
        for boundary, expected_state in ((150, 'Watch'), (450, 'Restricted')):
            with self.subTest(boundary=boundary):
                while r.sim.tick < boundary:
                    r.tick()
                self.assertEqual(r.sim.security.states['UAV-02'], expected_state)
                r.action('pause')
                snapshot = r.sim.snapshot()
                decisions = r.sim.security.counter
                for _ in range(100):
                    r.tick()
                self.assertEqual(r.sim.snapshot(), snapshot)
                self.assertEqual(r.sim.tick, boundary)
                self.assertEqual(r.time, boundary / 5)
                self.assertEqual(r.sim.security.counter, decisions)
                self.assertTrue(r.status()['paused'])
                self.assertFalse(r.status()['running'])
                r.action('resume')
                r.tick()
                self.assertEqual(r.sim.tick, boundary + 1)
                self.assertEqual(r.sim.security.counter, decisions)
                self.assertFalse(r.status()['paused'])
                self.assertTrue(r.status()['running'])

    def test_outage_restore_waits_for_simulation_time(self):
        r = self.runtime
        for _ in range(550):
            r.tick()
        self.assertEqual(r.sim.links['UAV-03'], 'DOWN')
        r.action('pause')
        for _ in range(200):
            r.tick()
        self.assertEqual(r.sim.links['UAV-03'], 'DOWN')
        self.assertEqual(r.time, 110)
        r.action('resume')
        for _ in range(99):
            r.tick()
        self.assertEqual(r.sim.links['UAV-03'], 'DOWN')
        r.tick()
        self.assertEqual(r.sim.links['UAV-03'], 'UP')
        self.assertEqual(r.sim.security.states['UAV-03'], 'Trusted')

    def test_paused_scenario_actions_rejected_and_reset_clears_pause(self):
        r = self.runtime
        r.action('pause')
        before = copy.deepcopy(r.sim.snapshot())
        for action in ('start', 'unauthorized', 'contact', 'repeat', 'link_down'):
            with self.assertRaises(ValueError):
                r.action(action)
        self.assertEqual(r.sim.snapshot(), before)
        r.action('reset')
        self.assertFalse(r.paused)
        self.assertFalse(r.sim.running)
        self.assertEqual(r.time, 0)
        with self.assertRaises(ValueError):
            r.action('resume')

    def test_replay_clock_and_cursor_freeze_without_losing_events(self):
        r = self.runtime
        r.action('replay')
        for _ in range(10):
            r.tick()
        r.action('pause')
        clock, cursor, snapshot = r.replay_clock, r.next_event, r.sim.snapshot()
        for _ in range(100):
            r.tick()
        self.assertEqual(r.replay_clock, clock)
        self.assertEqual(r.next_event, cursor)
        self.assertEqual(r.sim.snapshot(), snapshot)
        r.action('resume')
        for _ in range(740):
            r.tick()
        self.assertEqual(r.mode, 'REPLAY_COMPLETE')
        self.assertEqual(r.time, 150)
        self.assertEqual(r.sim.platforms['UAV-02']['security_state'], 'Restricted')

    def test_completed_demo_cannot_be_resumed_or_paused(self):
        r = self.runtime
        for _ in range(750):
            r.tick()
        for action in ('pause', 'resume'):
            with self.assertRaises(ValueError):
                r.action(action)
        self.assertFalse(r.sim.running)
        self.assertEqual(r.time, 150)

    def test_restart_completed_demo_repeats_full_automatic_schedule(self):
        r = self.runtime
        for _ in range(750):
            r.tick()
        old_seq = r.seq
        self.assertFalse(r.sim.running)
        r.action('restart')
        self.assertTrue(r.scripted)
        self.assertTrue(r.sim.running)
        self.assertFalse(r.paused)
        self.assertEqual(r.time, 0)
        self.assertEqual(r.sim.tick, 0)
        self.assertEqual(r.sim.snapshot(), Simulator(r.seed).snapshot())
        self.assertGreater(r.seq, old_seq)
        for tick in range(1, 751):
            r.tick()
            if tick == 150:
                self.assertEqual(r.sim.security.states['UAV-02'], 'Watch')
            if tick == 300:
                self.assertEqual(r.sim.contacts['CONTACT-01']['status'], 'Unverified')
            if tick == 450:
                self.assertEqual(r.sim.security.states['UAV-02'], 'Restricted')
            if tick == 550:
                self.assertEqual(r.sim.links['UAV-03'], 'DOWN')
            if tick == 650:
                self.assertEqual(r.sim.links['UAV-03'], 'UP')
        self.assertEqual(r.time, 150)
        self.assertFalse(r.sim.running)
        self.assertEqual(r.sim.security.counter, 8)

    def test_restart_paused_outage_clears_restriction_contacts_and_links(self):
        r = self.runtime
        for _ in range(550):
            r.tick()
        r.action('pause')
        self.assertEqual(r.sim.links['UAV-03'], 'DOWN')
        self.assertEqual(r.sim.security.states['UAV-02'], 'Restricted')
        self.assertTrue(r.sim.contacts)
        r.action('restart')
        self.assertFalse(r.paused)
        self.assertTrue(r.sim.running)
        self.assertEqual(set(r.sim.links.values()), {'UP'})
        self.assertEqual(set(r.sim.security.states.values()), {'Trusted'})
        self.assertFalse(r.sim.contacts)
        self.assertFalse(r.sim.evidence)
        self.assertEqual(r.time, 0)
        self.assertTrue(r.status()['restart_supported'])


class HttpPauseTests(unittest.IsolatedAsyncioTestCase):
    async def test_actual_http_controls_and_paused_reconnect_keep_v1_schema(self):
        with tempfile.TemporaryDirectory() as temp:
            r = PresentationRuntime(Path(temp) / 'http.jsonl', scripted=True)
            http = await r.panel.start(0)
            tcp = await r.tcp.start(0)
            writer = None
            async def post(action):
                reader, w = await asyncio.open_connection('127.0.0.1', http)
                w.write(f'POST /action/{action} HTTP/1.1\r\nHost: 127.0.0.1:{http}\r\nContent-Length: 0\r\n\r\n'.encode())
                await w.drain()
                response = await reader.read()
                w.close()
                await w.wait_closed()
                self.assertIn(b'200 Response', response)
            try:
                await post('pause')
                reader, writer = await asyncio.open_connection('127.0.0.1', tcp)
                snapshot = await asyncio.wait_for(read_frame(reader), 2)
                heartbeat = await asyncio.wait_for(read_frame(reader), 2)
                self.assertEqual(snapshot['type'], 'SNAPSHOT')
                self.assertEqual(heartbeat['data'], {'running': False})
                self.assertEqual(set(heartbeat), {'v', 'type', 'seq', 't_sec', 'data'})
                self.assertTrue(r.status()['pause_supported'])
                await post('resume')
                resumed = await asyncio.wait_for(read_frame(reader), 2)
                self.assertEqual(resumed['data'], {'running': True})
                await post('restart')
                restarted = await asyncio.wait_for(read_frame(reader), 2)
                self.assertEqual(restarted['type'], 'SNAPSHOT')
                self.assertEqual(restarted['t_sec'], 0)
                self.assertGreater(restarted['seq'], resumed['seq'])
                self.assertEqual(len(restarted['data']['platforms']), 6)
                self.assertTrue(r.scripted and r.sim.running)

            finally:
                if writer:
                    writer.close()
                    await writer.wait_closed()
                await r.panel.close()
                await r.tcp.close()
                r.close_files()


if __name__ == '__main__':
    unittest.main()
