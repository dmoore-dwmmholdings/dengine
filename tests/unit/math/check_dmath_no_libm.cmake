# check_dmath_no_libm.cmake: SPEC §5.7 rule 2 for the deterministic math (DENG-243).
#
# The files of src/core/math/ that make up de_dmath may include <math.h> only for sqrt and sqrtf
# (correctly rounded IEEE operations). Every other libm function differs between platforms, so a
# call to one (or to its __builtin_ form) fails this check. The fast-math header
# math_internal.h must not be included either. Comments are stripped before the scan.
#
# Usage: cmake -DDE_ROOT_DIR=<repo> -P check_dmath_no_libm.cmake
if(NOT DE_ROOT_DIR)
    message(FATAL_ERROR "check_dmath_no_libm: DE_ROOT_DIR is not set")
endif()

file(GLOB files "${DE_ROOT_DIR}/src/core/math/dmath*.c" "${DE_ROOT_DIR}/src/core/math/dmath*.h")
list(LENGTH files nfiles)
if(nfiles LESS 3)
    message(FATAL_ERROR "check_dmath_no_libm: expected dmath.c, dmath64.c and dmath_reduce.h, "
                        "found: ${files}")
endif()

set(funcs "acos|acosh|asin|asinh|atan|atan2|atanh|cbrt|ceil|copysign|cos|cosh|erf|erfc|exp|"
          "exp2|expm1|fabs|fdim|floor|fma|fmax|fmin|fmod|frexp|hypot|ilogb|ldexp|lgamma|llrint|"
          "llround|log|log10|log1p|log2|logb|lrint|lround|modf|nan|nearbyint|nextafter|"
          "nexttoward|pow|remainder|remquo|rint|round|scalbln|scalbn|sin|sinh|tan|tanh|tgamma|"
          "trunc|sincos|exp10")
string(JOIN "" funcs ${funcs})

set(errors "")
foreach(f IN LISTS files)
    file(READ "${f}" text)
    # Strip /* */ and // comments (string literals in these files hold no comment markers).
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" text "${text}")
    string(REGEX REPLACE "//[^\n]*" "" text "${text}")
    if(text MATCHES "#[ \t]*include[ \t]*[<\"][^>\"]*math_internal\\.h")
        string(APPEND errors "${f}: includes math_internal.h (the fast math)\n")
    endif()
    # A call: the name, not preceded by an identifier character, optionally with an f or l
    # suffix or a __builtin_ prefix, then '('.
    string(REGEX MATCHALL "(^|[^A-Za-z0-9_])(__builtin_)?(${funcs})[fl]?[ \t]*\\(" hits "${text}")
    foreach(h IN LISTS hits)
        string(STRIP "${h}" h)
        string(APPEND errors "${f}: calls a <math.h> function: ${h}\n")
    endforeach()
endforeach()

if(errors)
    message(FATAL_ERROR "check_dmath_no_libm: deterministic math must not use libm "
                        "(SPEC §5.7 rule 2):\n${errors}")
endif()
message(STATUS "check_dmath_no_libm: ${nfiles} files clean")
