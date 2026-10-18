# OpenType + built-in Unicode only. Optional platform shapers have no HAVE_* definition.
add_library(de_tp_harfbuzz STATIC src/harfbuzz.cc)
target_include_directories(de_tp_harfbuzz SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(de_tp_harfbuzz PRIVATE cxx_std_17)
target_compile_definitions(de_tp_harfbuzz PRIVATE HB_NO_ATEXIT HB_NO_GETENV)
set_target_properties(de_tp_harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
 target_compile_options(de_tp_harfbuzz PRIVATE -fno-exceptions -fno-rtti)
endif()
