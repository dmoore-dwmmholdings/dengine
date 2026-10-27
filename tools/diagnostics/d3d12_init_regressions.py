#!/usr/bin/env python3
"""Run original compute-to-UI goldens through both D3D12 barrier implementations."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET

SCENES = (
    "text_msdf_scales", "text_outline_shadow", "text_bitmap_pixel", "text_world_text",
    "ui_rect_border", "ui_text_labels", "ui_widgets_basic", "ui_widgets_advanced_text",
    "ui_pixel_four",
)
EXPECTED = {"golden.d3d12." + scene for scene in SCENES}


def verify_report(path, mode, numeric=False):
    cases = list(ET.parse(path).iter("testcase"))
    errors = []
    expected = ({"gpu_d3d12.d3d12.compute_first_rt_" + q for q in ("graphics", "compute")}
                if numeric else EXPECTED)
    if len(cases) != len(expected) or {c.get("name") for c in cases} != expected:
        errors.append("missing, duplicate or unexpected native cases")
    for case in cases:
        name = case.get("name")
        if case.find("failure") is not None or case.find("error") is not None:
            errors.append(f"{name}: failed")
        if case.find("skipped") is not None or case.get("status") == "notrun":
            errors.append(f"{name}: skipped/notrun")
        output = case.findtext("system-out", "")
        marker = f"barriers={mode}" if numeric else f"d3d12: {mode} barriers"
        if marker not in output:
            errors.append(f"{name}: actual {mode} backend mode not observed")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-dir", type=Path, required=True)
    parser.add_argument("--numeric-only", action="store_true",
                        help="Run exact compute-first/LOAD/draw readback cases, without goldens")
    args = parser.parse_args()
    build = args.test_dir.resolve()
    pattern = r"^golden\.d3d12\.(" + "|".join(map(re.escape, SCENES)) + r")$"
    if args.numeric_only:
        pattern = r"^gpu_d3d12\.d3d12\.compute_first_rt_(graphics|compute)$"
    errors = []
    for mode, value in (("enhanced", "0"), ("legacy", "1")):
        family = "numeric" if args.numeric_only else "golden"
        stem = "d3d12-init-numeric" if args.numeric_only else "d3d12-init"
        report = build / f"{stem}-{mode}.xml"
        log = build / f"{stem}-{mode}.log"
        report.unlink(missing_ok=True)
        env = dict(os.environ, DE_D3D12_FORCE_LEGACY_BARRIERS=value)
        if args.numeric_only:
            raw = build / "d3d12-init-readbacks"
            raw.mkdir(parents=True, exist_ok=True)
            env["DE_D3D12_INIT_EVIDENCE_DIR"] = str(raw)
        command = ["ctest", "--test-dir", str(build), "--no-tests=error",
                   "--output-on-failure", "--test-output-size-passed", "1048576",
                   "--test-output-size-failed", "1048576", "--output-junit", str(report),
                   "-R", pattern]
        with log.open("w") as stream:
            result = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            errors.append(f"{mode}: CTest exited {result.returncode}; see {log}")
        try:
            errors.extend(f"{mode}: {e}" for e in verify_report(report, mode, args.numeric_only))
        except (OSError, ET.ParseError) as exc:
            errors.append(f"{mode}: missing/malformed report: {exc}")
        images = build / "golden"
        if not args.numeric_only and images.exists():
            shutil.copytree(images, build / f"golden-init-{mode}", dirs_exist_ok=True)
        count = 2 if args.numeric_only else len(EXPECTED)
        print(f"D3D12 initialization: {mode}, {count} required {family} cases, "
              f"CTest exit {result.returncode}", flush=True)
    for error in errors:
        print(error, flush=True)
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
