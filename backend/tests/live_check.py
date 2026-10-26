"""Real subprocess rehearsal on the presentation ports; takes approximately 151 seconds."""
import asyncio
import json
from pathlib import Path
import signal
import sys
import tempfile
import time
from backend.tcp_server import read_frame


async def main():
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)/'live.jsonl'
        proc = await asyncio.create_subprocess_exec(sys.executable, '-m', 'backend.main',
            '--demo', '--record', str(path), stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE)
        writer = None
        try:
            ready = await asyncio.wait_for(proc.stdout.readline(), 5)
            assert b'HEZARFEN ready' in ready, ready
            start = time.monotonic()
            reader, writer = await asyncio.open_connection('127.0.0.1',5556)
            first = await read_frame(reader)
            assert first['type']=='SNAPSHOT'
            events = [first]
            reconnected = False
            while True:
                event = await asyncio.wait_for(read_frame(reader),3)
                events.append(event)
                if event['t_sec'] >= 115 and not reconnected:
                    writer.close(); await writer.wait_closed()
                    reader,writer = await asyncio.open_connection('127.0.0.1',5556)
                    snap = await read_frame(reader)
                    assert snap['type']=='SNAPSHOT'
                    assert next(p for p in snap['data']['platforms'] if p['id']=='UAV-02')['security_state']=='Restricted'
                    events.append(snap)
                    link = await read_frame(reader)
                    assert link['type']=='LINK_EVENT' and link['data']['status']=='DOWN'
                    events.append(link)
                    reconnected = True
                if event['t_sec'] >= 150 and event['type']=='HEARTBEAT' and not event['data']['running']:
                    break
            elapsed = time.monotonic()-start
            assert 148 < elapsed < 155, elapsed
            assert sum(e['type']=='DELTA' for e in events)==750
            assert all(b['seq']>a['seq'] for a,b in zip(events,events[1:]))
            r,w = await asyncio.open_connection('127.0.0.1',8000)
            w.write(b'GET /state HTTP/1.1\r\nHost: 127.0.0.1:8000\r\n\r\n')
            await w.drain()
            response = await r.read()
            w.close(); await w.wait_closed()
            state = json.loads(response.split(b'\r\n\r\n',1)[1])
            assert state['links']['UAV-03']=='UP'
            assert state['contacts'][0]['status']=='Unverified'
            assert len(state['evidence']['CONTACT-01'])==3
            assert not state['quarantine_enabled']
            writer.close(); await writer.wait_closed(); writer=None
            proc.send_signal(signal.SIGTERM)
            _, errors = await asyncio.wait_for(proc.communicate(),5)
            assert proc.returncode==0, errors.decode()
            assert not errors, errors.decode()
            server = await asyncio.start_server(lambda r,w:w.close(),'127.0.0.1',5556)
            server.close(); await server.wait_closed()
            server = await asyncio.start_server(lambda r,w:w.close(),'127.0.0.1',8000)
            server.close(); await server.wait_closed()
            report = dict(elapsed_seconds=round(elapsed,3),received_events=len(events),
                deltas=750,reconnect_during_outage='PASS',contact_unverified='PASS',
                safety_telemetry_accepted=any(e['type']=='SECURITY_EVENT' and e['data']['subject']=='UAV-02' and e['data']['scope']=='SAFETY_TELEMETRY' and e['data']['result']=='ACCEPTED' for e in events),
                graceful_shutdown='PASS',ports_released='PASS')
            print(json.dumps(report,indent=2),flush=True)
        finally:
            if writer:
                writer.close(); await writer.wait_closed()
            if proc.returncode is None:
                proc.terminate()
                await proc.wait()


if __name__=='__main__':
    asyncio.run(main())
