#!/usr/bin/env python3
"""Checks tests/unit/math/vectors/dmath_golden.txt against mpmath (DENG-243).

For each function it prints the number of normal-range cases, the largest error in ulps against
the exact value, and how many results are not the correctly rounded value (f32: transcendentals;
exact for floor/ceil/round/fmod; f64: sin64/cos64). Run it after regenerating the golden file.

Needs mpmath (pip install mpmath).
Usage: python3 tools/dmath/verify_dmath_golden.py tests/unit/math/vectors/dmath_golden.txt
"""
import sys, struct, math
import mpmath as mp
from fractions import Fraction
mp.mp.dps = 60
def f32(u): return struct.unpack('<f', struct.pack('<I', u))[0]
def f64(u): return struct.unpack('<d', struct.pack('<Q', u))[0]
def ulp32(v):  # v mp value, normal range
    e = int(mp.floor(mp.log(abs(v), 2)))
    return mp.mpf(2) ** (e - 23)
def ulp64(v):
    e = int(mp.floor(mp.log(abs(v), 2)))
    return mp.mpf(2) ** (e - 52)
FLT_MIN = 2.0**-126; FLT_MAX = f32(0x7f7fffff)
def isnormal32(u): e=(u>>23)&0xff; return 0 < e < 255
ref = {
 'sin': lambda a: mp.sin(a), 'cos': lambda a: mp.cos(a), 'tan': lambda a: mp.tan(a),
 'asin': lambda a: mp.asin(a), 'acos': lambda a: mp.acos(a), 'atan': lambda a: mp.atan(a),
 'exp': lambda a: mp.exp(a), 'log': lambda a: mp.log(a), 'sqrt': lambda a: mp.sqrt(a),
 'atan2': lambda a, b: mp.atan2(a, b), 'pow': lambda a, b: mp.power(a, b),
}
stats = {}
sec = None
for line in open(sys.argv[1]):
    if line.startswith('#'): continue
    if line.startswith('@'):
        p = line.split(); sec = p[1]; nin = int(p[2]); nout = int(p[3]); bits = int(p[4]); continue
    v = [int(t, 16) for t in line.split()]
    ins, outs = v[:nin], v[nin:]
    st = stats.setdefault(sec, [0, 0.0, 0, None])  # n, max ulp, n_not_cr, worst
    if sec in ('sin64', 'cos64'):
        x = f64(ins[0]); e = (ins[0] >> 52) & 0x7ff
        if e == 0 or e == 0x7ff: continue
        with mp.workdps(400):
            t = mp.sin(mp.mpf(x)) if sec == 'sin64' else mp.cos(mp.mpf(x))
        r = f64(outs[0])
        err = abs(mp.mpf(r) - t) / ulp64(t)
        st[0] += 1
        if err > st[1]: st[1] = float(err); st[3] = hex(ins[0])
        if err > 0.5: st[2] += 1
        continue
    if sec in ref:
        if not all(isnormal32(u) for u in ins): continue
        a = [mp.mpf(f32(u)) for u in ins]
        with mp.workdps(60 if sec not in ('sin','cos','tan') else 200):
            try:
                t = ref[sec](*a)
            except Exception:
                continue
        if isinstance(t, mp.mpc) or not mp.isfinite(t): continue
        if t == 0 or abs(t) < FLT_MIN or abs(t) > FLT_MAX: continue
        r = f32(outs[0])
        if math.isinf(r) or math.isnan(r):
            st[0] += 1; st[1] = 1e30; st[3] = [hex(u) for u in ins]; continue
        err = abs(mp.mpf(r) - t) / ulp32(t)
        st[0] += 1
        if err > st[1]: st[1] = float(err); st[3] = [hex(u) for u in ins]
        if err > 0.5: st[2] += 1
    elif sec in ('floor', 'ceil', 'round', 'fmod'):
        if not all(isnormal32(u) or (u & 0x7fffffff) == 0 for u in ins): continue
        x = f32(ins[0])
        if sec == 'fmod':
            y = f32(ins[1])
            if y == 0: continue
            fx, fy = Fraction(x), Fraction(y)
            q = abs(fx) // abs(fy)
            t = abs(fx) - q * abs(fy)
            t = t if x >= 0 else -t
            if abs(t) < FLT_MIN: t = 0
        else:
            fx = Fraction(x)
            if sec == 'floor': t = math.floor(fx)
            elif sec == 'ceil': t = math.ceil(fx)
            else: t = math.floor(abs(fx) + Fraction(1, 2)) * (1 if x >= 0 else -1)
        r = f32(outs[0])
        st[0] += 1
        if Fraction(r) != Fraction(t): st[2] += 1; st[3] = [hex(u) for u in ins]
for k, (n, m, ncr, w) in stats.items():
    print(f"{k:16s} n={n:5d} max_ulp={m:.4f} not_correctly_rounded_or_wrong={ncr} worst={w}")
