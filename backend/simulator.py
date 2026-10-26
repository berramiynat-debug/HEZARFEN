"""Deterministic synthetic trajectories near the Sea of Marmara; no radio/sensor physics."""
import copy
import math
import random
from .security_engine import SecurityEngine, ROLES


class Simulator:
    def __init__(self, seed=42):
        self.seed = seed
        self.security = SecurityEngine()
        self.tick = 0
        self.running = False
        self.message_counter = 0
        self.contacts = {}
        self.evidence = {}
        self.links = dict.fromkeys(ROLES, 'UP')
        self.restore_tick = None
        self.routes = {key: 90.0 + i * 15 for i, key in enumerate(ROLES)}
        rng = random.Random(seed)
        self.origins = {key: (40.56 + i * .003, 27.50 + rng.random() * .01)
                        for i, key in enumerate(ROLES)}
        self.platforms = {}
        for i, (key, role) in enumerate(ROLES.items()):
            lat, lon = self.origins[key]
            self.platforms[key] = dict(id=key, kind='UAV' if key.startswith('UAV') else
                                      ('USV' if key.startswith('USV') else 'SHIP'),
                                      role=role, lat_deg=lat, lon_deg=lon,
                                      alt_m=150.0 + 20 * i if key.startswith('UAV') else 0.0,
                                      heading_deg=self.routes[key], security_state='Trusted')

    @property
    def t_sec(self):
        return round(self.tick / 5, 1)

    def snapshot(self):
        return copy.deepcopy(dict(platforms=list(self.platforms.values()),
                                  contacts=list(self.contacts.values())))

    def receive(self, sender, receiver, command, heading=90.0, message_id=None):
        self.message_counter += 1
        message_id = message_id or f'MSG-{self.message_counter:04d}'
        allowed, event, audit = self.security.authorize(message_id, sender, receiver, command)
        # The actual receiving application gate: rejected packets cannot mutate routes.
        if allowed and command in {'ROUTE_CHANGE', 'RETURN_TO_BASE'}:
            self.routes[receiver] = heading % 360
        for key in self.platforms:
            self.platforms[key]['security_state'] = self.security.states[key]
        return event, audit

    def contact(self):
        key = f'CONTACT-{len(self.contacts) + 1:02d}'
        if len(self.contacts) >= 100:
            raise ValueError('Reset mission before creating more than 100 contacts')
        contact = dict(id=key, lat_deg=40.59, lon_deg=27.56,
                       status='Unverified', source='UAV-02')
        self.contacts[key] = contact
        # Negative synthetic observations are not independent corroboration.
        self.evidence[key] = [dict(id=f'{key}-OBS-1', source='UAV-02', finding='REPORTED'),
                              dict(id=f'{key}-OBS-2', source='USV-02', finding='NOT_CORROBORATED'),
                              dict(id=f'{key}-OBS-3', source='UAV-01', finding='NOT_CORROBORATED')]
        return copy.deepcopy(contact)

    def link(self, down):
        self.links['UAV-03'] = 'DOWN' if down else 'UP'
        self.restore_tick = self.tick + 100 if down else None
        return dict(subject='UAV-03', status=self.links['UAV-03'],
                    reason='SYNTHETIC_LINK_OUTAGE' if down else 'SYNTHETIC_LINK_RESTORED')

    def step(self):
        if not self.running:
            return []
        self.tick += 1
        for i, (key, platform) in enumerate(self.platforms.items()):
            angle = math.radians(self.routes[key])
            speed = 22.0 if key.startswith('UAV') else 5.0
            platform['lat_deg'] += math.cos(angle) * speed * .2 / 111320
            platform['lon_deg'] += math.sin(angle) * speed * .2 / (
                111320 * math.cos(math.radians(platform['lat_deg'])))
            platform['heading_deg'] = self.routes[key]
        events = [('DELTA', copy.deepcopy(dict(platforms=list(self.platforms.values()))))]
        if self.restore_tick is not None and self.tick >= self.restore_tick:
            events.append(('LINK_EVENT', self.link(False)))
        return events
