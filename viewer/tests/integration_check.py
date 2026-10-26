"""Run actual C++ SIMDIS viewer against the unchanged Python backend, then offline.

Requires a Windows graphics session and a compiled Debug viewer. Writes only viewer/.
Run from repository root: python viewer/tests/integration_check.py
"""
from pathlib import Path
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'viewer' / 'build-verified'
EVIDENCE = ROOT / 'viewer' / 'evidence'
EXE = BUILD / 'Debug' / 'HezarfenViewer.exe'
ENV = dict(os.environ)
ENV['PATH'] = os.environ.get('SIMDIS_SDK_DIR', 'D:/SIMDIS_SDK') + '/bin;' + ENV['PATH']
ENV['SIMDIS_SDK_FILE_PATH'] = os.environ.get('SIMDIS_SDK_FILE_PATH', 'D:/Projects/SIMDIS_SDK-Data')
ENV['GDAL_DATA'] = os.environ.get('SIMDIS_SDK_DIR', 'D:/SIMDIS_SDK') + '/data/GDAL'
ENV['OSG_LIBRARY_PATH'] = os.environ.get('SIMDIS_SDK_DIR', 'D:/SIMDIS_SDK') + '/bin/osgPlugins-3.6.5'
ENV['PYTHONDONTWRITEBYTECODE'] = '1'


def wait_for(predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.1)
    raise AssertionError('timeout waiting for application readiness')


def action(name):
    request = urllib.request.Request('http://127.0.0.1:8000/action/' + name, method='POST')
    with urllib.request.urlopen(request, timeout=3) as response:
        assert response.status == 200


def launch_viewer(name, seconds, capture_at, *args):
    output = (BUILD / (name + '.stdout.log')).open('w')
    errors = (BUILD / (name + '.stderr.log')).open('w')
    process = subprocess.Popen([str(EXE), '--quit-after', str(seconds), '--capture',
        str(EVIDENCE / (name + '.png')), '--capture-at', str(capture_at), '--report',
        str(EVIDENCE / (name + '.json')), *args], cwd=ROOT, env=ENV,
        stdout=output, stderr=errors, creationflags=subprocess.CREATE_NO_WINDOW)
    return process, output, errors


def finished(handles, name, timeout):
    process, output, errors = handles
    try:
        result = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=5)
        raise
    finally:
        output.close()
        errors.close()
    assert result == 0, (name, result, (BUILD / (name + '.stderr.log')).read_text())
    assert (EVIDENCE / (name + '.png')).stat().st_size > 20000
    report = json.loads((EVIDENCE / (name + '.json')).read_text())
    assert report['ui'] == 'Dear ImGui' and report['ui_frames'] > 0
    assert report['pause_controls_available']
    assert len(report['platforms']) == 6
    assert all(p['sdk_active'] for p in report['platforms'])
    for p in report['platforms']:
        assert all(abs(a-b) < .01 for a,b in zip(p['ecef_m'], p['sdk_position_m']))
    return report


def main():
    EVIDENCE.mkdir(exist_ok=True)
    for port in (5556, 8000):
        with socket.socket() as test:
            test.bind(('127.0.0.1', port))  # Never interrupt someone else's backend.
    backend_out = (BUILD / 'backend.stdout.log').open('w')
    backend_err = (BUILD / 'backend.stderr.log').open('w')
    record = BUILD / ('session-integration-' + str(time.time_ns()) + '.jsonl')
    backend = subprocess.Popen([sys.executable, '-m', 'viewer.presentation_backend', '--record', str(record)],
        cwd=ROOT, env=ENV, stdout=backend_out, stderr=backend_err,
        creationflags=subprocess.CREATE_NO_WINDOW)
    viewer = None
    try:
        wait_for(lambda: 'HEZARFEN ready' in (BUILD / 'backend.stdout.log').read_text())
        viewer = launch_viewer('live', 22, 12)
        wait_for(lambda: 'SDK platform P:UAV-03' in (BUILD / 'live.stdout.log').read_text())
        action('start'); time.sleep(1)
        action('unauthorized'); time.sleep(1)
        action('contact'); time.sleep(1)
        action('repeat'); time.sleep(1)
        action('link_down')
        # Capture the actual link-down scene while all ground-truth paths continue.
        time.sleep(9)
        action('link_up')
        live = finished(viewer, 'live', 30); viewer = None
        values = {p['id']: p for p in live['platforms']}
        assert live['mode'] == 'LIVE TCP' and not live['stale']
        assert values['UAV-02']['security_state'] == 'Restricted'
        assert values['UAV-03']['security_state'] == 'Trusted' and values['UAV-03']['link'] == 'UP'
        assert live['contacts'][0]['status'] == 'Unverified' and live['security_events'] >= 5
        # The viewer remains open across actual socket loss and backend process restart.
        viewer = launch_viewer('reconnect', 16, 5)
        wait_for(lambda: 'SDK platform P:UAV-03' in (BUILD / 'reconnect.stdout.log').read_text())
        backend.terminate(); backend.wait(timeout=5)
        time.sleep(7)
        backend_out.close(); backend_err.close()
        backend_out = (BUILD / 'backend-restarted.stdout.log').open('w')
        backend_err = (BUILD / 'backend-restarted.stderr.log').open('w')
        record = BUILD / ('session-restarted-' + str(time.time_ns()) + '.jsonl')
        backend = subprocess.Popen([sys.executable, '-m', 'viewer.presentation_backend', '--record', str(record)],
            cwd=ROOT, env=ENV, stdout=backend_out, stderr=backend_err,
            creationflags=subprocess.CREATE_NO_WINDOW)
        wait_for(lambda: 'HEZARFEN ready' in (BUILD / 'backend-restarted.stdout.log').read_text())
        reconnect = finished(viewer, 'reconnect', 25); viewer = None
        assert reconnect['snapshots'] >= 2 and not reconnect['stale']
        assert not reconnect['contacts'] and all(p['security_state'] == 'Trusted' for p in reconnect['platforms'])
        assert all(p['link'] == 'UP' for p in reconnect['platforms'])
    finally:
        if viewer and viewer[0].poll() is None:
            viewer[0].terminate(); viewer[0].wait(timeout=5)
            viewer[1].close(); viewer[2].close()
        if backend.poll() is None:
            backend.terminate(); backend.wait(timeout=5)
        backend_out.close(); backend_err.close()
    # Offline fallback is tested only after live mode.
    offline = launch_viewer('replay', 14, 8, '--replay', str(ROOT / 'recordings' / 'demo-events.jsonl'), '--speed', '15')
    replay = finished(offline, 'replay', 30)
    assert replay['applied'] == 913 and replay['t_sec'] == 150
    assert replay['source_status'].startswith('REPLAY COMPLETE') and not replay['stale']
    assert replay['contacts'][0]['status'] == 'Unverified'
    print(json.dumps({'live': 'PASS', 'six_actual_sdk_nodes': 'PASS', 'ecef_datastore_readback': 'PASS',
        'actual_disconnect_restart_snapshot': 'PASS', 'offline_913_messages': 'PASS',
        'framebuffer_captures': ['live.png', 'reconnect.png', 'replay.png']}, indent=2))


if __name__ == '__main__':
    main()
