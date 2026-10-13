# tools/cook: the de-cook tool (SPEC §11.5). DENG-145 builds `de-cook --check`, the manifest
# validator; S08 adds the cooker library (lib/), the importers (importers/) and the cook and pack
# commands (DENG-209, DENG-213). Later tasks append their targets below.
#
# de_cook_check: the --check code as a library, so tests/unit/cook/ runs it in-process. It reads
# the asset module's internal manifest headers ("asset/manifest_internal.h",
# "asset/resolve_internal.h") through the private src/ include root, and calls only the manifest
# code of de_asset (parse, resolve, validate), not the load pipeline.
add_library(de_cook_check STATIC
    check.c
    check.h
    known_types.c)
target_include_directories(de_cook_check
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${DE_ROOT_DIR}/src")
target_link_libraries(de_cook_check
    PUBLIC de_core
    PRIVATE de_asset de_platform de_build_flags)
set_target_properties(de_cook_check PROPERTIES FOLDER "dengine/tools")

# The executable: main.c only dispatches to the commands.
add_executable(de-cook main.c)
target_link_libraries(de-cook PRIVATE de_cook_check de_build_flags)
set_target_properties(de-cook PROPERTIES FOLDER "dengine/tools")

# DENG-209: the cooker library (tools/cook/lib/, target de_cooker), also linked by de_asset in
# Debug and Dev builds. known_types.c takes its table from the cooker's engine type names.
add_subdirectory(lib)
target_include_directories(de_cook_check PRIVATE "${DE_ROOT_DIR}/tools")

# DENG-213: `de-cook cook` (cook_cmd.c): cooks a whole project through the cooker and its
# `.decache` into one .dpak pack (de_pack_lib, tools/pack/lib). A library, so tests run it
# in-process (tests/unit/cook/test_cook_cli.c, tests/unit/pack/test_pack_cli.c). It reads the
# asset module's internals (manifest, resolve, type registry, pack reader) through the src/ root
# and the cooker through the tools/ root. de_pack_lib is defined later (tools/pack sorts after
# tools/cook); CMake resolves the name at generate time.
add_library(de_cook_cmd STATIC cook_cmd.c cook_cmd.h)
target_include_directories(de_cook_cmd
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
target_link_libraries(de_cook_cmd
    PUBLIC de_cook_check de_asset de_reflect de_core
    PRIVATE de_cooker de_pack_lib de_ecs de_task de_platform de_tp_xxhash de_build_flags)
set_target_properties(de_cook_cmd PROPERTIES FOLDER "dengine/tools")
target_link_libraries(de-cook PRIVATE de_cook_cmd)
