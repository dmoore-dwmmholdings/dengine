"""Compare actual GPU hit samples with independently intersected authored box faces.

CI diagnostic only. This is neither a final-image nor a motion/performance oracle.
The geometry classifier does not use the production depth-plane reconstruction.
"""
from pathlib import Path
import contextlib
import io
import json
import math
import os
import runpy
import sys

capture = Path(sys.argv[1])
output = Path(sys.argv[2])
saved_args = sys.argv[:]
try:
    sys.argv = ["geometry", str(capture), str(output) + ".geometry.json"]
    with contextlib.redirect_stdout(io.StringIO()):
        geometry = runpy.run_path(".github/scripts/deng604-authored-geometry.py")
finally:
    sys.argv = saved_args

g = geometry
moving = bool(os.getenv("DE_DENG604_MOTION"))
if moving:
    assert not g["static_matrices"] and g["max_motion"] > 1e-5
    assert g["previous_depth_errors"]
else:
    assert g["static_matrices"] and g["max_motion"] == 0
    assert g["surface_depth_mismatch"] == 0
assert g["missing_geometry"] == g["missing_raster"] == 0
w, h, tw, th = g["w"], g["h"], g["tw"], g["th"]
actual = g["texture"]("hit_footprints", 11, "4f", tw, th)
color = g["texture"]("history_color", 5, "4e", w, h)
faces = g["faces"]
counts = {"positive": 0, "inactive": 0, "full": 0, "partial": 0, "none": 0}
errors = []
max_coverage_error = max_color_error = 0.0
for i, (hit, sample) in enumerate(zip(g["hits"], actual)):
    assert all(math.isfinite(v) for v in sample), (i, sample)
    u, v, z, confidence = hit
    if not (z > 0 and confidence > 0):
        counts["inactive"] += 1
        assert all(abs(v) < 1e-7 for v in sample), (i, sample)
        continue
    counts["positive"] += 1
    hx, hy = min(int(u*w), w-1), min(int(v*h), h-1)
    face = faces[hy*w+hx]
    assert face is not None
    mx, my = g["motion"][hy*w+hx]
    previous_u, previous_v = u-mx*.5, v+my*.5
    in_domain = 0 <= previous_u < 1 and 0 <= previous_v < 1
    px, py = previous_u*w-.5, previous_v*h-.5
    bx, by = math.floor(px), math.floor(py)
    fx, fy = px-bx, py-by
    rgb = [0., 0., 0.]
    coverage = 0.
    for yy in range(2):
        for xx in range(2):
            x, y = bx+xx, by+yy
            weight = (fx if xx else 1-fx)*(fy if yy else 1-fy)
            if not in_domain or weight <= 0 or not (0 <= x < w and 0 <= y < h):
                continue
            other = g["previous_faces"][y*w+x]
            # Distinct scene objects can share the same geometric plane.
            coplanar = (other is not None and other[1:3] == face[1:3]
                        and abs(other[3]-face[3]) < 1e-4)
            if not coplanar:
                continue
            coverage += weight
            for k in range(3):
                rgb[k] += color[y*w+x][k]*weight
    if coverage > 1e-6:
        rgb = [v/coverage for v in rgb]
    else:
        rgb = [0., 0., 0.]
        coverage = 0.
    counts["full" if coverage > 1-1e-6 else "partial" if coverage > 0 else "none"] += 1
    coverage_error = abs(sample[3]-coverage)
    color_error = max(abs(sample[k]-rgb[k]) for k in range(3))
    max_coverage_error = max(max_coverage_error, coverage_error)
    max_color_error = max(max_color_error, color_error)
    # Float32 projection/bilinear arithmetic versus F64 authored intersections.
    if (coverage_error > 1e-4 or
            any(abs(sample[k]-rgb[k]) > 1e-4*(1+abs(rgb[k])) for k in range(3))):
        errors.append({"trace_pixel": [i % tw, i // tw], "face": face,
                       "expected": rgb+[coverage], "actual": sample})

if moving:
    assert counts["positive"] >= 2000 and counts["partial"] >= 50
else:
    assert counts["positive"] == 2578
    assert counts["full"] == 2454 and counts["partial"] == 124
assert counts["inactive"] == tw*th-counts["positive"]
report = {"moving_camera": moving, "max_motion_ndc": g["max_motion"],
          "previous_depth_max_error": max(g["previous_depth_errors"], default=0),
          "previous_snapped_empty": g["previous_snapped_empty"], "counts": counts, "max_coverage_error": max_coverage_error,
          "max_color_error": max_color_error, "mismatches": len(errors),
          "examples": errors[:40], "coverage_tolerance": 1e-4,
          "color_tolerance": "abs(error) <= 1e-4 * (1 + abs(expected))",
          "scope": "Actual shader helper on captured scene hits versus independent authored faces. "
                   "No final-image, actual ANC motion, native Metal or performance acceptance."}
output.write_text(json.dumps(report, indent=2)+"\n")
print(json.dumps(report, indent=2), flush=True)
assert not errors, "Actual GPU hit sample disagrees with authored-face oracle"
