"""Actual WinHTTP control client + native paused SIMDIS screenshot on ephemeral ports."""
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.request

from viewer.tests.integration_check import ROOT, BUILD, EVIDENCE, EXE, ENV, wait_for


def main():
    EVIDENCE.mkdir(exist_ok=True)
    stamp = time.time_ns()
    out_path = BUILD / 'pause-backend.stdout.log'
    out = out_path.open('w')
    err = (BUILD / 'pause-backend.stderr.log').open('w')
    backend = subprocess.Popen([sys.executable, '-m', 'viewer.presentation_backend', '--demo',
        '--tcp-port', '0', '--http-port', '0', '--record', str(BUILD / f'session-pause-{stamp}.jsonl')],
        cwd=ROOT, env=ENV, stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW)
    viewer = None
    handles = []
    try:
        wait_for(lambda: 'pause/resume enabled' in out_path.read_text(encoding='utf-8'))
        tcp, http = map(int, re.search(r'TCP 127\.0\.0\.1:(\d+); panel http://127\.0\.0\.1:(\d+)', out_path.read_text()).groups())
        def state():
            with urllib.request.urlopen(f'http://127.0.0.1:{http}/state', timeout=3) as response:
                return json.load(response)
        def action(name):
            req = urllib.request.Request(f'http://127.0.0.1:{http}/action/{name}', method='POST')
            with urllib.request.urlopen(req, timeout=3) as response:
                assert response.status == 200
        probe = subprocess.run([str(BUILD / 'Debug' / 'ControlProbe.exe'), str(http)], cwd=ROOT,
            env=ENV, capture_output=True, text=True, timeout=20)
        assert probe.returncode == 0, probe.stdout + probe.stderr
        print(probe.stdout.strip(), flush=True)
        action('pause')
        frozen = state()
        for path in (BUILD / 'pause-viewer.stdout.log', BUILD / 'pause-viewer.stderr.log'):
            handles.append(path.open('w'))
        viewer = subprocess.Popen([str(EXE), '--port', str(tcp), '--control-port', str(http),
            '--capture', str(EVIDENCE / 'pause.png'), '--capture-at', '4', '--quit-after', '7',
            '--report', str(EVIDENCE / 'pause.json')], cwd=ROOT, env=ENV, stdout=handles[0], stderr=handles[1],
            creationflags=subprocess.CREATE_NO_WINDOW)
        assert viewer.wait(timeout=25) == 0
        current = state()
        assert current['t_sec'] == frozen['t_sec'] and current['platforms'] == frozen['platforms']
        report = json.loads((EVIDENCE / 'pause.json').read_text())
        assert report['paused'] and report['pause_controls_available'] and not report['stale']
        assert report['restart_controls_available']
        assert report['t_sec'] == frozen['t_sec'] and all(p['sdk_active'] for p in report['platforms'])
        assert (EVIDENCE / 'pause.png').stat().st_size > 20000
        action('resume')
        time.sleep(.7)
        assert state()['t_sec'] > frozen['t_sec']
        print('PASS paused native scene: time and all six positions held; heartbeat stays fresh; resume continues', flush=True)
    finally:
        if viewer and viewer.poll() is None:
            viewer.terminate()
            viewer.wait(timeout=5)
        if backend.poll() is None:
            backend.terminate()
            backend.wait(timeout=5)
        for handle in handles + [out, err]:
            handle.close()


if __name__ == '__main__':
    main()
