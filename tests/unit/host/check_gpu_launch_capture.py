#!/usr/bin/env python3
"""Opt-in DENG-573 Metal host capture proof. Native GPU required; no window is opened.
Usage: python3 check_gpu_launch_capture.py /path/to/de-offscreen-capture /fresh/output/directory
Preserves each log and trace. Kills only its owned subprocess group on a bounded timeout.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def contains(path, needle):
    with path.open("rb") as stream:
        carry = b""
        while chunk := stream.read(1024 * 1024):
            data = carry + chunk
            if needle in data:
                return True
            carry = data[-len(needle):]
    return False


def run(exe, root, name, mode, pipeline, frames):
    directory = root / name
    directory.mkdir()
    env = dict(os.environ)
    for key in ("DE_GPU_CAPTURE", "DE_GPU_CAPTURE_DIR", "DE_GPU_CAPTURE_FRAMES"):
        env.pop(key, None)
    if mode != "disabled":
        env.update(DE_GPU_CAPTURE="xcode", DE_GPU_CAPTURE_DIR=str(directory),
                   DE_GPU_CAPTURE_FRAMES=str(frames))
    command = [str(exe), "--backend", "metal", "--allow-native", "--pipeline", str(pipeline),
               "--scratch", str(directory)]
    if mode == "explicit":
        command.append("--gpu-capture")
    log = directory / "host.log"
    with log.open("w") as output:
        process = subprocess.Popen(command, env=env, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            code = process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=10)
            raise RuntimeError(f"{name}: host exceeded 120 seconds; owned group killed")
    text = log.read_text()
    if code != 0:
        raise RuntimeError(f"{name}: host exit {code}; see {log}")
    report = json.loads(next(line for line in reversed(text.splitlines()) if line.startswith('{"ok"')))
    if not report["ok"] or not report["native_pixels"] or not report["asset_pixels"]:
        raise RuntimeError(f"{name}: original fixture acceptance failed")
    traces = list(directory.glob("*.gputrace"))
    if mode != "explicit":
        if traces or "capture: complete" in text:
            raise RuntimeError(f"{name}: an unrequested capture was made")
    else:
        if len(traces) != 1:
            raise RuntimeError(f"{name}: expected one native trace, found {len(traces)}")
        if frames <= 4:
            if f"capture: complete ({frames} frames)" not in text:
                raise RuntimeError(f"{name}: requested completion missing")
        elif "capture: complete" in text or "capture: incomplete (4/100 frames)" not in text:
            raise RuntimeError(f"{name}: early exit incorrectly reported completion")
        # Metal can finish writing after stopCapture. Poll readiness, never rerun the workload.
        deadline = time.monotonic() + 10
        while True:
            if any(contains(p, b"offscreen.fixture.clear") for p in traces[0].rglob("*") if p.is_file()):
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(f"{name}: real render graph marker absent from trace")
            time.sleep(0.1)
    print(f"PASS {name}: {log}", flush=True)


def main():
    if sys.platform != "darwin" or len(sys.argv) != 3:
        raise SystemExit(__doc__)
    exe = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    root.mkdir(parents=True, exist_ok=False)
    for case in [("disabled", "disabled", 1, 2), ("armed", "armed", 1, 2),
                 ("explicit_sync", "explicit", 0, 2), ("explicit_async", "explicit", 1, 2),
                 ("early_exit", "explicit", 1, 100)]:
        run(exe, root, *case)


if __name__ == "__main__":
    main()
