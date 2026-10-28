#!/usr/bin/env python3
"""Writes the seed corpora of tests/fuzz/tilemap/ (DENG-80) from tests/data/tilemap/.

The fuzz targets cannot read external files, so the Tiled seeds embed their tileset and the LDtk
seed carries its levels inline. Run after tests/data/tilemap/generate.py.
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.normpath(os.path.join(HERE, "..", "..", "data", "tilemap"))
NL = chr(10)


def read(rel):
    with open(os.path.join(DATA, rel), encoding="utf-8") as f:
        return f.read()


def write(target, name, text):
    d = os.path.join(HERE, "corpus", target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "w", encoding="utf-8", newline=NL) as f:
        f.write(text)


def embed_tmj(text):
    m = json.loads(text)
    ts = json.loads(read("tiles/terrain.tsj"))
    ts["firstgid"] = 1
    ts["image"] = "../tiles/terrain.png"
    m["tilesets"] = [ts]
    return json.dumps(m, separators=(",", ":")) + NL


def embed_tmx(text):
    tsx = read("tiles/terrain.tsx")
    body = tsx[tsx.index("<tileset"):]
    body = body.replace("<tileset ", '<tileset firstgid="1" ', 1)
    body = body.replace('source="terrain.png"', 'source="../tiles/terrain.png"')
    return re.sub(r'<tileset firstgid="1" source="[^"]*"/>', body.strip(), text)


def inline_ldtk(text):
    p = json.loads(text)
    levels = []
    for lv in p["levels"]:
        full = json.loads(read("maps/" + lv["externalRelPath"]))
        full["externalRelPath"] = None
        levels.append(full)
    p["levels"] = levels
    p["externalLevels"] = False
    return json.dumps(p, separators=(",", ":")) + NL


def main():
    for name in ("fixture.tmj", "enc_csv.tmj", "enc_gzip.tmj", "enc_lz4.tmj"):
        write("tiled", name, embed_tmj(read("maps/" + name)))
    for name in ("fixture.tmx", "enc_plain.tmx", "enc_xml.tmx", "enc_zstd.tmx"):
        write("tiled", name, embed_tmx(read("maps/" + name)))
    write("ldtk", "fixture.ldtk", inline_ldtk(read("maps/fixture.ldtk")))
    write("tilemap_json", "fixture.tmj", embed_tmj(read("maps/fixture.tmj")))
    write("tilemap_json", "terrain.tsj", read("tiles/terrain.tsj"))
    write("tilemap_json", "level0.ldtkl", read("maps/fixture/Level_0.ldtkl"))
    write("tilemap_json", "escapes.json",
          '{"s":"a\\u00e9\\ud83d\\ude00\\n","n":[0,-1.5e3,1E+2],"o":{},"a":[],"b":[true,false,null]}' + NL)
    write("tilemap_xml", "fixture.tmx", embed_tmx(read("maps/fixture.tmx")))
    write("tilemap_xml", "terrain.tsx", read("tiles/terrain.tsx"))
    write("tilemap_xml", "entities.xml",
          "<?xml version='1.0'?><!DOCTYPE m [<!ENTITY x 'y'>]><!-- c -->"
          "<m a='&lt;&#65;&#x42;'><d><![CDATA[1,2]]>3</d><e/></m>" + NL)


if __name__ == "__main__":
    main()
