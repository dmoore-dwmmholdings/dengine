#!/usr/bin/env python3
"""abi_check.py: compare two abi_dump.py dumps and classify every difference (DENG-128).

  abi_check.py baseline.json current.json [--json report.json]

Breaking (docs/versioning.md, "What counts as breaking"):
  function or exported variable removed (a rename is a removal plus an addition), or its type
  changed (parameter, return type, variadic); struct/union removed, or its size or alignment
  changed, or a field removed, added, moved (offset) or retyped; enum constant removed or its
  value changed; enum size changed; typedef removed or its underlying type changed (handles,
  callbacks); opaque type removed; DE_GAME_API_VERSION changed.
Compatible:
  new function or variable; new struct, union, enum or typedef; new enum constant (appended,
  since a constant inserted in the middle shifts the values after it, which is breaking); new
  header; an opaque type that becomes defined.

Exit status: 0 no breaking change, or breaking changes allowed by the version (the current
DE_VERSION_MAJOR is higher than the baseline's; or, while the major is 0, the minor is higher:
0.x may break, DENG-2 Q8). 1 breaking changes not allowed. 2 bad input (unreadable dumps,
different platforms or dump formats).

Python 3.10+, stdlib only.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def change(kind: str, name: str, detail: str) -> dict:
    return {"kind": kind, "name": name, "detail": detail}


def diff(base: dict, cur: dict) -> tuple[list[dict], list[dict]]:
    brk: list[dict] = []
    ok: list[dict] = []

    for section, noun in (("functions", "function"), ("variables", "variable")):
        b, c = base.get(section, {}), cur.get(section, {})
        for name in sorted(b):
            if name not in c:
                brk.append(change(f"{noun}_removed", name, f"{b[name]['type']} ({b[name]['header']})"))
            elif b[name]["type"] != c[name]["type"]:
                brk.append(change(f"{noun}_signature_changed", name,
                                  f"{b[name]['type']} -> {c[name]['type']}"))
        for name in sorted(set(c) - set(b)):
            ok.append(change(f"{noun}_added", name, f"{c[name]['type']} ({c[name]['header']})"))

    b, c = base.get("records", {}), cur.get("records", {})
    for name in sorted(b):
        if name not in c:
            brk.append(change("record_removed", name, f"{b[name]['kind']} ({b[name]['header']})"))
            continue
        rb, rc = b[name], c[name]
        if rb["kind"] != rc["kind"]:
            brk.append(change("record_kind_changed", name, f"{rb['kind']} -> {rc['kind']}"))
        if rb.get("size") != rc.get("size"):
            brk.append(change("record_size_changed", name, f"size {rb.get('size')} -> {rc.get('size')}"))
        if rb.get("align") != rc.get("align"):
            brk.append(change("record_align_changed", name,
                              f"align {rb.get('align')} -> {rc.get('align')}"))
        fb = {f["name"]: f for f in rb["fields"]}
        fc = {f["name"]: f for f in rc["fields"]}
        for fname, f in fb.items():
            g = fc.get(fname)
            if g is None:
                brk.append(change("field_removed", f"{name}.{fname}", f["type"]))
                continue
            if f["type"] != g["type"]:
                brk.append(change("field_type_changed", f"{name}.{fname}", f"{f['type']} -> {g['type']}"))
            if f.get("offset") != g.get("offset") or f.get("bits") != g.get("bits"):
                where = lambda x: f"offset {x['offset']}" if "offset" in x else f"bits {x.get('bits')}"
                brk.append(change("field_offset_changed", f"{name}.{fname}", f"{where(f)} -> {where(g)}"))
        if [n for n in fb if n in fc] != [n for n in fc if n in fb] and not any(
                x["kind"] == "field_offset_changed" and x["name"].startswith(name + ".") for x in brk):
            brk.append(change("field_order_changed", name, "fields declared in a different order"))
        for fname in fc:
            if fname not in fb:
                brk.append(change("field_added", f"{name}.{fname}",
                                  f"{fc[fname]['type']}: new fields only in a new major or a new *_desc2"))
    for name in sorted(set(c) - set(b)):
        ok.append(change("record_added", name, f"{c[name]['kind']} ({c[name]['header']})"))

    ob, oc = set(base.get("opaque", [])), set(cur.get("opaque", []))
    for name in sorted(ob - oc):
        if name not in c:
            brk.append(change("opaque_removed", name, "opaque type no longer declared"))
        else:
            ok.append(change("opaque_defined", name, "opaque type now has a public definition"))
    for name in sorted(oc - ob):
        ok.append(change("opaque_added", name, "new opaque type"))

    eb, ec = base.get("enums", {}), cur.get("enums", {})
    cb = {k: v for e in eb.values() for k, v in e["constants"].items()}
    cc = {k: v for e in ec.values() for k, v in e["constants"].items()}
    for k in sorted(cb):
        if k not in cc:
            brk.append(change("enum_constant_removed", k, f"was {cb[k]}"))
        elif cb[k] != cc[k]:
            brk.append(change("enum_value_changed", k, f"{cb[k]} -> {cc[k]}"))
    for k in sorted(set(cc) - set(cb)):
        ok.append(change("enum_constant_added", k, f"= {cc[k]}"))
    for name in sorted(eb):
        if name in ec and eb[name].get("size") != ec[name].get("size"):
            brk.append(change("enum_size_changed", name, f"size {eb[name].get('size')} -> {ec[name].get('size')}"))
    for name in sorted(set(ec) - set(eb)):
        if not name.startswith("(anonymous"):
            ok.append(change("enum_added", name, ec[name]["header"]))

    tb, tc = base.get("typedefs", {}), cur.get("typedefs", {})
    for name in sorted(tb):
        if name not in tc:
            brk.append(change("typedef_removed", name, tb[name]))
        elif tb[name] != tc[name]:
            brk.append(change("typedef_changed", name, f"{tb[name]} -> {tc[name]}"))
    for name in sorted(set(tc) - set(tb)):
        ok.append(change("typedef_added", name, tc[name]))

    hb = {v["header"] for s in ("functions", "records") for v in base.get(s, {}).values()}
    hc = {v["header"] for s in ("functions", "records") for v in cur.get(s, {}).values()}
    for h in sorted(hc - hb):
        ok.append(change("header_added", h, "new public header"))

    gb = base.get("versions", {}).get("DE_GAME_API_VERSION")
    gc = cur.get("versions", {}).get("DE_GAME_API_VERSION")
    if gb != gc:
        brk.append(change("game_api_version_changed", "DE_GAME_API_VERSION", f"{gb} -> {gc}"))
    return brk, ok


def version(d: dict) -> tuple[int, int, int]:
    v = d.get("versions", {})
    return (int(v.get("DE_VERSION_MAJOR", 0)), int(v.get("DE_VERSION_MINOR", 0)),
            int(v.get("DE_VERSION_PATCH", 0)))


def allowed_by(vb: tuple[int, int, int], vc: tuple[int, int, int]) -> str | None:
    if vc[0] > vb[0]:
        return "major"
    if vb[0] == 0 and vc[0] == 0 and vc[1] > vb[1]:
        return "0.x-minor"
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("baseline", type=Path)
    ap.add_argument("current", type=Path)
    ap.add_argument("--json", type=Path, help="also write the classified report here")
    ap.add_argument("--quiet", action="store_true", help="do not list compatible changes")
    args = ap.parse_args(argv)
    try:
        base = json.loads(args.baseline.read_text())
        cur = json.loads(args.current.read_text())
    except (OSError, ValueError) as e:
        print(f"abi_check: cannot read dump: {e}", file=sys.stderr)
        return 2
    if base.get("format") != cur.get("format"):
        print(f"abi_check: dump formats differ ({base.get('format')} vs {cur.get('format')})", file=sys.stderr)
        return 2
    if base.get("platform") != cur.get("platform"):
        print(f"abi_check: platforms differ ({base.get('platform')} vs {cur.get('platform')}); "
              "compare dumps from the same platform", file=sys.stderr)
        return 2
    if not (base.get("measured", True) and cur.get("measured", True)):
        print("abi_check: warning: a dump was made with --compile-only; layouts were not measured",
              file=sys.stderr)

    brk, ok = diff(base, cur)
    vb, vc = version(base), version(cur)
    allow = allowed_by(vb, vc) if brk else None
    vs = lambda v: ".".join(map(str, v))

    print(f"abi_check: {base.get('platform')}: {vs(vb)} -> {vs(vc)}: "
          f"{len(brk)} breaking, {len(ok)} compatible")
    for c in brk:
        print(f"  BREAKING   {c['kind']:<28} {c['name']}: {c['detail']}")
    if not args.quiet:
        for c in ok:
            print(f"  compatible {c['kind']:<28} {c['name']}: {c['detail']}")
    if brk and allow == "major":
        print(f"abi_check: NOTICE: breaking changes allowed because the major version increased "
              f"({vb[0]} -> {vc[0]}). Record a new baseline for this major.")
    elif brk and allow == "0.x-minor":
        print(f"abi_check: NOTICE: breaking changes allowed in 0.x because the minor version "
              f"increased ({vs(vb)} -> {vs(vc)}); major version 0 makes no ABI promise.")
    elif brk:
        print("abi_check: FAIL: breaking changes need a major version bump (docs/versioning.md)")

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({
            "platform": base.get("platform"), "baseline_version": vs(vb), "current_version": vs(vc),
            "breaking": brk, "compatible": ok, "allowed_by": allow,
            "allowed_by_major_bump": allow == "major",
        }, indent=1) + "\n")
    return 1 if brk and not allow else 0


if __name__ == "__main__":
    sys.exit(main())
