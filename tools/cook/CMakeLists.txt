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

# DENG-101: the model importer (tools/cook/importers/model/): glTF 2.0 → model IR → textures,
# materials, meshes and a prefab (SPEC §11.6). Importers are cooker-library code, so the sources go
# into de_cooker: the tools have them, and so do Debug and Dev builds (DE_HAS_COOKER), never
# Release or Shipping. The cooked formats are header-only (src/render/formats/). Entry:
# "cook/importers/model/model_import.h". cgltf and MikkTSpace are tools-only vendored libraries.
target_sources(de_cooker PRIVATE
    importers/model/model_import.h
    importers/model/model_internal.h
    importers/model/model_ir.h
    importers/model/model_ir.c
    importers/model/model_axes.c
    importers/model/gltf_import.c
    importers/model/model_material.c
    importers/model/model_prefab.c
    importers/model/model_cook.c)
# de_tp_tomlc17: only for its include path ("reflect/ser_internal.h", the float and hex formatters
# of the prefab TOML, includes the core TOML header).
target_link_libraries(de_cooker PRIVATE de_tp_cgltf de_tp_mikktspace de_reflect de_tp_tomlc17)

# DENG-289: the shader importer (importers/shader/, target de_cook_shader): `.slang` to SPIR-V,
# DXIL and metallib with reflection, through the prebuilt Slang (third_party/slang/). A separate
# library, not part of de_cooker: its C++ shim and Slang stay out of every target that does not
# cook shaders.
add_subdirectory(importers/shader)
