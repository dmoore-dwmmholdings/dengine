#!/usr/bin/env python3
"""Writes the hand-made seeds of fuzz_shader_view (DENG-409): small cooked-shader blobs (DSHD v1,
src/gpu/shader/shader_format.h) at the edges of the format, valid and refused.

    python3 tests/fuzz/gpu/make_shader_view_corpus.py

Output: tests/fuzz/gpu/corpus/shader_view/*.dshd (the hand-made names below). The cooked seeds
(`cooked_*.dshd`) are real cooks, taken from packs with `de-pack extract`:
`de-cook cook tests/unit/cook/data/builtin_project/assets.toml --platform macos` (and `linux`),
then `de-pack extract <pak> "shaders/*"`. Running this script twice gives the same bytes.
"""
import os
import struct
import sys

MAGIC = 0x44485344
def blob(entries, perms, records, version=1, reserved=0, codes=None, strings_override=None, nul=True):
    # entries: [(name, stage)], perms: [defines], records: [(target, perm, entry, stage, codebytes, tg, pc, flags, mesh)]
    strings = b''
    offs = {}
    def add(s):
        nonlocal strings
        if s not in offs:
            offs[s] = len(strings)
            strings += s.encode() + b'\0'
        return offs[s]
    ent = [(add(n), st) for n, st in entries]
    per = [add(d) for d in perms]
    if not nul:
        strings = strings[:-1] + b'X'
    n_e, n_p, n_r = len(entries), len(perms), len(records)
    tables = 32 + 8*n_e + 4*n_p + 48*n_r
    so = tables
    code_start = (so + len(strings) + 15) & ~15
    body = b''
    recs = b''
    pos = code_start
    code_blob = b''
    for (t, p, e, st, code, tg, pc, fl, mesh) in records:
        while (pos % 16): pos += 1; code_blob += b'\0'
        off = pos
        code_blob += code
        pos += len(code)
        recs += struct.pack('<12I', t, p, e, st, off, len(code), tg[0], tg[1], tg[2], pc, fl, mesh)
    hdr = struct.pack('<8I', MAGIC, version, n_e, n_p, n_r, so, len(strings), reserved)
    data = hdr + b''.join(struct.pack('<2I', o, s) for o, s in ent) + b''.join(struct.pack('<I', o) for o in per) + recs + strings
    data += b'\0' * (code_start - len(data))
    data += code_blob
    return data
spirv = struct.pack('<I', 0x07230203) + b'\x00\x06\x01\x00' + b'\x11' * 8
mtl = b'MTLB' + b'\x22' * 28
seeds = {
  'min_one_record': blob([('cs_main', 3)], [''], [(1, 0, 0, 3, spirv, (8, 4, 1), 16, 9, 0)]),
  'no_records': blob([('vs_main', 1)], [''], []),
  'two_entries_two_perms': blob([('vs_main', 1), ('ps_main', 2)], ['', 'A=1;B'],
      [(1, 0, 0, 1, spirv, (0, 0, 0), 32, 8, 0), (1, 0, 1, 2, spirv, (0, 0, 0), 32, 0, 0),
       (1, 1, 0, 1, spirv, (0, 0, 0), 32, 8, 0), (1, 1, 1, 2, spirv, (0, 0, 0), 0, 0, 0),
       (3, 0, 0, 1, mtl, (0, 0, 0), 32, 8, 0), (3, 1, 1, 2, mtl, (0, 0, 0), 0, 0, 0)]),
  'mesh_limits': blob([('ms_main', 4), ('as_main', 5)], [''],
      [(1, 0, 0, 4, spirv, (32, 1, 1), 4, 15, (124 << 16) | 64), (2, 0, 1, 5, b'DXBC' + b'\x33' * 12, (32, 1, 1), 128, 0, 0)]),
  'empty_code': blob([('cs_main', 3)], [''], [(1, 0, 0, 3, b'', (1, 1, 1), 0, 0, 0)]),
  'bad_version': blob([('cs_main', 3)], [''], [(1, 0, 0, 3, spirv, (1, 1, 1), 0, 0, 0)], version=2),
  'bad_reserved': blob([('cs_main', 3)], [''], [], reserved=1),
  'strings_no_nul': blob([('cs_main', 3)], [''], [], nul=False),
  'unsorted_records': blob([('a', 3), ('b', 3)], [''],
      [(1, 0, 1, 3, spirv, (1, 1, 1), 0, 0, 0), (1, 0, 0, 3, spirv, (1, 1, 1), 0, 0, 0)]),
  'push_constants_132': blob([('cs_main', 3)], [''], [(1, 0, 0, 3, spirv, (1, 1, 1), 132, 0, 0)]),
  'zero_entries_header': struct.pack('<8I', MAGIC, 1, 0, 1, 0, 36, 1, 0) + b'\0' * 4,
  'header_only_short': struct.pack('<7I', MAGIC, 1, 1, 1, 0, 44, 1),
}
# Code past the end: truncate a valid blob.
v = seeds['two_entries_two_perms']
seeds['code_truncated'] = v[:-5]
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                       "corpus", "shader_view")
os.makedirs(out, exist_ok=True)
for k, d in seeds.items():
    with open(os.path.join(out, k + '.dshd'), 'wb') as f:
        f.write(d)
