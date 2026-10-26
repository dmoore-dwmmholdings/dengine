"""Public fixture driver. Default/CTest is null-only; native needs an explicit opt-in.

Native pixel equality does not establish absence of app activation or Space changes.
Use the separate supervised desktop observer and retain the user's observations.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--backend', choices=('null', 'metal', 'vulkan', 'd3d12'), default='null')
parser.add_argument('--allow-native', action='store_true')
parser.add_argument('--output', type=Path)
parser.add_argument('--observer', type=Path,
                    help='macOS desktop observer; required for supervised Space/window evidence')
args = parser.parse_args()
if args.backend != 'null' and not args.allow_native:
    parser.error('native execution requires --allow-native and a supervised test session')
binary = args.binary.resolve()
root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='dengine-offscreen-'))
if args.output:
    root.mkdir(parents=True, exist_ok=False)
facts = {'status': 'RUNNING', 'backend': args.backend, 'executions': [],
         'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
         'limits': 'Null is CPU/lifecycle only. Native image equality is not desktop/Space, '
                   'presentation-performance, LSan, or cross-platform qualification.'}
environment = dict(os.environ)
if args.backend == 'metal':
    # Metal selects its validation layers before device creation; desc flags alone cannot
    # enable them. Retain the native validation log as evidence, not just these requests.
    environment.update(MTL_DEBUG_LAYER='1', MTL_SHADER_VALIDATION='1')
facts['validation_environment'] = {key: environment[key] for key in
                                  ('MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION') if key in environment}


def save():
    (root / 'facts.json').write_text(json.dumps(facts, indent=2) + '\n')


def execute(name, command, guard=False):
    desktop = root / (name + '-desktop.json')
    if args.observer and not guard:
        command = [args.observer.resolve(), '--report', desktop, '--', *command]
    command = list(map(str, command))
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                               env=environment,
                               **({'start_new_session': True} if os.name != 'nt' else {}))
    try:
        stdout, stderr = process.communicate(timeout=620 if args.backend != 'null' else 120)
    except subprocess.TimeoutExpired as error:
        # An observer owns a fixture child. Stop the isolated group while its leader still
        # has this unreaped PID; never leave a native fixture behind on a harness timeout.
        if process.poll() is None:
            if os.name != 'nt':
                os.killpg(process.pid, signal.SIGTERM)
            else:
                process.terminate()
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            if process.poll() is None:
                if os.name != 'nt':
                    os.killpg(process.pid, signal.SIGKILL)
                else:
                    process.kill()
            stdout, stderr = process.communicate()
        (root / (name + '.log')).write_text(stdout + stderr)
        facts['executions'].append({'name': name, 'command': command,
                                    'timeout': True, 'terminal': 'owned child/group stopped and reaped',
                                    'exit': process.returncode})
        save()
        raise
    log = stdout + stderr
    (root / (name + '.log')).write_text(log)
    row = {'name': name, 'command': command, 'exit': process.returncode}
    facts['executions'].append(row)
    save()
    if guard:
        if process.returncode != 2 or 'offscreen: no video' in log or 'gpu device:' in log:
            raise RuntimeError('native opt-in guard failed before host startup')
        return None
    if process.returncode:
        raise RuntimeError(f'{name}: exit {process.returncode}; retained original log')
    if args.observer:
        row['desktop'] = json.loads(desktop.read_text())
        if not row['desktop']['ok']:
            raise RuntimeError(name + ': desktop observer failed')
    data = [json.loads(line) for line in stdout.splitlines() if line.startswith('{"ok":')]
    if len(data) != 1 or not data[0]['ok']:
        raise RuntimeError(name + ': missing/failed fixture report')
    row['report'] = report = data[0]
    if report['actual_backend'] != {'null': 4, 'metal': 3, 'vulkan': 2, 'd3d12': 1}[args.backend]:
        raise RuntimeError(name + ': actual device backend differs')
    if any(report[k] for k in ('tracked_bytes', 'tracked_blocks', 'gpu_leak_warnings',
                               'invalid_hook_info')):
        raise RuntimeError(name + ': resource or hook validation failed')
    save()
    return report


try:
    save()
    guard = root / 'guard'
    guard.mkdir()
    execute('native-opt-in-guard', [binary, '--backend', 'metal', '--scratch', guard], guard=True)
    captures = []
    for pipeline, failure in [(0, 0), (1, 0), (0, 1), (0, 2), (0, 3), (0, 4)]:
        name = f'p{pipeline}-failure{failure}'
        scratch = root / name
        scratch.mkdir()
        image = root / (name + '.rgba')
        command = [binary, '--backend', args.backend, '--pipeline', pipeline,
                   '--fail-after', failure, '--scratch', scratch, '--output', image]
        if args.allow_native:
            command.append('--allow-native')
        report = execute(name, command)
        if report['failure_stage'] != failure or report['pipeline'] != pipeline:
            raise RuntimeError(name + ': fixture exercised the wrong mode')
        if failure:
            if not report['intentional'] or report['shutdowns'] or report['host_exit'] != 1:
                raise RuntimeError(name + ': failure was not the requested boundary')
        elif not report['asset_ready'] or report['hooks'] != 4 or report['passes'] != 4:
            raise RuntimeError(name + ': real load/Extract/hook path missing')
        elif args.backend == 'null':
            if report['native_pixels'] or image.exists():
                raise RuntimeError(name + ': null backend must not claim a native image')
        else:
            pixels = image.read_bytes()
            if not report['native_pixels'] or not report['asset_pixels'] or len(pixels) != 64 * 48 * 4:
                raise RuntimeError(name + ': native capture or exact uploaded checker missing')
            digest = 14695981039346656037
            for byte in pixels:
                digest = ((digest ^ byte) * 1099511628211) & ((1 << 64) - 1)
            if f'{digest:016x}' != report['image_hash']:
                raise RuntimeError(name + ': independent raw capture hash differs')
            captures.append(pixels)
    if captures and captures[0] != captures[1]:
        raise RuntimeError('complete pipelined/nonpipelined native captures differ')
    facts['status'] = 'PASS'
    facts['native_image_pair_exact'] = bool(captures)
    save()
    print(f"{args.backend}: six actual host lifecycles + native opt-in refusal pass; "
          f"native image comparison={bool(captures)}")
except BaseException as error:
    facts['status'] = 'FAIL'
    facts['error'] = str(error)
    save()
    print('Evidence retained:', root)
    raise
else:
    if not args.output:
        shutil.rmtree(root)
