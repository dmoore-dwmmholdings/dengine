# Pinned core only: no FreeType, Skia, PNG or standalone executable.
file(GLOB _msdf_core CONFIGURE_DEPENDS core/*.cpp)
add_library(de_tp_msdfgen STATIC ${_msdf_core})
target_include_directories(de_tp_msdfgen SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(de_tp_msdfgen PRIVATE cxx_std_17)
target_compile_definitions(de_tp_msdfgen PUBLIC MSDFGEN_PUBLIC= MSDFGEN_USE_CPP11)
set_target_properties(de_tp_msdfgen PROPERTIES POSITION_INDEPENDENT_CODE ON)
