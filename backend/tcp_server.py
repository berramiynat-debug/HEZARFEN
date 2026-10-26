"""Length-prefixed UTF-8 JSON transport with bounded per-client buffering."""
import asyncio
import json
import math
from .security_engine import ROLES
import struct

MAX_PAYLOAD = 1024 * 1024
TYPES = {'SNAPSHOT', 'DELTA', 'SECURITY_EVENT', 'CONTACT_EVENT', 'LINK_EVENT', 'HEARTBEAT'}


def validate(event):
    if not isinstance(event, dict) or set(event) != {'v', 'type', 'seq', 't_sec', 'data'}:
        raise ValueError('Invalid envelope keys')
    if type(event['v']) is not int or event['v'] != 1 or event['type'] not in TYPES:
        raise ValueError('Invalid version or type')
    if type(event['seq']) is not int or event['seq'] < 1:
        raise ValueError('Invalid sequence')
    if type(event['t_sec']) not in (int, float) or not 0 <= event['t_sec'] < float('inf'):
        raise ValueError('Invalid timestamp')
    if not isinstance(event['data'], dict):
        raise ValueError('Invalid data')
    data = event['data']
    def keys(value, expected):
        if not isinstance(value, dict) or set(value) != set(expected.split()):
            raise ValueError('Invalid data keys')
    def position(value):
        for field, low, high in [('lat_deg', -90, 90), ('lon_deg', -180, 180)]:
            number = value[field]
            if type(number) not in (int, float) or not low <= number <= high:
                raise ValueError('Invalid WGS84 position')
    def platform(value):
        keys(value, 'id kind role lat_deg lon_deg alt_m heading_deg security_state')
        if value['id'] not in ROLES or value['role'] != ROLES[value['id']]:
            raise ValueError('Invalid platform identity or role')
        expected_kind = 'UAV' if value['id'].startswith('UAV') else ('USV' if value['id'].startswith('USV') else 'SHIP')
        if value['kind'] != expected_kind or value['security_state'] not in {'Trusted', 'Watch', 'Restricted'}:
            raise ValueError('Invalid platform kind or security state')
        position(value)
        if type(value['alt_m']) not in (int, float) or not math.isfinite(value['alt_m']):
            raise ValueError('Invalid altitude')
        if (expected_kind == 'UAV' and value['alt_m'] <= 0) or (expected_kind != 'UAV' and value['alt_m'] != 0):
            raise ValueError('Invalid surface or air altitude')
        if type(value['heading_deg']) not in (int, float) or not 0 <= value['heading_deg'] < 360:
            raise ValueError('Invalid heading')
    def contact(value):
        keys(value, 'id lat_deg lon_deg status source')
        position(value)
        if not isinstance(value['id'], str) or not value['id'] or value['source'] not in ROLES or value['status'] not in {'Unverified', 'Contested', 'Confirmed'}:
            raise ValueError('Invalid contact')
    kind = event['type']
    if kind in {'SNAPSHOT', 'DELTA'}:
        keys(data, 'platforms contacts' if kind == 'SNAPSHOT' else 'platforms')
        if not isinstance(data['platforms'], list):
            raise ValueError('Platforms must be a list')
        for value in data['platforms']:
            platform(value)
        ids = [p['id'] for p in data['platforms']]
        if len(ids) != len(set(ids)) or (kind == 'SNAPSHOT' and set(ids) != set(ROLES)):
            raise ValueError('Duplicate or missing platforms')
        if kind == 'SNAPSHOT':
            if not isinstance(data['contacts'], list):
                raise ValueError('Contacts must be a list')
            for value in data['contacts']:
                contact(value)
    elif kind == 'CONTACT_EVENT':
        contact(data)
    elif kind == 'SECURITY_EVENT':
        keys(data, 'event_id subject code result security_state scope reason evidence_ids')
        if data['subject'] not in ROLES or data['result'] not in {'ACCEPTED', 'REJECTED'} or data['security_state'] not in {'Trusted', 'Watch', 'Restricted'}:
            raise ValueError('Invalid security decision')
        if not isinstance(data['evidence_ids'], list) or not all(isinstance(e, str) for e in data['evidence_ids']):
            raise ValueError('Invalid evidence references')
        if not all(isinstance(data[k], str) and data[k] for k in ('event_id', 'code', 'scope', 'reason')):
            raise ValueError('Invalid security strings')
    elif kind == 'LINK_EVENT':
        keys(data, 'subject status reason')
        if data['subject'] not in ROLES or data['status'] not in {'UP', 'DOWN'} or not isinstance(data['reason'], str):
            raise ValueError('Invalid link event')
    else:
        keys(data, 'running')
        if type(data['running']) is not bool:
            raise ValueError('Invalid heartbeat')
    return event


def encode(event):
    validate(event)
    payload = json.dumps(event, separators=(',', ':'), ensure_ascii=False, allow_nan=False).encode('utf-8')
    if len(payload) > MAX_PAYLOAD:
        raise ValueError('Payload exceeds 1 MiB')
    return struct.pack('!I', len(payload)) + payload


def decode(payload):
    if not 0 < len(payload) <= MAX_PAYLOAD:
        raise ValueError('Invalid payload length')
    return validate(json.loads(payload.decode('utf-8'),
                               parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x))))


async def read_frame(reader):
    length, = struct.unpack('!I', await reader.readexactly(4))
    if not 0 < length <= MAX_PAYLOAD:
        raise ValueError('Invalid frame length')
    return decode(await reader.readexactly(length))


class TCPServer:
    def __init__(self, on_connect):
        self.on_connect = on_connect
        self.clients = {}
        self.tasks = set()
        self.server = None

    async def start(self, port=5556):
        self.server = await asyncio.start_server(self.handle, '127.0.0.1', port, limit=4096)
        return self.server.sockets[0].getsockname()[1]

    def publish(self, event):
        frame = encode(event)
        for writer, queue in list(self.clients.items()):
            try:
                queue.put_nowait(frame)
            except asyncio.QueueFull:
                # Disconnect lagging consumers; their reconnect gets a fresh snapshot.
                writer.close()
                self.clients.pop(writer, None)

    async def handle(self, reader, writer):
        task = asyncio.current_task()
        self.tasks.add(task)
        if len(self.clients) >= 16:
            writer.close()
            self.tasks.discard(task)
            return
        queue = asyncio.Queue(maxsize=128)
        self.clients[writer] = queue
        async def send():
            while True:
                writer.write(await queue.get())
                await asyncio.wait_for(writer.drain(), 2)
        async def watch():
            # This is a one-way event feed; inbound application bytes are not accepted.
            await reader.read(1)
        pending = []
        try:
            self.on_connect()
            pending = [asyncio.create_task(send()), asyncio.create_task(watch())]
            done, _ = await asyncio.wait(pending, return_when=asyncio.FIRST_COMPLETED)
            for item in done:
                item.result()
        except (ConnectionError, OSError, asyncio.TimeoutError):
            pass
        finally:
            for item in pending:
                item.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
            self.clients.pop(writer, None)
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass
            self.tasks.discard(task)

    async def close(self):
        if self.server:
            self.server.close()
        tasks = list(self.tasks)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if self.server:
            await self.server.wait_closed()
