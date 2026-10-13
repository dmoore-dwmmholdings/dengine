#!/usr/bin/env python3
"""check_caps_json.py: runs de-caps and checks its output (DENG-290, CTest de_caps_json_schema_*).

    check_caps_json.py --exe <de-caps> --schema caps_schema.json [--backend <name>] [--null]
                       [--expect-ok <backend>]...

1. `de-caps [--backend <name>]` (the text report) must exit 0 and print its summary header.
2. `de-caps --json [--backend <name>]` must exit 0 and print one JSON document that validates
   against caps_schema.json. The validator below implements the JSON Schema keywords the schema
   uses (stdlib only, so the check runs wherever CTest runs).
3. The document must be consistent: adapters only for status "ok", caps.backend equal to the
   backend, at most one default adapter, a reason for every failed tier, and a recommended tier
   that the tier flags allow.
4. With --null (for `--backend null`): exactly the null backend, one adapter, Base, no RT,
   recommended "base".
5. With --expect-ok <backend>: that backend has status "ok" and at least one adapter whose device
   was created (D3D12 on Windows always has WARP; every macOS machine has a Metal device).

Exit code 0 when every check passes, 1 otherwise.
"""
import argparse
import json
import subprocess
import sys

_TYPES = {
    "object": dict,
    "array": list,
    "string": str,
    "boolean": bool,
    "null": type(None),
}


def _is_type(value, name):
    if name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return isinstance(value, _TYPES[name])


def validate(value, schema, path, errors):
    """Appends a message to `errors` for every violation of `schema` by `value`."""
    if "oneOf" in schema:
        matches = 0
        closest = None  # the errors of the alternative whose type fits, to explain a miss
        for sub in schema["oneOf"]:
            sub_errors = []
            validate(value, sub, path, sub_errors)
            matches += not sub_errors
            if sub_errors and not any("is not" in e and e.startswith(f"{path}:")
                                      for e in sub_errors[:1]):
                closest = sub_errors
        if matches != 1:
            errors.append(f"{path}: matches {matches} of the oneOf schemas (needs exactly 1)")
            if matches == 0 and closest:
                errors.extend(closest)
        return
    if "const" in schema and value != schema["const"]:
        errors.append(f"{path}: {value!r} is not {schema['const']!r}")
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path}: {value!r} is not one of {schema['enum']}")
    if "type" in schema:
        types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_is_type(value, t) for t in types):
            errors.append(f"{path}: {type(value).__name__} is not {'/'.join(types)}")
            return
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{path}: {value} < minimum {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            errors.append(f"{path}: {value} > maximum {schema['maximum']}")
    if isinstance(value, str) and "minLength" in schema and len(value) < schema["minLength"]:
        errors.append(f"{path}: shorter than {schema['minLength']}")
    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            errors.append(f"{path}: fewer than {schema['minItems']} items")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            errors.append(f"{path}: more than {schema['maxItems']} items")
        if "items" in schema:
            for i, item in enumerate(value):
                validate(item, schema["items"], f"{path}[{i}]", errors)
    if isinstance(value, dict):
        props = schema.get("properties", {})
        for key in schema.get("required", []):
            if key not in value:
                errors.append(f"{path}: missing key '{key}'")
        for key, item in value.items():
            if key in props:
                validate(item, props[key], f"{path}.{key}", errors)
            elif schema.get("additionalProperties", True) is False:
                errors.append(f"{path}: unexpected key '{key}'")


def check_consistency(doc, errors):
    for b in doc["backends"]:
        where = f"backend {b['backend']}"
        adapters = b["adapters"]
        if b["status"] == "ok":
            if not adapters:
                errors.append(f"{where}: status ok without adapters")
            if b["error"] is not None:
                errors.append(f"{where}: status ok with an error")
        else:
            if adapters:
                errors.append(f"{where}: status {b['status']!r} with adapters")
            if not b["error"]:
                errors.append(f"{where}: status {b['status']!r} without an error message")
        defaults = [a["index"] for a in adapters if a["default"]]
        if len(defaults) > 1:
            errors.append(f"{where}: several default adapters {defaults}")
        if b["default_adapter"] is not None and defaults != [b["default_adapter"]]:
            errors.append(f"{where}: default_adapter {b['default_adapter']} but defaults {defaults}")
        if b["default_adapter"] is None and defaults:
            errors.append(f"{where}: default flags {defaults} without default_adapter")
        indices = [a["index"] for a in adapters]
        if indices != sorted(set(indices)):
            errors.append(f"{where}: adapter indices {indices} are not increasing")
        for a in adapters:
            at = f"{where} adapter {a['index']}"
            if a["status"] == "ok":
                if a["caps"] is None or a["tier"] is None or a["error"] is not None:
                    errors.append(f"{at}: status ok needs caps and tier and no error")
                    continue
            else:
                if a["caps"] is not None or a["tier"] is not None or not a["error"]:
                    errors.append(f"{at}: status error needs an error and no caps or tier")
                continue
            caps, tier = a["caps"], a["tier"]
            if caps["backend"] != b["backend"]:
                errors.append(f"{at}: caps.backend {caps['backend']!r}")
            if caps["base_tier_ok"] == bool(caps["base_tier_reason"]):
                errors.append(f"{at}: base_tier_ok and base_tier_reason disagree")
            if tier["base_ok"] == bool(tier["base_reasons"]):
                errors.append(f"{at}: base_ok and base_reasons disagree")
            if tier["rt_ok"] == bool(tier["rt_reasons"]):
                errors.append(f"{at}: rt_ok and rt_reasons disagree")
            if tier["rt_ok"] and not tier["base_ok"]:
                errors.append(f"{at}: RT without Base")
            if not caps["base_tier_ok"] and tier["base_ok"]:
                errors.append(f"{at}: Base ok although the backend's check failed")
            rec = tier["recommended"]
            expect = "none" if not tier["base_ok"] else None
            if expect and rec != expect:
                errors.append(f"{at}: recommended {rec!r} below Base")
            if rec == "rt" and not tier["rt_ok"]:
                errors.append(f"{at}: recommended rt without RT")
            if rec == "base" and tier["rt_ok"] and not tier["recommended_reason"]:
                errors.append(f"{at}: RT capable, Base recommended, no reason")
            if tier["mesh_shaders"] and not caps["mesh_shaders"]:
                errors.append(f"{at}: tier mesh_shaders without caps.mesh_shaders")


def check_null(doc, errors):
    names = [b["backend"] for b in doc["backends"]]
    if names != ["null"]:
        errors.append(f"--backend null printed backends {names}")
        return
    b = doc["backends"][0]
    if b["status"] != "ok" or len(b["adapters"]) != 1 or b["default_adapter"] != 1:
        errors.append(f"null backend: {b['status']}, {len(b['adapters'])} adapters, "
                      f"default {b['default_adapter']}")
        return
    a = b["adapters"][0]
    if a["caps"] is None or a["caps"]["device_name"] != "null":
        errors.append("null adapter: missing caps or device_name is not 'null'")
        return
    tier = a["tier"]
    if not tier["base_ok"] or tier["rt_ok"] or tier["recommended"] != "base":
        errors.append(f"null adapter: tier {tier}")


def check_expect_ok(doc, backend, errors):
    for b in doc["backends"]:
        if b["backend"] != backend:
            continue
        if b["status"] != "ok" or not any(a["status"] == "ok" for a in b["adapters"]):
            errors.append(f"backend {backend}: expected a working adapter, got status "
                          f"{b['status']!r} ({b['error'] or 'every adapter failed'})")
        return
    errors.append(f"backend {backend}: not in the report")


def run(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=240)
    return proc.returncode, proc.stdout, proc.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--schema", required=True)
    ap.add_argument("--backend")
    ap.add_argument("--null", action="store_true", help="expect the null backend's report")
    ap.add_argument("--expect-ok", action="append", default=[], metavar="BACKEND",
                    help="require a working adapter of this backend")
    args = ap.parse_args()

    extra = ["--backend", args.backend] if args.backend else []
    errors = []

    code, out, err = run([args.exe, *extra])
    if code != 0:
        errors.append(f"text report: exit code {code}\n{err}")
    elif not out.startswith("de-caps ") or "backend" not in out:
        errors.append(f"text report: unexpected output:\n{out}")
    else:
        sys.stdout.write(out)

    code, out, err = run([args.exe, "--json", *extra])
    if code != 0:
        errors.append(f"--json: exit code {code}\n{err}")
    else:
        try:
            doc = json.loads(out)
        except json.JSONDecodeError as e:
            errors.append(f"--json: not JSON ({e}):\n{out}")
            doc = None
        if doc is not None:
            with open(args.schema, encoding="utf-8") as f:
                schema = json.load(f)
            schema_errors = []
            validate(doc, schema, "$", schema_errors)
            errors.extend(schema_errors)
            if not schema_errors:
                check_consistency(doc, errors)
                if args.null:
                    check_null(doc, errors)
                for backend in args.expect_ok:
                    check_expect_ok(doc, backend, errors)

    code, _, err = run([args.exe, "--backend", "nonsense"])
    if code != 2:
        errors.append(f"--backend nonsense: exit code {code}, expected 2")

    for e in errors:
        print(f"FAIL: {e}", file=sys.stderr)
    if not errors:
        print("de-caps output: OK")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
