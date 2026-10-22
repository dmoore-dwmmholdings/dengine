"""Scan shared animation headers with the unchanged SPEC source checker.

The per-source CMake/table entries cover every fixed-path TU. Headers do not have
compile_commands entries, so scan all animation headers separately, with no rule
exceptions. Keep a planted include/libm/fast-API/approximate-SIMD negative control.
"""
from pathlib import Path
import importlib.util
import sys

root = Path(sys.argv[1]).resolve()
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("det_source", root / "tools/check_determinism_src.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
errors = []
headers = sorted((root / "src/anim").rglob("*.h"))
for header in headers:
    errors.extend(checker.scan_text(str(header.relative_to(root)), header.read_text(), set(), set()))
planted = '#include <math.h>\nvoid f(void) { sinf(1); de_quat_normalize(q); _mm_rsqrt_ps(q); }'
findings = checker.scan_text("negative-control.c", planted, set(), set())
for rule in ("math-include", "libm-call", "fast-api", "simd-approx"):
    if not any(rule in finding for finding in findings):
        errors.append(f"Negative control did not detect {rule}")
if errors:
    print("\n".join(errors), file=sys.stderr)
    sys.exit(1)
print(f"Animation deterministic header guard: {len(headers)} headers, negative controls pass")
