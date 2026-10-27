"""CPU-only regression: interrupting the capture driver must reap its fixture."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='dengine-offscreen-cancel-') as scratch:
    root = Path(scratch)
    fixture = root / 'fixture'
    # No host, renderer, observer or platform startup. A detached sleeping CLI stands in
    # for the native child so this regression itself never opens a window or uses a GPU.
    fixture.write_text('#!' + sys.executable + '\n' + '''import os, sys, time
from pathlib import Path
if 'metal' in sys.argv:
    sys.exit(2)
marker = Path(os.environ['DENG_CANCEL_PID'])
pending = marker.with_suffix('.pending')
pending.write_text(str(os.getpid()))
pending.replace(marker)
time.sleep(300)
''')
    fixture.chmod(0o755)
    pid_file = root / 'pid'
    driver = Path(__file__).with_name('check_offscreen_capture.py')
    with (root / 'driver.log').open('w') as log:
        process = subprocess.Popen(
            [sys.executable, str(driver), str(fixture), '--output', str(root / 'matrix')],
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
            env={**os.environ, 'DENG_CANCEL_PID': str(pid_file)})
        child = None
        try:
            deadline = time.monotonic() + 10
            while not pid_file.exists():
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('capture driver failed to start the CPU fixture')
                time.sleep(0.01)
            child = int(pid_file.read_text())
            process.send_signal(signal.SIGINT)
            result = process.wait(timeout=10)
            if result == 0:
                raise RuntimeError('interruption incorrectly reported success')
            try:
                os.kill(child, 0)
            except ProcessLookupError:
                child = None
            else:
                raise RuntimeError('fixture survived capture-driver interruption')
            facts = json.loads((root / 'matrix/facts.json').read_text())
            rows = [row for row in facts['executions'] if row.get('interrupted')]
            if facts['status'] != 'FAIL' or len(rows) != 1 or rows[0]['timeout']:
                raise RuntimeError('missing or incorrectly classified cancellation evidence')
            if rows[0]['terminal'] != 'owned child/group stopped and reaped':
                raise RuntimeError('missing terminal child evidence')
            print('PASS: Ctrl-C stops/reaps the CPU fixture and retains failure evidence')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            if child is not None:
                try:
                    os.killpg(child, signal.SIGTERM)
                except ProcessLookupError:
                    pass
