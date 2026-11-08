#!/usr/bin/env python3
"""DENG-663 bounded resize regression (macOS, Metal, MessagePack dependency).

Build `flappy`, then run with Python containing msgpack:
  python check_view_resize.py build/dev/examples/flappy/flappy/flappy
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
    with tempfile.TemporaryDirectory(prefix="deng663-", dir="/tmp") as tmp:
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

                def call(method, expected_error=None, **params):
                    nonlocal request_id
                    request_id += 1
                    body = msgpack.packb(dict(id=request_id, method=method, params=params), use_bin_type=True)
                    sock.sendall(struct.pack("<I", len(body) + 1) + b"\x01" + body)
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        kind, response = receive()
                        if kind == 2 and response["id"] == request_id:
                            if expected_error is not None:
                                assert response.get("error", {}).get("code") == expected_error, (method, response)
                                return response["error"]
                            assert "error" not in response, (method, response)
                            return response["result"]
                    raise TimeoutError(method)

                hello = call("hello", protocol="1.8", token=token, capabilities=[])
                assert "views.sharedTexture" in hello["capabilities"]
                scene_path = Path(__file__).resolve().parents[3] / "examples/flappy/flappy.scene"
                call("scene.load", path=str(scene_path), additive=False)
                camera = dict(position=[0., -12., 6.25], rotation=[0., 0., 0., 1.],
                              fovY=0., orthoHeight=12.5, near=.1, far=1000.)
                views = [call("view.create", width=320, height=240, scale=1., camera=camera,
                              renderMode="lit", showFlags=[], game=is_game)
                         for is_game in (False, True)]

                def frame(view):
                    # Several new notifications cover producer/readback latency. Verify the
                    # same generation, mapped dimensions and coherent live frame contents.
                    count = 0
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        kind, note = receive()
                        if kind != 3 or note.get("topic") != "view.frame":
                            continue
                        data = note["data"]
                        if data["view"] != view["view"]:
                            continue
                        assert data["generation"] == view["generation"], data
                        count += 1
                        if count < 8:
                            continue
                        path = Path(view["surfaces"][data["surface"]]["handle"])
                        raw = path.read_bytes()
                        fence, width, height, pitch, magic = struct.unpack_from("=QIIII", raw)
                        with path.open("rb") as stream:
                            final_fence, = struct.unpack("=Q", stream.read(8))
                        if fence == data["fenceValue"] == final_fence:
                            assert magic == 0x44455631
                            assert pitch == width * 4 and len(raw) == 64 + pitch * height
                            return data["frame"], (width, height)
                    raise TimeoutError("stable shared frame")

                for view in views:
                    previous_frame, dimensions = frame(view)
                    # Each old-code target has only 8193 pixels (32 KiB RGBA8), never a huge
                    # square or UINT_MAX extent. The old handler returns internal instead
                    # of invalid_params; fail immediately before consuming damaged frames.
                    for width, height in ((8193, 1), (1, 8193)):
                        call("view.resize", view=view["view"], width=width, height=height,
                             scale=2., expected_error=2)
                        current_frame, current_dimensions = frame(view)
                        assert current_frame > previous_frame
                        assert current_dimensions == dimensions
                        previous_frame = current_frame
                    print("retained view/ring generation", view["view"], view["generation"], dimensions)

                scene = views[0]
                # Existing valid resize replaces the ring; zero still normalizes to one.
                for width, height in ((400, 256), (0, 0), (320, 240)):
                    resized = call("view.resize", view=scene["view"], width=width, height=height, scale=1.)
                    assert resized["generation"] > scene["generation"]
                    scene = dict(resized, view=scene["view"])
                    _, dimensions = frame(scene)
                    assert dimensions == (max(width, 1), max(height, 1)), dimensions
                for view in views:
                    call("view.destroy", view=view["view"])
                print("PASS: invalid per-axis resize preserves live Scene/Game rings; valid resize and zero normalization retained")
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
