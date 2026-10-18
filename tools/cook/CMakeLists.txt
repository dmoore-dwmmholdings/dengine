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

# DENG-289: the shader importer (importers/shader/): `.slang` to SPIR-V, DXIL and metallib with
# reflection, through the prebuilt Slang (third_party/slang/, opened at run time). Since DENG-409
# its sources are part of de_cooker (like the other importers), so the built-in importer table
# names it; `de_cook_shader` remains as an INTERFACE alias of de_cooker.
add_subdirectory(importers/shader)

# DENG-102: the FBX front end of the model importer (tools/cook/importers/model/fbx_*.c): ufbx
# (tools-only vendored library) into the same model IR as glTF. Entry for the fuzz target:
# "cook/importers/model/fbx_import.h" (de_model_fbx_load).
target_sources(de_cooker PRIVATE
    importers/model/fbx_import.h
    importers/model/fbx_internal.h
    importers/model/fbx_import.c
    importers/model/fbx_material.c)
target_link_libraries(de_cooker PRIVATE de_tp_ufbx)

# DENG-98: the texture importer (tools/cook/importers/texture/): PNG, JPEG (stb_image), OpenEXR
# (tinyexr), KTX2 (own container reader; Basis ETC1S/UASTC through the Basis Universal transcoder)
# to DTEX (src/render/formats/de_cooked_texture.h) with linear-space mips. Cooker-library code like
# the model importer: Debug and Dev builds and the tools, never Release or Shipping. Entry:
# "cook/importers/texture/texture_import.h". tinyexr and basisu are tools-only libraries; the
# importer reaches their C++ only through tinyexr's C declarations and texture_basis.cpp
# (DENG-2 Q1). The filter, sRGB and encode files are built without FMA contraction, so a cook's
# float results do not depend on the compiler's contraction choices.
set(_de_texture_importer_sources
    importers/texture/texture_import.c
    importers/texture/texture_decode.c
    importers/texture/texture_ktx2.c
    importers/texture/texture_mips.c
    importers/texture/texture_encode.c)
target_sources(de_cooker PRIVATE
    importers/texture/texture_import.h
    importers/texture/texture_internal.h
    ${_de_texture_importer_sources}
    importers/texture/texture_basis.cpp)
set_source_files_properties(${_de_texture_importer_sources} PROPERTIES
    COMPILE_OPTIONS "${DE_DETERMINISTIC_FLAGS}")
target_link_libraries(de_cooker PRIVATE de_tp_stb de_tp_tinyexr de_tp_basisu)

# DENG-103: the mesh stage of the model importer: quantized vertices, the LOD chain and meshlets
# (DMSH v2, src/render/formats/de_cooked_mesh.h) through meshoptimizer, a tools-only C++ library
# called through its C API (DENG-2 Q1). Only de_cooker links it, so Release and Shipping never
# contain it (cooker_link_check). The stage's own files compile without FMA contraction
# (DE_DETERMINISTIC_FLAGS), like meshoptimizer, so their float decisions are the same on hosts
# whose compilers contract by default.
set(_de_mesh_stage_sources
    importers/model/mesh_cook.c
    importers/model/mesh_lod.c
    importers/model/mesh_meshlets.c
    importers/model/mesh_quantize.c)
target_sources(de_cooker PRIVATE ${_de_mesh_stage_sources})
set_source_files_properties(${_de_mesh_stage_sources} TARGET_DIRECTORY de_cooker
    PROPERTIES COMPILE_OPTIONS "${DE_DETERMINISTIC_FLAGS}")
target_link_libraries(de_cooker PRIVATE de_tp_meshoptimizer)

# DENG-104: skeletons and animation clips of the model importer (tools/cook/importers/model/):
# skins → skeleton sub-assets (DSKL), animations → compressed clip sub-assets (DCLP). The cooked
# formats are header-only in the anim module (src/anim/formats/); the reference sampler is
# src/anim/formats/clip_decode.c in de_anim. anim_clip_compress.c evaluates its error with the
# sampler's inline helpers, so it gets the deterministic flags (no FMA contraction) like de_anim.
target_sources(de_cooker PRIVATE
    importers/model/anim_cook.h
    importers/model/anim_skeleton.c
    importers/model/anim_clip_compress.c)
set_source_files_properties(importers/model/anim_clip_compress.c TARGET_DIRECTORY de_cooker
    PROPERTIES COMPILE_OPTIONS "${DE_DETERMINISTIC_FLAGS}")

# DENG-100: HDR environments of the texture importer (`usage = "environment"`): the source
# (equirectangular, cube cross or KTX2 cube) to a cube with GGX-prefiltered mips and SH9
# irradiance, cooked as a DTEX cube plus an ENVS trailer (src/render/formats/de_cooked_env.h).
# The work runs on the cooker's task system in fixed (face, row) chunks; the files compile
# without FMA contraction so a cook is the same bytes on every host and worker count.
set(_de_env_importer_sources
    importers/texture/env_prefilter.c
    importers/texture/env_sh.c)
target_sources(de_cooker PRIVATE importers/texture/env_internal.h ${_de_env_importer_sources})
set_source_files_properties(${_de_env_importer_sources} TARGET_DIRECTORY de_cooker
    PROPERTIES COMPILE_OPTIONS "${DE_DETERMINISTIC_FLAGS}")

# DENG-99: block compression of the texture importer (tools/cook/importers/texture/texture_bc*):
# the `compression` rules (texture_bc_rules.c), the BC encode stage (texture_bc.c, fixed block
# ranges on the cooker's task system) and the C++ encoders behind texture_bc_enc.cpp: bc7enc_rdo
# (BC1/BC3/BC4/BC5/BC7 and the rate-distortion pass) and the Basis Universal transcoder's BC6H
# encoder (de_tp_basisu, already linked above). Tools-only libraries, linked only into de_cooker
# (DENG-2 Q1). The C files are built without FMA contraction, like the rest of the importer.
set(_de_texture_bc_sources
    importers/texture/texture_bc.c
    importers/texture/texture_bc_rules.c)
target_sources(de_cooker PRIVATE
    importers/texture/texture_bc.h
    ${_de_texture_bc_sources}
    importers/texture/texture_bc_enc.cpp)
set_source_files_properties(${_de_texture_bc_sources} importers/texture/texture_bc_enc.cpp
    TARGET_DIRECTORY de_cooker
    PROPERTIES COMPILE_OPTIONS "${DE_DETERMINISTIC_FLAGS}")
target_link_libraries(de_cooker PRIVATE de_tp_bc7enc_rdo)

# DENG-394: `de-cook cook --game <lib>` loads a game library with the host's loader
# (src/host/game_module.c, "host/game_module.h") and calls its register_types. A game library
# binds to the engine in the host process (cmake/DeAddGame.cmake), so where de_add_game() builds
# libraries with a static engine (macOS, Linux), de-cook links the whole engine and exports it,
# as the `dengine` executable does. The whole archive comes after de_cook_check and de_cook_cmd
# on the link line, before the module libraries they depend on, so no module object is linked
# twice. On Windows without DE_BUILD_SHARED nothing is exported (like `dengine` there), and a
# DE_BUILD_SHARED de-cook refuses --game (DE_COOK_SHARED_ENGINE; see cook_cmd.h).
target_link_libraries(de_cook_cmd PRIVATE de_host)
if(DE_BUILD_SHARED)
    target_compile_definitions(de_cook_cmd PRIVATE DE_COOK_SHARED_ENGINE=1)
elseif(DE_GAME_DYLIB)
    target_link_libraries(de-cook PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,dengine>")
    set_target_properties(de-cook PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        target_link_options(de-cook PRIVATE "LINKER:-export_dynamic")
    endif()
endif()

# DENG-409: the cooker library's built-in importer table (importers/engine_importers.c,
# de_cooker_builtin_importer in lib/de_cooker.h): texture, model and shader. `de-cook cook`
# registers each one the project did not register; the runtime `shader` type of Debug and Dev
# builds takes its importer from it. The shader importer itself joined de_cooker in
# importers/shader/CMakeLists.txt (added above).
target_sources(de_cooker PRIVATE importers/engine_importers.c)

# DENG-178: `de-cook cook` registers the render components (de_render_register_components) in its
# tool world, so the prefabs of glTF models keep DeMeshRenderer. Not in headless builds (no
# de_render; cook_cmd.c tests DE_HAS_RENDER).
target_link_libraries(de_cook_cmd PRIVATE "$<TARGET_NAME_IF_EXISTS:de_render>")

# DENG-302: the `material` importer for `.mat.toml` files (importers/material/): a material written
# by hand, cooked to DMAT v2 (render/formats/de_cooked_material.h); its texture asset IDs are cook
# dependencies. TOML through core/de_toml_internal.h (de_core); de_tp_tomlc17 for its header.
target_sources(de_cooker PRIVATE
    importers/material/mat_toml_import.h
    importers/material/mat_toml_import.c)

# DENG-255: font importer; engine-owned code remains C11.
target_sources(de_cooker PRIVATE
    "${DE_ROOT_DIR}/src/text/import/font_import.c"
    "${DE_ROOT_DIR}/src/text/import/charsets.c"
    "${DE_ROOT_DIR}/src/text/import/atlas_build.c"
    "${DE_ROOT_DIR}/src/text/import/bmfont_import.c"
    "${DE_ROOT_DIR}/src/text/import/pixel_font_import.c")
target_link_libraries(de_cooker PRIVATE de_text_msdf de_tp_stb de_tp_tomlc17)
