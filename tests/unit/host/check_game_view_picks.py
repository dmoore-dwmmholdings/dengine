#!/usr/bin/env python3
"""DENG-665 real-service regression (macOS, Metal, MessagePack dependency).

Build `flappy`, then run with Python containing msgpack:
  python check_game_view_picks.py build/dev/examples/flappy/flappy/flappy
Uses private temporary IPC/cache paths, never mutates the example scene. This is
an explicit native integration check, not a mocked renderer/unit-test substitute.
"""
import argparse
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import time

import msgpack


def main():
    if not __debug__:
        sys.exit("Run without -O: this regression requires assertions")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    args = parser.parse_args()
    game = args.game.resolve()
    with tempfile.TemporaryDirectory(prefix="deng665-", dir="/tmp") as tmp:
        endpoint = str(Path(tmp) / "editor.sock")
        token = secrets.token_hex(32)
        env = dict(os.environ, DE_EDITOR_TOKEN=token, HOME=tmp,
                   XDG_CACHE_HOME=tmp, XDG_CONFIG_HOME=tmp)
        with open(Path(tmp) / "engine.log", "w+") as log:
            proc = subprocess.Popen([str(game), "--editor-service", endpoint,
                                     "--editor-grace-s", "15", "--no-splash"],
                                    cwd=game.parent, env=env, stdout=log, stderr=log)
            sock = socket.socket(socket.AF_UNIX)
            sock.settimeout(30)
            try:
                until = time.monotonic() + 60
                while not Path(endpoint).exists():
                    if proc.poll() is not None or time.monotonic() > until:
                        raise RuntimeError("editor service failed to listen")
                    time.sleep(.1)
                sock.connect(endpoint)
                request_id = 0

                def exact(n):
                    out = b""
                    while len(out) < n:
                        chunk = sock.recv(n - len(out))
                        if not chunk:
                            raise RuntimeError("editor disconnected")
                        out += chunk
                    return out

                def receive():
                    size, = struct.unpack("<I", exact(4))
                    assert 0 < size <= 64 * 1024 * 1024
                    data = exact(size)
                    return data[0], msgpack.unpackb(data[1:], raw=False) if data[0] != 4 else None

                def call(method, **params):
                    nonlocal request_id
                    request_id += 1
                    body = msgpack.packb(dict(id=request_id, method=method, params=params), use_bin_type=True)
                    sock.sendall(struct.pack("<I", len(body) + 1) + b"\x01" + body)
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        kind, response = receive()
                        if kind == 2 and response["id"] == request_id:
                            assert "error" not in response, (method, response)
                            return response["result"]
                    raise TimeoutError(method)

                hello = call("hello", protocol="1.8", token=token, capabilities=[])
                assert "views.sharedTexture" in hello["capabilities"]
                scene_path = Path(__file__).resolve().parents[3] / "examples/flappy/flappy.scene"
                call("scene.load", path=str(scene_path), additive=False)
                camera = dict(position=[0., -12., 6.25], rotation=[0., 0., 0., 1.],
                              fovY=0., orthoHeight=12.5, near=.1, far=1000.)
                views = [call("view.create", width=320, height=400, scale=1., camera=camera,
                              renderMode="lit", showFlags=[], game=is_game)
                         for is_game in (False, True)]
                scene, game_view = [v["view"] for v in views]

                def frame(view):
                    # Drain enough fresh notifications to cover producer/readback latency.
                    count = 0
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        kind, note = receive()
                        if kind == 3 and note.get("topic") == "view.frame":
                            data = note["data"]
                            if data["view"] == view["view"]:
                                count += 1
                                if count >= 8:
                                    path = view["surfaces"][data["surface"]]["handle"]
                                    raw = Path(path).read_bytes()
                                    if struct.unpack_from("=Q", raw)[0] == data["fenceValue"]:
                                        return raw[64:]
                    raise TimeoutError("stable shared frame")

                baseline = frame(views[1])
                # Select a real GPU hit rather than relying on serialized ECS ids.
                hit = None
                for y in (200., 280., 120., 340.):
                    for x in (160., 80., 240.):
                        point = call("pick", view=game_view, x=x, y=y)
                        if point.get("entity"):
                            hit = (x, y, point["entity"])
                            break
                    if hit:
                        break
                assert hit, "fixture must expose a pickable rendered Game entity"
                x, y, entity = hit
                rect = [x - 2., y - 2., 4., 4.]
                assert entity in call("pick.rect", view=game_view, rect=rect)["entities"]
                assert entity in call("pick.rect", view=scene, rect=rect)["entities"]
                call("view.setHidden", entities=[entity])
                point_kept = call("pick", view=game_view, x=x, y=y).get("entity") == entity
                rect_kept = entity in call("pick.rect", view=game_view, rect=rect)["entities"]
                assert point_kept and rect_kept, (
                    "hidden Game hit retained", {"point": point_kept, "rectangle": rect_kept})
                assert call("pick", view=scene, x=x, y=y).get("entity") != entity
                assert entity not in call("pick.rect", view=scene, rect=rect)["entities"]
                assert call("pick", view=game_view, x=x, y=y, skip=[entity]).get("entity") != entity
                assert frame(views[1]) == baseline, "Scene hide changed canonical Game pixels"
                call("view.setHidden", entities=[])
                assert entity in call("pick.rect", view=scene, rect=rect)["entities"]
                for view in views:
                    call("view.destroy", view=view["view"])
                print("PASS: Game point/rect picks and pixels remain canonical; Scene hide/unhide and skip preserved")
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read())
                raise
            finally:
                sock.close()
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    main()
