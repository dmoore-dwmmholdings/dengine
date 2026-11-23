#!/usr/bin/env python3
"""DENG-817 real-service check of the Scene render modes on a 3D mesh (macOS, Metal, msgpack).

Build `mesh_scene`, then run with Python containing msgpack:
  python check_mesh_scene_modes.py build/dev/examples/mesh_scene/mesh_scene/mesh_scene
Starts the game with --editor-service on a private endpoint, loads mesh_scene.scene, creates a
Scene view and reads back the shared frame per render mode. Wireframe, uv0, uv1, meshletIds and normals must be
non-black and differ from Lit by a real number of pixels (the mesh, not just the background).
An explicit native integration check, not a mocked renderer.
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

try:
    import msgpack
except ImportError:
    print("check_mesh_scene_modes: SKIPPED (msgpack not installed)")
    sys.exit(77)

W, H = 320, 240
MIN_PIXELS = 200  # mesh coverage is thousands of pixels; this rejects background-only output


def lit_pixels(frame):
    """Pixels with any colour channel set (alpha is byte 3 in both BGRA and RGBA)."""
    return sum(1 for i in range(0, len(frame), 4) if frame[i] | frame[i + 1] | frame[i + 2])


def differing(a, b):
    return sum(1 for i in range(0, min(len(a), len(b)), 4) if a[i:i + 3] != b[i:i + 3])


def main():
    if not __debug__:
        sys.exit("Run without -O: this check requires assertions")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    args = parser.parse_args()
    game = args.game.resolve()
    if not game.exists():
        print("check_mesh_scene_modes: SKIPPED (mesh_scene is not built)")
        sys.exit(77)
    with tempfile.TemporaryDirectory(prefix="deng817-", dir="/tmp") as tmp:
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
                    body = msgpack.packb(dict(id=request_id, method=method, params=params),
                                         use_bin_type=True)
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
                scene_path = Path(__file__).resolve().parents[3] / "examples/mesh_scene/mesh_scene.scene"
                call("scene.load", path=str(scene_path), additive=False)
                camera = dict(position=[1., -7., 2.], rotation=[0., 0., 0., 1.],
                              fovY=.8, near=.1, far=1000.)
                view = call("view.create", width=W, height=H, scale=1., camera=camera,
                            renderMode="lit", showFlags=[], game=False)

                def frame():
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

                # Mesh assets load asynchronously: wait until the first mesh pixels appear.
                lit = frame()
                for _ in range(40):
                    if lit_pixels(lit) >= MIN_PIXELS:
                        break
                    lit = frame()
                assert lit_pixels(lit) >= MIN_PIXELS, "Lit mesh scene is black"
                results = {}
                for mode in ("wireframe", "uv0", "uv1", "meshletIds", "normals"):
                    call("view.setRenderMode", view=view["view"], renderMode=mode)
                    img = frame()
                    nonblack, diff = lit_pixels(img), differing(lit, img)
                    results[mode] = (nonblack, diff)
                    assert nonblack >= MIN_PIXELS, (mode, "is black on the mesh", nonblack)
                    assert diff >= MIN_PIXELS, (mode, "does not differ from Lit", diff)
                call("view.destroy", view=view["view"])

                # DENG-818: orthoHeight <= 0 keeps the perspective camera (fovY > 0), and a
                # positive orthoHeight renders the meshes orthographically, not black.
                def mesh_lit(cam, name):
                    v = call("view.create", width=W, height=H, scale=1., camera=cam,
                             renderMode="lit", showFlags=[], game=False)
                    nonlocal view
                    view = v
                    img = frame()
                    for _ in range(40):
                        if lit_pixels(img) >= MIN_PIXELS:
                            break
                        img = frame()
                    call("view.destroy", view=v["view"])
                    assert lit_pixels(img) >= MIN_PIXELS, (name, "is black on the mesh")
                    return img

                persp = mesh_lit(camera, "perspective")
                zero = mesh_lit(dict(camera, orthoHeight=0.), "orthoHeight 0")
                assert differing(persp, zero) < MIN_PIXELS, "orthoHeight 0 did not stay perspective"
                ortho = mesh_lit(dict(camera, orthoHeight=8.), "orthoHeight 8")
                assert differing(persp, ortho) >= MIN_PIXELS, "orthoHeight 8 is not orthographic"
                print("PASS: mesh_scene Scene modes differ from Lit and are non-black "
                      "(non-black px, differing px):", results)
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
