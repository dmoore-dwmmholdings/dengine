/*
 * de_detour_math.h: the engine's replacement of Detour's DetourMath.h (DENG-77, SPEC §5.7 rule 2).
 * Not part of the upstream release: a local file (vendor.py default `local` glob "de_*").
 *
 * The CMakeLists.txt of this directory force-includes this header into every C++ translation
 * unit that compiles or uses the library (-include / /FI), so it comes before any Detour header.
 * It defines DetourMath.h's include guard, so the upstream header is then empty and every
 * dtMath* wrapper of Detour, DetourCrowd and DetourTileCache is the one below. No vendored file is
 * patched.
 *
 * The wrappers read a thread-local switch. Off (the default), they call libm, as upstream does.
 * On, they call de_dmath, so the results are the same on every platform and libm. The nav module
 * turns the switch on for the duration of a query in deterministic mode (det_scope in
 * src/gameplay/nav/nav_recast_bridge.cpp, de_nav_set_deterministic). fabsf and isfinite are
 * exact everywhere and stay libm calls. Detour calls fabsf and floorf directly in a few places
 * (both exact); <math.h> is included for them, as upstream DetourMath.h does.
 */
#ifndef DETOURMATH_H
#define DETOURMATH_H

#ifdef __cplusplus

#include <math.h>

#include <dengine/de_math.h>

/** true while the calling thread runs a deterministic nav query (det_scope sets it). */
inline thread_local bool de__nav_detmath_on = false;

inline float dtMathFabsf(float x) { return fabsf(x); }
inline float dtMathSqrtf(float x) { return de__nav_detmath_on ? de_dsqrt(x) : sqrtf(x); }
inline float dtMathFloorf(float x) { return de__nav_detmath_on ? de_dfloor(x) : floorf(x); }
inline float dtMathCeilf(float x) { return de__nav_detmath_on ? de_dceil(x) : ceilf(x); }
inline float dtMathCosf(float x) { return de__nav_detmath_on ? de_dcos(x) : cosf(x); }
inline float dtMathSinf(float x) { return de__nav_detmath_on ? de_dsin(x) : sinf(x); }
inline float dtMathAtan2f(float y, float x) {
    return de__nav_detmath_on ? de_datan2(y, x) : atan2f(y, x);
}
inline bool dtMathIsfinite(float x) { return isfinite(x); }

#endif /* __cplusplus */

#endif /* DETOURMATH_H */
