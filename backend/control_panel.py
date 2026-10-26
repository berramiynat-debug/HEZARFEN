"""Small loopback-only HTTP server; controls run on the simulation's event loop."""
import asyncio
import json

HTML = '''<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HEZARFEN | Mission Security &amp; Monitoring</title>
<style>body{background:#101b2b;color:#e1edf6;font:16px system-ui;margin:30px;max-width:1200px}button{padding:12px;margin:5px;background:#1d4260;color:white;border:1px solid #5ea3bf;border-radius:5px;cursor:pointer}pre{white-space:pre-wrap;background:#17283a;padding:15px;max-height:420px;overflow:auto}small{color:#a5cbdc}h1{font-size:26px}</style>
<h1>HEZARFEN | Mission Security &amp; Monitoring</h1>
<p>Synthetic software demonstrator • 6 platforms • local policy enforcement</p>
<div id="buttons"></div><p id="result" role="status"></p>
<small>Quarantine disabled: no cryptographically verified equivocation proof is implemented.
Link outages are availability events. Unverified observations are not compromise evidence.</small>
<h2>Mission state</h2><pre id="state"></pre><h2>Contact evidence</h2><pre id="evidence"></pre>
<h2>Receiver decisions and events (latest 300)</h2><pre id="log"></pre>
<script>
const actions=[['start','Start Mission'],['unauthorized','Unauthorized Command'],['contact','Inject Unverified Contact'],['repeat','Repeat Policy Violation'],['link_down','Link Outage'],['link_up','Restore Link'],['reset','Reset'],['replay','Replay']];
for(const [action,label] of actions){const b=document.createElement('button');b.textContent=label;b.onclick=async()=>{b.disabled=true;try{const r=await fetch('/action/'+action,{method:'POST'});const j=await r.json();document.getElementById('result').textContent=j.message||j.error;}catch(e){document.getElementById('result').textContent='Backend unavailable: '+e.message;}finally{b.disabled=false;}};document.getElementById('buttons').append(b);}
async function poll(){try{const r=await fetch('/state');const s=await r.json();document.getElementById('state').textContent=JSON.stringify({mode:s.mode,time:s.t_sec,running:s.running,links:s.links,platforms:s.platforms,contacts:s.contacts},null,2);document.getElementById('evidence').textContent=JSON.stringify(s.evidence,null,2);document.getElementById('log').textContent=JSON.stringify(s.log,null,2);}catch(e){document.getElementById('result').textContent='Connection lost';}setTimeout(poll,1000);}poll();
</script></html>'''


class ControlPanel:
    def __init__(self, runtime):
        self.runtime = runtime
        self.server = None
        self.tasks = set()
        self.writers = set()
        self.port = 8000

    async def start(self, port=8000):
        self.server = await asyncio.start_server(self.handle, '127.0.0.1', port, limit=8192)
        self.port = self.server.sockets[0].getsockname()[1]
        return self.port

    async def handle(self, reader, writer):
        task = asyncio.current_task()
        if len(self.tasks) >= 32:
            writer.close()
            return
        self.tasks.add(task)
        self.writers.add(writer)
        status, content_type = 200, 'application/json'
        try:
            raw = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 2)
            lines = raw.decode('ascii').split('\r\n')
            method, path, version = lines[0].split()
            headers = dict(line.split(':', 1) for line in lines[1:] if ':' in line)
            headers = {k.lower(): v.strip() for k, v in headers.items()}
            hosts = {f'127.0.0.1:{self.port}', f'localhost:{self.port}'}
            if headers.get('host') not in hosts:
                raise ValueError('Invalid Host')
            if method == 'GET' and path == '/':
                body, content_type = HTML.encode(), 'text/html; charset=utf-8'
            elif method == 'GET' and path == '/state':
                body = json.dumps(self.runtime.status()).encode()
            elif method == 'POST' and path.startswith('/action/'):
                if headers.get('origin') and headers['origin'] not in {'http://' + h for h in hosts}:
                    raise ValueError('Cross-origin action rejected')
                if headers.get('sec-fetch-site') == 'cross-site':
                    raise ValueError('Cross-site action rejected')
                if headers.get('transfer-encoding') or headers.get('content-length', '0') != '0':
                    raise ValueError('Actions do not accept request bodies')
                self.runtime.action(path[len('/action/'):])
                body = json.dumps({'message': 'Backend action executed'}).encode()
            else:
                status, body = 404, b'{"error":"Not found"}'
        except (ValueError, UnicodeError, asyncio.LimitOverrunError, asyncio.IncompleteReadError, asyncio.TimeoutError) as exc:
            status, body = 400, json.dumps({'error': str(exc)}).encode()
        except OSError as exc:
            status, body = 503, json.dumps({'error': str(exc)}).encode()
        try:
            writer.write(f'HTTP/1.1 {status} Response\r\nContent-Type: {content_type}\r\nContent-Length: {len(body)}\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n'.encode() + body)
            await asyncio.wait_for(writer.drain(), 2)
        except (ConnectionError, OSError, asyncio.TimeoutError):
            pass
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass
            self.tasks.discard(task)
            self.writers.discard(writer)

    async def close(self):
        if self.server:
            self.server.close()
        writers = list(self.writers)
        for writer in writers:
            writer.close()
        tasks = list(self.tasks)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        await asyncio.gather(*(writer.wait_closed() for writer in writers), return_exceptions=True)
        self.writers.clear()
        self.tasks.clear()
        if self.server:
            await self.server.wait_closed()
