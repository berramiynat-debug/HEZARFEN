"""Local demonstrator policy; identities are configured, not cryptographically authenticated."""
from collections import deque

ROLES = {
    'CMD-01': 'COMMAND_AUTHORITY', 'USV-01': 'SURVEILLANCE',
    'USV-02': 'SURVEILLANCE', 'UAV-01': 'RECONNAISSANCE',
    'UAV-02': 'RECONNAISSANCE', 'UAV-03': 'COMMUNICATION_SUPPORT',
}
COMMANDS = frozenset({'ROUTE_CHANGE', 'TASK_ASSIGNMENT', 'RETURN_TO_BASE'})
MATRIX = {role: frozenset({'SAFETY_TELEMETRY', 'CONTACT_REPORT', 'REQUEST_TASK'}) |
          (COMMANDS if role == 'COMMAND_AUTHORITY' else frozenset())
          for role in set(ROLES.values())}


class SecurityEngine:
    def __init__(self):
        self.states = dict.fromkeys(ROLES, 'Trusted')
        self.violations = {key: set() for key in ROLES}
        self.audit = deque(maxlen=300)
        self.counter = 0

    def authorize(self, message_id, sender, receiver, command):
        """Called at the receiving application BEFORE any mission state mutation."""
        scope = 'SAFETY_TELEMETRY' if command == 'SAFETY_TELEMETRY' else (
            'OBSERVATION' if command == 'CONTACT_REPORT' else 'MISSION_COMMAND')
        reason, rule = 'ROLE_PERMITTED', 'RBAC-01'
        allowed = sender in ROLES and receiver in ROLES
        if not allowed:
            reason = 'UNKNOWN_IDENTITY'
        elif self.states[sender] == 'Restricted' and scope == 'MISSION_COMMAND':
            allowed, reason, rule = False, 'SCOPED_SENDER_RESTRICTION', 'LOCAL-02'
        elif command not in MATRIX[ROLES[sender]]:
            allowed, reason = False, 'SENDER_NOT_AUTHORIZED'
        if (not allowed and reason == 'SENDER_NOT_AUTHORIZED' and command in COMMANDS):
            # Two distinct message IDs, not retransmissions. Evidence capped at two.
            evidence = self.violations[sender]
            if len(evidence) < 2:
                evidence.add(message_id)
            self.states[sender] = 'Restricted' if len(evidence) >= 2 else 'Watch'
        self.counter += 1
        decision = 'ACCEPTED' if allowed else 'REJECTED'
        row = dict(message_id=message_id, sender=sender, receiver=receiver,
                   command=command, policy_rule=rule, decision=decision, reason=reason)
        self.audit.append(row)
        event = dict(event_id=f'EV-{self.counter:03d}', subject=sender,
                     code='MESSAGE_ACCEPTED' if allowed else 'UNAUTHORIZED_COMMAND',
                     result=decision, security_state=self.states.get(sender, 'Trusted'),
                     scope=scope, reason=reason, evidence_ids=[message_id])
        return allowed, event, row
