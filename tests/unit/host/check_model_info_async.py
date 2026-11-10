#!/usr/bin/env python3
"""DENG-692 real cold modelInfo dispatch and legacy metadata regression.

Build model_inspection and run with a Python containing msgpack. Uses private temporary
IPC/cache paths. --record-only permits baseline cold failure; --compare checks the entire
warm legacy response against a baseline JSON recorded by this script. No IDE UI claim.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import tempfile
import time

import msgpack


def main():
    if not __debug__:
        raise RuntimeError("Run without -O: assertions are required")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("--record-only", action="store_true")
    parser.add_argument("--compare", type=Path)
    args = parser.parse_args()
    game = args.game.resolve()
    with tempfile.TemporaryDirectory(prefix="deng692-", dir="/tmp") as tmp:
        endpoint = str(Path(tmp) / "editor.sock")
        token = secrets.token_hex(32)
        env = dict(os.environ, HOME=tmp, XDG_CACHE_HOME=tmp, XDG_CONFIG_HOME=tmp,
                   DE_EDITOR_TOKEN=token)
        with open(Path(tmp) / "engine.log", "w+") as log:
            proc = subprocess.Popen([str(game), "--editor-service", endpoint,
                                     "--editor-grace-s", "15", "--no-splash"],
                                    cwd=game.parent, env=env, stdout=log, stderr=log)
            sock = socket.socket(socket.AF_UNIX)
            sock.settimeout(20)
            try:
                deadline = time.monotonic() + 30
                while not Path(endpoint).exists():
                    if proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("editor service did not listen")
                    time.sleep(.1)
                sock.connect(endpoint)

                def send(request, method, **params):
                    body = b"\x01" + msgpack.packb(
                        dict(id=request, method=method, params=params), use_bin_type=True)
                    sock.sendall(struct.pack("<I", len(body)) + body)

                def exact(size):
                    data = b""
                    while len(data) < size:
                        chunk = sock.recv(size - len(data))
                        if not chunk:
                            raise RuntimeError("editor disconnected")
                        data += chunk
                    return data

                def receive():
                    size, = struct.unpack("<I", exact(4))
                    assert 0 < size <= 64 * 1024 * 1024
                    body = exact(size)
                    return body[0], msgpack.unpackb(body[1:], raw=False) if body[0] != 4 else {}

                def response(request):
                    deadline = time.monotonic() + 20
                    while time.monotonic() < deadline:
                        kind, result = receive()
                        if kind == 2 and result["id"] == request:
                            return result
                    raise TimeoutError(request)

                send(1, "hello", protocol="1.8", token=token, capabilities=[])
                assert "result" in response(1)
                start = time.monotonic()
                send(2, "asset.modelInfo", id="models/inspection")
                send(3, "asset.status", ids=["models/inspection"])
                pending = {2, 3}
                order, replies = [], {}
                while pending:
                    kind, result = receive()
                    if kind == 2 and result["id"] in pending:
                        request = result["id"]
                        print("response", request, "seconds", time.monotonic() - start, result)
                        pending.remove(request)
                        order.append(request)
                        replies[request] = result
                assert "result" in replies[3], replies[3]
                # Let ordinary frame integration finish after the baseline wait timed out.
                deadline = time.monotonic() + 15
                request = 10
                while time.monotonic() < deadline:
                    request += 1
                    send(request, "asset.modelInspect", id="models/inspection", mesh=0)
                    if "result" in response(request):
                        break
                    time.sleep(.05)
                send(100, "asset.modelInfo", id="models/inspection")
                warm = response(100)
                assert "result" in warm, warm
                args.metadata.write_text(json.dumps(warm["result"], sort_keys=True, indent=2))
                if args.compare:
                    assert warm["result"] == json.loads(args.compare.read_text())
                    print("Complete warm legacy metadata matches baseline")
                if not args.record_only:
                    # Ordering, not an arbitrary millisecond performance threshold.
                    assert order == [3, 2] and "result" in replies[2], (order, replies)
                    assert replies[2]["result"] == warm["result"]
                    print("PASS: unrelated dispatch progresses before cold success; cold/warm metadata equal")
            finally:
                sock.close()
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
                log.flush()
                log.seek(0)
                print(log.read())


if __name__ == "__main__":
    main()
