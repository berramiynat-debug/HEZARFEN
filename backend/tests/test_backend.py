import asyncio
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest

from backend.main import Runtime, read_recording
from backend.scenarios import perform
from backend.security_engine import SecurityEngine, ROLES, COMMANDS
from backend.simulator import Simulator
from backend.tcp_server import encode, read_frame, decode, MAX_PAYLOAD


class PolicyTests(unittest.TestCase):
    def test_six_platforms_and_wgs84(self):
        sim = Simulator()
        self.assertEqual(set(sim.platforms), {'CMD-01', 'USV-01', 'USV-02', 'UAV-01', 'UAV-02', 'UAV-03'})
        for p in sim.snapshot()['platforms']:
            self.assertEqual(set(p), {'id','kind','role','lat_deg','lon_deg','alt_m','heading_deg','security_state'})
            self.assertTrue(-90 <= p['lat_deg'] <= 90)
            self.assertTrue(-180 <= p['lon_deg'] <= 180)
            self.assertEqual(p['role'], ROLES[p['id']])
            self.assertEqual(p['alt_m'] > 0, p['kind'] == 'UAV')
            if p['kind'] != 'UAV':
                self.assertEqual(p['alt_m'], 0)

    def test_all_role_command_pairs(self):
        for sender in ROLES:
            for command in COMMANDS:
                engine = SecurityEngine()
                allowed, event, audit = engine.authorize('M1', sender, 'USV-01', command)
                self.assertEqual(allowed, sender == 'CMD-01')
                self.assertEqual(audit['receiver'], 'USV-01')
                self.assertEqual(audit['policy_rule'], 'RBAC-01')
                self.assertEqual(event['evidence_ids'], ['M1'])

    def test_denied_command_preserves_entire_route_and_trajectory(self):
        sim, baseline = Simulator(), Simulator()
        sim.running = baseline.running = True
        original = copy.deepcopy(sim.routes)
        perform(sim, 'unauthorized')
        self.assertEqual(sim.routes, original)
        for _ in range(100):
            sim.step(); baseline.step()
        for key in sim.platforms:
            for field in ('lat_deg', 'lon_deg', 'heading_deg', 'alt_m'):
                self.assertEqual(sim.platforms[key][field], baseline.platforms[key][field])

    def test_authorized_command_actually_changes_route(self):
        sim = Simulator()
        sim.receive('CMD-01', 'USV-01', 'ROUTE_CHANGE', heading=270)
        self.assertEqual(sim.routes['USV-01'], 270)

    def test_distinct_attempts_scoped_restriction_and_safety(self):
        sim = Simulator()
        sim.receive('UAV-02', 'USV-01', 'ROUTE_CHANGE', message_id='M1')
        sim.receive('UAV-02', 'USV-01', 'ROUTE_CHANGE', message_id='M1')
        self.assertEqual(sim.security.states['UAV-02'], 'Watch')
        sim.receive('UAV-02', 'USV-01', 'TASK_ASSIGNMENT', message_id='M2')
        self.assertEqual(sim.security.states['UAV-02'], 'Restricted')
        event, _ = sim.receive('UAV-02', 'CMD-01', 'REQUEST_TASK')
        self.assertEqual(event['result'], 'REJECTED')
        self.assertEqual(event['reason'], 'SCOPED_SENDER_RESTRICTION')
        for command in ('SAFETY_TELEMETRY', 'CONTACT_REPORT'):
            event, _ = sim.receive('UAV-02', 'CMD-01', command)
            self.assertEqual(event['result'], 'ACCEPTED')

    def test_repeat_button_works_without_prior_violation(self):
        sim = Simulator()
        perform(sim, 'repeat')
        self.assertEqual(sim.security.states['UAV-02'], 'Restricted')

    def test_contact_never_confirms_or_restricts(self):
        sim = Simulator()
        perform(sim, 'contact')
        sim.running = True
        for _ in range(750):
            sim.step()
        self.assertEqual(sim.contacts['CONTACT-01']['status'], 'Unverified')
        self.assertEqual(sim.security.states['UAV-02'], 'Trusted')
        self.assertEqual([e['finding'] for e in sim.evidence['CONTACT-01']],
                         ['REPORTED', 'NOT_CORROBORATED', 'NOT_CORROBORATED'])

    def test_link_loss_recovers_without_security_finding(self):
        sim = Simulator()
        sim.running = True
        original = copy.deepcopy(sim.security.states)
        sim.link(True)
        for _ in range(100):
            sim.step()
        self.assertEqual(sim.links['UAV-03'], 'UP')
        self.assertEqual(sim.security.states, original)
        self.assertEqual(len(sim.security.audit), 0)

    def test_unknown_identity_and_command_fail_closed(self):
        engine = SecurityEngine()
        for sender, receiver, command in [('UNKNOWN', 'USV-01','ROUTE_CHANGE'),
                                          ('CMD-01','UNKNOWN','ROUTE_CHANGE'),
                                          ('CMD-01','USV-01','UNKNOWN')]:
            self.assertFalse(engine.authorize('M1', sender, receiver, command)[0])


class FramingTests(unittest.IsolatedAsyncioTestCase):
    async def test_fragmented_and_coalesced_frames(self):
        event = dict(v=1,type='HEARTBEAT',seq=1,t_sec=0.0,data={'running':True})
        frame = encode(event)
        reader = asyncio.StreamReader()
        task = asyncio.create_task(read_frame(reader))
        for byte in frame:
            reader.feed_data(bytes([byte]))
            await asyncio.sleep(0)
        self.assertEqual(await task, event)
        reader.feed_data(frame + frame)
        self.assertEqual(await read_frame(reader), event)
        self.assertEqual(await read_frame(reader), event)

    async def test_invalid_platform_data_is_rejected(self):
        for field, value in [('lat_deg', 91), ('alt_m', 100), ('id', 'USV-99'), ('security_state', 'INVALID')]:
            event = dict(v=1,type='SNAPSHOT',seq=1,t_sec=0,data=Simulator().snapshot())
            event['data']['platforms'][0][field] = value
            with self.assertRaises(ValueError):
                encode(event)

    async def test_reject_oversized_zero_truncated_and_invalid_json(self):
        for length in (0, MAX_PAYLOAD + 1):
            reader = asyncio.StreamReader()
            reader.feed_data(struct.pack('!I', length))
            with self.assertRaises(ValueError):
                await read_frame(reader)
        reader = asyncio.StreamReader()
        reader.feed_data(struct.pack('!I', 20) + b'{}'); reader.feed_eof()
        with self.assertRaises(asyncio.IncompleteReadError):
            await read_frame(reader)
        for payload in (b'bad', b'\xff', b'{}', b'{"v":NaN}'):
            with self.assertRaises((ValueError, UnicodeError)):
                decode(payload)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
    def tearDown(self):
        self.temp.cleanup()
    def generate(self, name):
        runtime = Runtime(self.root/name, scripted=True)
        for _ in range(750):
            runtime.tick()
        runtime.close_files()
        return self.root/name

    def test_seed_determinism_and_150_second_recording(self):
        a, b = self.generate('a.jsonl'), self.generate('b.jsonl')
        self.assertEqual(a.read_bytes(), b.read_bytes())
        events = list(read_recording(a))
        self.assertEqual(events[-1]['t_sec'], 150)
        self.assertEqual(sum(e['type']=='DELTA' for e in events), 750)
        self.assertTrue(all(e['seq'] == i+1 for i,e in enumerate(events)))
        self.assertNotEqual(Simulator(42).snapshot(), Simulator(43).snapshot())

    def test_replay_preserves_order_payload_and_timestamps(self):
        source = self.generate('source.jsonl')
        runtime = Runtime(self.root/'replay.jsonl', replay_source=source)
        runtime.action('replay')
        for _ in range(750):
            runtime.tick()
        self.assertEqual(runtime.mode, 'REPLAY_COMPLETE')
        self.assertEqual(runtime.sim.platforms['UAV-02']['security_state'], 'Restricted')
        runtime.close_files()
        expected = list(read_recording(source))
        actual = list(read_recording(self.root/'replay.jsonl'))[1:]
        for event in actual:
            event['seq'] -= 1
        self.assertEqual(actual, expected)

    def test_reset_and_recording_overwrite_protection(self):
        runtime = Runtime(self.root/'run.jsonl')
        runtime.action('start'); runtime.action('repeat'); runtime.action('link_down')
        seq = runtime.seq
        runtime.action('reset')
        self.assertGreater(runtime.seq, seq)
        self.assertEqual(set(runtime.sim.security.states.values()), {'Trusted'})
        self.assertEqual(runtime.sim.links['UAV-03'], 'UP')
        self.assertFalse(runtime.sim.running)
        with self.assertRaises(ValueError):
            runtime.action('quarantine')
        runtime.close_files()
        with self.assertRaises(FileExistsError):
            Runtime(self.root/'run.jsonl')

    def test_bundled_capture_matches_current_generator(self):
        generated = self.generate('generated.jsonl')
        bundled = Path(__file__).resolve().parents[2]/'recordings'/'demo-events.jsonl'
        self.assertEqual(generated.read_bytes(), bundled.read_bytes())
        self.assertEqual(generated.with_suffix('.audit.jsonl').read_bytes(),
                         bundled.with_suffix('.audit.jsonl').read_bytes())

    def test_replay_across_reset_preserves_relative_timing(self):
        source = self.root/'reset-source.jsonl'
        runtime = Runtime(source)
        runtime.action('start')
        for _ in range(10): runtime.tick()
        runtime.action('reset'); runtime.action('start')
        for _ in range(10): runtime.tick()
        runtime.close_files()
        replay = Runtime(self.root/'reset-replay.jsonl', replay_source=source)
        replay.action('replay')
        for _ in range(10): replay.tick()
        self.assertEqual(replay.mode, 'REPLAY')
        for _ in range(10): replay.tick()
        self.assertEqual(replay.mode, 'REPLAY_COMPLETE')
        replay.close_files()

    def test_unordered_recording_rejected(self):
        path = self.generate('bad.jsonl')
        with path.open('a') as f:
            f.write(path.read_text().splitlines()[0]+'\n')
        with self.assertRaises(ValueError):
            list(read_recording(path))


class IntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.runtime = Runtime(Path(self.temp.name)/'live.jsonl')
        self.tcp_port = await self.runtime.tcp.start(0)
        self.http_port = await self.runtime.panel.start(0)
        self.writers = []
    async def asyncTearDown(self):
        for writer in self.writers:
            writer.close()
            await writer.wait_closed()
        await self.runtime.panel.close()
        await self.runtime.tcp.close()
        self.runtime.close_files()
        self.temp.cleanup()
    async def connect(self):
        r,w = await asyncio.open_connection('127.0.0.1', self.tcp_port)
        self.writers.append(w)
        return r,w
    async def http(self, method, path, origin=''):
        r,w = await asyncio.open_connection('127.0.0.1', self.http_port)
        w.write(f'{method} {path} HTTP/1.1\r\nHost: 127.0.0.1:{self.http_port}\r\nOrigin: {origin or "http://127.0.0.1:"+str(self.http_port)}\r\nContent-Length: 0\r\n\r\n'.encode())
        await w.drain()
        result = await asyncio.wait_for(r.read(), 2)
        w.close(); await w.wait_closed()
        return result

    async def test_reconnect_fresh_snapshot_and_down_link(self):
        r,w = await self.connect()
        first = await asyncio.wait_for(read_frame(r),2)
        self.assertEqual(first['type'],'SNAPSHOT')
        w.close(); await w.wait_closed()
        self.runtime.action('start'); self.runtime.action('repeat'); self.runtime.action('link_down')
        for _ in range(10):
            self.runtime.tick()
        r,_ = await self.connect()
        event = await asyncio.wait_for(read_frame(r),2)
        self.assertEqual(event['type'],'SNAPSHOT')
        self.assertEqual(len(event['data']['platforms']),6)
        self.assertGreater(event['seq'],first['seq'])
        subject = next(p for p in event['data']['platforms'] if p['id']=='UAV-02')
        self.assertEqual(subject['security_state'],'Restricted')
        self.assertEqual((await read_frame(r))['data']['status'],'DOWN')

    async def test_http_buttons_execute_backend_and_origin_is_checked(self):
        self.assertIn(b'HEZARFEN | Mission Security', await self.http('GET','/'))
        for action in ('start','unauthorized','contact','repeat','link_down','link_up'):
            self.assertIn(b'200 Response', await self.http('POST','/action/'+action))
        self.assertEqual(self.runtime.sim.security.states['UAV-02'],'Restricted')
        self.assertEqual(len(self.runtime.sim.contacts),1)
        response = await self.http('POST','/action/reset','http://other.example')
        self.assertIn(b'400 Response',response)
        self.assertEqual(self.runtime.sim.security.states['UAV-02'],'Restricted')
        self.assertIn(b'200 Response', await self.http('POST','/action/reset'))
        self.assertFalse(self.runtime.sim.running)

    async def test_shutdown_closes_pending_http_and_tcp_connections(self):
        r,w = await asyncio.open_connection('127.0.0.1', self.http_port)
        self.writers.append(w)
        w.write(b'GET / HTTP/1.1\r\n')
        await w.drain()
        await asyncio.sleep(.01)
        await self.runtime.panel.close()
        self.assertEqual(await asyncio.wait_for(r.read(),1), b'')
        r,_ = await self.connect()
        await read_frame(r)
        await self.runtime.tcp.close()
        self.assertEqual(await asyncio.wait_for(r.read(),1), b'')

    async def test_slow_client_queue_is_bounded(self):
        class FakeWriter:
            closed = False
            def close(self): self.closed = True
        writer = FakeWriter()
        queue = asyncio.Queue(maxsize=1)
        self.runtime.tcp.clients[writer] = queue
        self.runtime.checkpoint(); self.runtime.checkpoint()
        self.assertTrue(writer.closed)
        self.assertNotIn(writer,self.runtime.tcp.clients)
        self.assertEqual(queue.qsize(),1)

if __name__ == '__main__':
    unittest.main()
