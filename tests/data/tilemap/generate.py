#!/usr/bin/env python3
"""Writes the tilemap fixtures (DENG-80). Own-made, CC0 (see LICENSE.md).

  tiles/terrain.png            64x32 tileset, 8 tiles of 16x16 (pixel art drawn here)
  tiles/terrain.tsj / .tsx     the tileset for Tiled (animation on tile 1, collision on 3, 6, 7)
  maps/fixture.tmj / .tmx      48x40 Tiled map: `back` base64+zstd, `ground` CSV (collision),
                               `deco` base64+zlib, an object layer; the two files are equivalent
  maps/enc_*.tmj / .tmx        a 6x4 map in every other encoding (gzip, plain base64, TMX <tile>,
                               and an unknown compression for the error test)
  maps/fixture.ldtk            LDtk 1.5 project, 2 external levels (maps/fixture/Level_*.ldtkl)
                               with an IntGrid + auto-layer, a tile layer and entities
  examples/scenes/tilemap/     a 16x9 two-layer sample map with the tileset, used by the golden
                               image tilemap_lit_two_layers

Run from anywhere: python tests/data/tilemap/generate.py. Needs the `zstd` command for the zstd
layers. Output is deterministic.
"""
import base64
import gzip
import io
import json
import os
import struct
import subprocess
import zlib

ROOT = os.path.dirname(os.path.abspath(__file__))
T = 16  # tile size in pixels
NL = chr(10)


def write(rel, data):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = "wb" if isinstance(data, bytes) else "w"
    if mode == "wb":
        with open(path, mode) as f:
            f.write(data)
    else:
        with open(path, mode, encoding="utf-8", newline="\n") as f:
            f.write(data)


# ---- Tileset image --------------------------------------------------------------------------


def png(width, height, rgba):
    def chunk(kind, body):
        c = struct.pack(">I", len(body)) + kind + body
        return c + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def tile_pixel(tile, x, y):
    grass, dirt, dark = (86, 168, 62), (128, 86, 52), (96, 62, 38)
    if tile == 0:  # grass on dirt
        if y < 4:
            return grass + (255,) if (x + y) % 5 else (120, 200, 90, 255)
        return (dirt if (x * 7 + y * 3) % 11 else dark) + (255,)
    if tile in (1, 2):  # water, two frames of a moving wave line
        row = 4 if tile == 1 else 9
        if y == row or (y == row + 1 and x % 4 == (0 if tile == 1 else 2)):
            return (170, 210, 250, 230)
        return (40, 90, 200, 220)
    if tile == 3:  # slope: lower-right half filled
        if x + y >= 15:
            return (grass if x + y < 18 else dirt) + (255,)
        return (0, 0, 0, 0)
    if tile == 4:  # dirt
        return (dirt if (x * 5 + y * 9) % 13 else dark) + (255,)
    if tile == 5:  # flower, alpha-tested
        if 6 <= x <= 9 and 3 <= y <= 6:
            return (240, 80, 120, 255) if (x, y) not in ((7, 4), (8, 5)) else (250, 220, 60, 255)
        if x == 8 and 7 <= y <= 15:
            return (60, 150, 60, 255)
        if y >= 12 and abs(x - 8) <= 15 - y:
            return (70, 160, 70, 255)
        return (0, 0, 0, 0)
    if tile == 6:  # bricks
        if y % 4 == 3 or (x + (4 if (y // 4) % 2 else 0)) % 8 == 7:
            return (90, 40, 30, 255)
        return (176, 72, 50, 255)
    # tile 7: stone platform, wavy top
    top = 3 + (1 if x % 4 in (1, 2) else 0)
    if y < top:
        return (0, 0, 0, 0)
    return (150, 150, 160, 255) if (x + y) % 6 else (110, 110, 125, 255)


def tileset_png():
    w, h = 4 * T, 2 * T
    px = []
    for y in range(h):
        for x in range(w):
            tile = (y // T) * 4 + x // T
            px.extend(tile_pixel(tile, x % T, y % T))
    return png(w, h, px)


# ---- Tileset descriptions ------------------------------------------------------------------

# Tile 3: the slope triangle. Tile 6: a full-tile rectangle (solid). Tile 7: a 12-point polyline
# along its wavy top (split into chains of at most 8 points by the importer).
SLOPE = [(0, 16), (16, 0), (16, 16)]
WAVE = [(i * 1.25, 3 + (1 if i % 2 else 0)) for i in range(12)]


def tsj():
    tiles = [
        {"id": 1, "animation": [{"tileid": 1, "duration": 200}, {"tileid": 2, "duration": 300}]},
        {"id": 3, "objectgroup": {"draworder": "index", "name": "", "objects": [
            {"id": 1, "name": "", "type": "", "x": 0, "y": 0, "width": 0, "height": 0,
             "rotation": 0, "visible": True,
             "polygon": [{"x": px, "y": py} for (px, py) in SLOPE]}],
            "opacity": 1, "type": "objectgroup", "visible": True, "x": 0, "y": 0}},
        {"id": 6, "objectgroup": {"draworder": "index", "name": "", "objects": [
            {"id": 1, "name": "", "type": "", "x": 0, "y": 0, "width": 16, "height": 16,
             "rotation": 0, "visible": True}],
            "opacity": 1, "type": "objectgroup", "visible": True, "x": 0, "y": 0}},
        {"id": 7, "objectgroup": {"draworder": "index", "name": "", "objects": [
            {"id": 1, "name": "", "type": "", "x": 0, "y": 0, "width": 0, "height": 0,
             "rotation": 0, "visible": True,
             "polyline": [{"x": px, "y": py} for (px, py) in WAVE]}],
            "opacity": 1, "type": "objectgroup", "visible": True, "x": 0, "y": 0}},
    ]
    return {"columns": 4, "image": "terrain.png", "imageheight": 32, "imagewidth": 64,
            "margin": 0, "name": "terrain", "spacing": 0, "tilecount": 8,
            "tiledversion": "1.10.2", "tileheight": 16, "tiles": tiles, "tilewidth": 16,
            "type": "tileset", "version": "1.10"}


def fmt(v):
    return ("%.6f" % v).rstrip("0").rstrip(".") if isinstance(v, float) else str(v)


def tsx():
    pts = lambda pl: " ".join("%s,%s" % (fmt(float(x)), fmt(float(y))) for x, y in pl)
    return ("""<?xml version="1.0" encoding="UTF-8"?>
<tileset version="1.10" tiledversion="1.10.2" name="terrain" tilewidth="16" tileheight="16" tilecount="8" columns="4">
 <image source="terrain.png" width="64" height="32"/>
 <tile id="1">
  <animation>
   <frame tileid="1" duration="200"/>
   <frame tileid="2" duration="300"/>
  </animation>
 </tile>
 <tile id="3">
  <objectgroup draworder="index" id="2">
   <object id="1" x="0" y="0">
    <polygon points="%s"/>
   </object>
  </objectgroup>
 </tile>
 <tile id="6">
  <objectgroup draworder="index" id="2">
   <object id="1" x="0" y="0" width="16" height="16"/>
  </objectgroup>
 </tile>
 <tile id="7">
  <objectgroup draworder="index" id="2">
   <object id="1" x="0" y="0">
    <polyline points="%s"/>
   </object>
  </objectgroup>
 </tile>
</tileset>
""" % (pts(SLOPE), pts(WAVE)))


# ---- The fixture map -----------------------------------------------------------------------

W, H = 48, 40
FH, FV, FD = 0x80000000, 0x40000000, 0x20000000


def gid(local, flags=0):
    return (local + 1) | flags if local is not None else 0


def fixture_layers():
    back = [0] * (W * H)
    ground = [0] * (W * H)
    deco = [0] * (W * H)
    for x in range(W):
        for y in range(34, H):
            ground[y * W + x] = gid(0 if y == 34 else 4)
    # A water pool (animated) at x 10..15, rows 34..35.
    for x in range(10, 16):
        ground[34 * W + x] = gid(1)
        ground[35 * W + x] = gid(1)
    # Slopes, one mirrored, crossing the chunk border at x = 32.
    ground[33 * W + 20] = gid(3)
    ground[33 * W + 31] = gid(3, FH)
    ground[33 * W + 33] = gid(3, FD | FV)
    # A brick wall in chunk (0, 0) and across the chunk row border (rows 28..33).
    for y in range(28, 34):
        ground[y * W + 40] = gid(6)
        ground[y * W + 41] = gid(6)
    # Platforms (tile shapes: chains) at row 22.
    for x in range(4, 9):
        ground[22 * W + x] = gid(7)
    # Background flowers (layer `back`) and decoration on top (layer `deco`).
    for x in range(0, W, 3):
        back[33 * W + x] = gid(5)
    for x in range(2, W, 7):
        deco[33 * W + x] = gid(5, FH)
    deco[5 * W + 5] = gid(1)  # an animated tile in chunk (0, 0)
    return back, ground, deco


def b64(data, compression):
    raw = struct.pack("<%dI" % len(data), *data)
    if compression == "zlib":
        raw = zlib.compress(raw, 9)
    elif compression == "gzip":
        buf = io.BytesIO()
        with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as g:
            g.write(raw)
        raw = buf.getvalue()
    elif compression == "zstd":
        raw = subprocess.run(["zstd", "-19", "-q", "-c", "--no-check"], input=raw,
                             check=True, capture_output=True).stdout
    elif compression == "lz4":
        raw = b"not really lz4"
    return base64.b64encode(raw).decode("ascii")


OBJECTS = [
    {"id": 1, "name": "hero", "type": "Player", "x": 40, "y": 520, "width": 16, "height": 24,
     "props": [("TmHealth.current", "float", 50.0), ("TmHealth.max", "float", 120.0),
               ("TmHealth.lives", "int", 3), ("TmHealth.tag", "string", "hero")]},
    {"id": 2, "name": "coin", "type": "Coin", "x": 300, "y": 500, "width": 8, "height": 8,
     "props": []},
]


def tmj_props(props):
    return [{"name": n, "type": t, "value": v} for (n, t, v) in props]


def tmj_map(layers, objects, width, height, tileset_source="../tiles/terrain.tsj"):
    out_layers = []
    lid = 1
    for (name, data, enc, comp, props) in layers:
        l = {"id": lid, "name": name, "type": "tilelayer", "width": width, "height": height,
             "x": 0, "y": 0, "opacity": 1, "visible": True}
        if enc == "csv":
            l["data"] = data
        else:
            l["data"] = b64(data, comp)
            l["encoding"] = "base64"
            if comp:
                l["compression"] = comp
        if props:
            l["properties"] = [{"name": n, "type": t, "value": v} for (n, t, v) in props]
        out_layers.append(l)
        lid += 1
    if objects is not None:
        objs = []
        for o in objects:
            j = {"id": o["id"], "name": o["name"], "type": o["type"], "x": o["x"], "y": o["y"],
                 "width": o["width"], "height": o["height"], "rotation": 0, "visible": True}
            if o["props"]:
                j["properties"] = tmj_props(o["props"])
            objs.append(j)
        out_layers.append({"id": lid, "name": "objects", "type": "objectgroup",
                           "draworder": "topdown", "objects": objs, "opacity": 1,
                           "visible": True, "x": 0, "y": 0})
    return {"compressionlevel": -1, "height": height, "infinite": False, "layers": out_layers,
            "nextlayerid": lid + 1, "nextobjectid": 3, "orientation": "orthogonal",
            "renderorder": "right-down", "tiledversion": "1.10.2", "tileheight": T,
            "tilesets": [{"firstgid": 1, "source": tileset_source}], "tilewidth": T,
            "type": "map", "version": "1.10", "width": width}


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def tmx_map(layers, objects, width, height, tileset_source="../tiles/terrain.tsx"):
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<map version="1.10" tiledversion="1.10.2" orientation="orthogonal" '
           'renderorder="right-down" width="%d" height="%d" tilewidth="16" tileheight="16" '
           'infinite="0" nextlayerid="%d" nextobjectid="3">' % (width, height, len(layers) + 2),
           ' <tileset firstgid="1" source="%s"/>' % tileset_source]
    lid = 1
    for (name, data, enc, comp, props) in layers:
        out.append(' <layer id="%d" name="%s" width="%d" height="%d">' % (lid, esc(name), width,
                                                                          height))
        if props:
            out.append('  <properties>')
            for (n, t, v) in props:
                out.append('   <property name="%s" type="%s" value="%s"/>' %
                           (esc(n), t, ("true" if v else "false") if t == "bool" else esc(str(v))))
            out.append('  </properties>')
        if enc == "csv":
            rows = [",".join(str(g) for g in data[y * width:(y + 1) * width]) for y in range(height)]
            out.append('  <data encoding="csv">\n' + ",\n".join(rows) + '\n</data>')
        elif enc == "xml":
            out.append('  <data>')
            for g in data:
                out.append('   <tile gid="%d"/>' % g if g else '   <tile/>')
            out.append('  </data>')
        else:
            attr = ' compression="%s"' % comp if comp else ""
            out.append('  <data encoding="base64"%s>\n   %s\n  </data>' % (attr, b64(data, comp)))
        out.append(' </layer>')
        lid += 1
    if objects is not None:
        out.append(' <objectgroup id="%d" name="objects">' % lid)
        for o in objects:
            out.append('  <object id="%d" name="%s" type="%s" x="%s" y="%s" width="%s" '
                       'height="%s">' % (o["id"], esc(o["name"]), esc(o["type"]), fmt(o["x"]),
                                         fmt(o["y"]), fmt(o["width"]), fmt(o["height"])))
            if o["props"]:
                out.append('   <properties>')
                for (n, t, v) in o["props"]:
                    out.append('    <property name="%s" type="%s" value="%s"/>' %
                               (esc(n), t, esc(fmt(v) if not isinstance(v, str) else v)))
                out.append('   </properties>')
            out.append('  </object>')
        out.append(' </objectgroup>')
    out.append('</map>')
    return "\n".join(out) + "\n"


# ---- LDtk ----------------------------------------------------------------------------------

LW, LH = 24, 16  # cells per level
TILESET_UID = 1


def ldtk_level(index, world_x):
    ident = "Level_%d" % index
    intgrid = [0] * (LW * LH)
    for x in range(LW):
        intgrid[(LH - 1) * LW + x] = 1
        intgrid[(LH - 2) * LW + x] = 1
    for y in range(8, LH - 2):  # a wall column
        intgrid[y * LW + (5 if index == 0 else 18)] = 1
    auto = []
    for y in range(LH):
        for x in range(LW):
            if intgrid[y * LW + x]:
                top = y == 0 or not intgrid[(y - 1) * LW + x]
                t = 0 if top else 4
                auto.append({"px": [x * T, y * T], "src": [(t % 4) * T, (t // 4) * T], "f": 0,
                             "t": t, "d": [1, y * LW + x], "a": 1})
    decor = []
    for x in range(1, LW, 4):
        f = 1 if x % 8 == 1 else 0
        decor.append({"px": [x * T, (LH - 3) * T], "src": [16, 16], "f": f, "t": 5,
                      "d": [(LH - 3) * LW + x], "a": 1})
    if index == 1:
        decor.append({"px": [10 * T, 4 * T], "src": [32, 16], "f": 2, "t": 6,
                      "d": [4 * LW + 10], "a": 1})
    entities = []
    if index == 0:
        entities.append({
            "__identifier": "Player", "__grid": [2, LH - 3], "__pivot": [0.5, 1],
            "__tags": [], "__tile": None, "__smartColor": "#BE4A2F",
            "__worldX": world_x + 40, "__worldY": (LH - 2) * T, "iid": "ent-player-0",
            "width": 16, "height": 24, "defUid": 20, "px": [40, (LH - 2) * T],
            "fieldInstances": [
                {"__identifier": "TmHealth__current", "__type": "Float", "__value": 75.0,
                 "__tile": None, "defUid": 21, "realEditorValues": []},
                {"__identifier": "TmHealth__lives", "__type": "Int", "__value": 2,
                 "__tile": None, "defUid": 22, "realEditorValues": []},
                {"__identifier": "TmHealth__tag", "__type": "String", "__value": "ldtk",
                 "__tile": None, "defUid": 23, "realEditorValues": []},
                {"__identifier": "TmHealth__max", "__type": "Float", "__value": None,
                 "__tile": None, "defUid": 24, "realEditorValues": []}]})
    else:
        entities.append({
            "__identifier": "Coin", "__grid": [12, LH - 4], "__pivot": [0.5, 0.5], "__tags": [],
            "__tile": None, "__smartColor": "#E8C547", "__worldX": world_x + 200,
            "__worldY": (LH - 4) * T + 8, "iid": "ent-coin-1", "width": 8, "height": 8,
            "defUid": 30, "px": [200, (LH - 4) * T + 8], "fieldInstances": []})

    def layer(ident_l, type_, uid, **kw):
        base = {"__identifier": ident_l, "__type": type_, "__cWid": LW, "__cHei": LH,
                "__gridSize": T, "__opacity": 1, "__pxTotalOffsetX": 0, "__pxTotalOffsetY": 0,
                "__tilesetDefUid": None, "__tilesetRelPath": None, "iid": "%s-%s" % (ident, ident_l),
                "levelId": 100 + index, "layerDefUid": uid, "pxOffsetX": 0, "pxOffsetY": 0,
                "visible": True, "optionalRules": [], "intGridCsv": [], "autoLayerTiles": [],
                "seed": 1234, "overrideTilesetUid": None, "gridTiles": [], "entityInstances": []}
        base.update(kw)
        return base

    layers = [
        layer("Entities", "Entities", 10, entityInstances=entities),
        layer("Decor", "Tiles", 11, __tilesetDefUid=TILESET_UID,
              __tilesetRelPath="../tiles/terrain.png", gridTiles=decor),
        layer("Walls", "IntGrid", 12, __tilesetDefUid=TILESET_UID,
              __tilesetRelPath="../tiles/terrain.png", intGridCsv=intgrid, autoLayerTiles=auto),
    ]
    return {"identifier": ident, "iid": "level-%d" % index, "uid": 100 + index,
            "worldX": world_x, "worldY": 0, "worldDepth": 0, "pxWid": LW * T, "pxHei": LH * T,
            "__bgColor": "#40465B", "bgColor": None, "useAutoIdentifier": True, "bgRelPath": None,
            "bgPos": None, "bgPivotX": 0.5, "bgPivotY": 0.5, "__smartColor": "#ADADB5",
            "__bgPos": None, "externalRelPath": None, "fieldInstances": [],
            "layerInstances": layers, "__neighbours": []}


def ldtk_project():
    levels = []
    for i in range(2):
        lv = ldtk_level(i, i * LW * T)
        rel = "fixture/Level_%d.ldtkl" % i
        write("maps/" + rel, json.dumps(lv, indent=1) + "\n")
        stub = dict(lv)
        stub["layerInstances"] = None
        stub["externalRelPath"] = rel
        levels.append(stub)
    tileset = {"__cWid": 4, "__cHei": 2, "identifier": "Terrain", "uid": TILESET_UID,
               "relPath": "../tiles/terrain.png", "embedAtlas": None, "pxWid": 64, "pxHei": 32,
               "tileGridSize": 16, "spacing": 0, "padding": 0, "tags": [], "tagsSourceEnumUid": None,
               "enumTags": [], "customData": [], "savedSelections": [], "cachedPixelData": None}
    defs = {"layers": [], "entities": [], "tilesets": [tileset], "enums": [],
            "externalEnums": [], "levelFields": []}
    return {"__header__": {"fileType": "LDtk Project JSON", "app": "LDtk",
                           "doc": "https://ldtk.io/json", "schema": "https://ldtk.io/files/JSON_SCHEMA.json",
                           "appAuthor": "Sebastien 'deepnight' Benard", "appVersion": "1.5.3",
                           "url": "https://ldtk.io"},
            "iid": "fixture-project", "jsonVersion": "1.5.3", "appBuildId": 473703,
            "nextUid": 200, "identifierStyle": "Capitalize", "toc": [],
            "worldLayout": "Free", "worldGridWidth": 256, "worldGridHeight": 256,
            "defaultLevelWidth": LW * T, "defaultLevelHeight": LH * T, "defaultPivotX": 0,
            "defaultPivotY": 0, "defaultGridSize": T, "defaultEntityWidth": 16,
            "defaultEntityHeight": 16, "bgColor": "#40465B", "defaultLevelBgColor": "#696A79",
            "minifyJson": False, "externalLevels": True, "exportTiled": False,
            "simplifiedExport": False, "imageExportMode": "None", "exportLevelBg": True,
            "pngFilePattern": None, "backupOnSave": False, "backupLimit": 10,
            "backupRelPath": None, "levelNamePattern": "Level_%idx", "tutorialDesc": None,
            "customCommands": [], "flags": [], "defs": defs, "levels": levels, "worlds": [],
            "dummyWorldIid": "dummy-world"}


# ---- Main ----------------------------------------------------------------------------------


def main():
    write("tiles/terrain.png", tileset_png())
    write("tiles/terrain.tsj", json.dumps(tsj(), indent=1) + "\n")
    write("tiles/terrain.tsx", tsx())

    back, ground, deco = fixture_layers()
    collision = [("collision", "bool", True)]
    layers = [("back", back, "base64", "zstd", []),
              ("ground", ground, "csv", "", collision),
              ("deco", deco, "base64", "zlib", [])]
    write("maps/fixture.tmj", json.dumps(tmj_map(layers, OBJECTS, W, H), indent=1) + "\n")
    write("maps/fixture.tmx", tmx_map(layers, OBJECTS, W, H))

    # Encodings: one 6x4 layer (with flips) in every other form.
    small = [gid(0), gid(4), 0, gid(6, FH), gid(3, FV), gid(5),
             0, gid(1), gid(2, FD), 0, gid(7), gid(4),
             gid(0, FH | FV), 0, 0, gid(5), gid(6), 0,
             gid(4), gid(4), gid(4), gid(4), gid(4), gid(4)]
    write("maps/enc_csv.tmj",
          json.dumps(tmj_map([("l", small, "csv", "", [])], None, 6, 4), indent=1) + "\n")
    write("maps/enc_gzip.tmj",
          json.dumps(tmj_map([("l", small, "base64", "gzip", [])], None, 6, 4), indent=1) + "\n")
    write("maps/enc_plain.tmx", tmx_map([("l", small, "base64", "", [])], None, 6, 4))
    write("maps/enc_xml.tmx", tmx_map([("l", small, "xml", "", [])], None, 6, 4))
    write("maps/enc_zstd.tmx", tmx_map([("l", small, "base64", "zstd", [])], None, 6, 4))
    write("maps/enc_lz4.tmj",
          json.dumps(tmj_map([("broken_layer", small, "base64", "lz4", [])], None, 6, 4),
                     indent=1) + "\n")

    write("maps/fixture.ldtk", json.dumps(ldtk_project(), indent=1) + "\n")

    # Golden: 16x9, two layers, lit.
    gw, gh = 16, 9
    gback = [0] * (gw * gh)
    gground = [0] * (gw * gh)
    for x in range(gw):
        gground[7 * gw + x] = gid(0)
        gground[8 * gw + x] = gid(4)
    for x in range(5, 8):
        gground[7 * gw + x] = gid(1)
    for y in range(3, 7):
        gground[y * gw + 12] = gid(6)
    gground[6 * gw + 3] = gid(3)
    gground[6 * gw + 9] = gid(3, FH)
    for x in range(1, gw, 2):
        gback[6 * gw + x] = gid(5)
    for x in range(2, 6):
        gback[3 * gw + x] = gid(7)
    level = json.dumps(tmj_map(
        [("back", gback, "csv", "", []), ("ground", gground, "csv", "", collision)], None, gw, gh),
        indent=1) + NL
    # examples/scenes/tilemap/: the sample scene (also the golden image's assets).
    ex = os.path.join(ROOT, "..", "..", "..", "examples", "scenes", "tilemap")
    for rel, data in (("maps/level.tmj", level), ("tiles/terrain.png", tileset_png()),
                      ("tiles/terrain.tsj", json.dumps(tsj(), indent=1) + NL)):
        path = os.path.normpath(os.path.join(ex, rel))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if isinstance(data, bytes):
            with open(path, "wb") as f:
                f.write(data)
        else:
            with open(path, "w", encoding="utf-8", newline=NL) as f:
                f.write(data)

if __name__ == "__main__":
    main()
