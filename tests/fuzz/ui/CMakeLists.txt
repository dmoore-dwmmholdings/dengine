# Compile the parser/resolver into the fuzz executable itself so libFuzzer coverage
# instruments the implementation, without changing normal module build flags.
if(TARGET de_ui)
 de_add_fuzz_target(NAME ui_stylesheet SOURCES fuzz_stylesheet.c
  "${DE_ROOT_DIR}/src/ui/style/stylesheet.c"
  "${DE_ROOT_DIR}/src/ui/style/style_resolve.c"
  "${DE_ROOT_DIR}/src/ui/style/theme.c"
  "${DE_ROOT_DIR}/src/ui/style/tween.c"
  "${DE_ROOT_DIR}/src/core/cvar_toml.c"
  "${DE_ROOT_DIR}/third_party/tomlc17/src/tomlc17.c"
  LIBS de_ui de_tp_tomlc17 CORPUS corpus/stylesheet)
 if(TARGET fuzz_ui_stylesheet)
  target_include_directories(fuzz_ui_stylesheet PRIVATE "${DE_ROOT_DIR}/src")
  # Keep fuzz coverage instrumentation and the normal vendored warning policy.
  set_source_files_properties("${DE_ROOT_DIR}/third_party/tomlc17/src/tomlc17.c" PROPERTIES
   COMPILE_OPTIONS "$<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>")
 endif()
endif()
