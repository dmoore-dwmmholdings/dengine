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
