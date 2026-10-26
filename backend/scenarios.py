"""150-second scripted presentation. Times are simulation ticks, never wall-clock triggers."""
TIMELINE = {0: 'start', 150: 'unauthorized', 300: 'contact', 450: 'repeat',
            550: 'link_down', 750: 'finish'}


def perform(sim, action):
    events, audits = [], []
    def receive(sender, receiver, command, **kwargs):
        event, audit = sim.receive(sender, receiver, command, **kwargs)
        events.append(('SECURITY_EVENT', event))
        audits.append(audit)
    if action == 'start':
        if not sim.running:
            sim.running = True
            receive('CMD-01', 'USV-01', 'TASK_ASSIGNMENT')
    elif action in {'unauthorized', 'repeat'}:
        receive('UAV-02', 'USV-01', 'ROUTE_CHANGE', heading=270)
        if action == 'repeat':
            receive('UAV-02', 'USV-01', 'TASK_ASSIGNMENT')
            # Also demonstrates that the restriction gates otherwise permitted requests.
            receive('UAV-02', 'CMD-01', 'REQUEST_TASK')
        receive('UAV-02', 'CMD-01', 'SAFETY_TELEMETRY')
    elif action == 'contact':
        receive('UAV-02', 'CMD-01', 'CONTACT_REPORT')
        events.append(('CONTACT_EVENT', sim.contact()))
    elif action in {'link_down', 'link_up'}:
        events.append(('LINK_EVENT', sim.link(action == 'link_down')))
    elif action == 'finish':
        sim.running = False
        events.append(('HEARTBEAT', {'running': False}))
    else:
        raise ValueError('Unknown action')
    return events, audits
