# Pinned core only: no FreeType, Skia, PNG or standalone executable.
file(GLOB _msdf_core CONFIGURE_DEPENDS core/*.cpp)
add_library(de_tp_msdfgen STATIC ${_msdf_core})
target_include_directories(de_tp_msdfgen SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(de_tp_msdfgen PRIVATE cxx_std_17)
target_compile_definitions(de_tp_msdfgen PUBLIC MSDFGEN_PUBLIC= MSDFGEN_USE_CPP11)
set_target_properties(de_tp_msdfgen PROPERTIES POSITION_INDEPENDENT_CODE ON)

# DENG-758: the generator is the whole cost of a font cook (about 56 s of the 60 s MSVC Debug
# budget for the Latin-ext determinism test). Compile this library optimized in Debug too, like
# a system library. Optimization changes no result: no fast-math flag is added and the
# contraction defaults stay as they are (cl /fp:precise never contracts). These directory-scope
# variables apply to this library only. cl rejects /O2 together with /RTC1, so drop /RTC1 here;
# _DEBUG, the debug CRT and _ITERATOR_DEBUG_LEVEL stay, so the link still matches the engine.
if(MSVC)
    foreach(_lang C CXX)
        separate_arguments(_de_flags NATIVE_COMMAND "${CMAKE_${_lang}_FLAGS_DEBUG}")
        list(FILTER _de_flags EXCLUDE REGEX "^[/-](RTC[1csu]*|Od|Ob0)$")
        list(JOIN _de_flags " " CMAKE_${_lang}_FLAGS_DEBUG)
    endforeach()
    target_compile_options(de_tp_msdfgen PRIVATE $<$<CONFIG:Debug>:/O2>)
else()
    target_compile_options(de_tp_msdfgen PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
