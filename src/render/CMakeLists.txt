# src/render: module `render` (target de_render), layer content. SPEC §12.
# Sources are listed one per line; S12 tasks append theirs (DENG-174 renderer + settings,
# DENG-175 render world, DENG-176 views, DENG-177 GPU scene, DENG-178 components).
# graph/: the render graph (DENG-171 builder; DENG-172 compiler and DENG-173 recorder append).
de_add_module(
    NAME render
    LAYER content
    SOURCES
        graph/rg_internal.h
        graph/rg_builder.c
        graph/rg_debug.c
        graph/rg_compile.c
        graph/rg_schedule.c
        graph/rg_barriers.c
        graph/rg_alias.c
        graph/rg_cache.c
        graph/rg_execute.c
        graph/rg_timing.c
    DEPS gpu asset ecs task
)

# DENG-105: the runtime `texture`, `mesh`, `material` and `model` asset types and their GPU upload
# path (loaders/: the staging ring and the per-frame copy list, the four loaders,
# de_render_register_asset_types; render_assets.h is the internal API S12 calls). In Debug and Dev
# builds with the cooker (DE_HAS_COOKER, the same condition as de_asset) the `texture` and `model`
# types also get their importers, so de_render links de_cooker there and includes the importer
# headers through the tools/ root (tools/check_layers/layers.toml: render -> cook/importers).
if(TARGET de_render) # not built in headless builds (DE_HEADLESS)
    target_sources(de_render PRIVATE
        loaders/render_assets.h
        loaders/upload_ring.h
        loaders/upload_ring.c
        loaders/loaders_internal.h
        loaders/texture_loader.c
        loaders/mesh_loader.c
        loaders/material_loader.c
        loaders/loaders.c)
    set(_de_render_with_cooker "$<AND:$<CONFIG:Debug,Dev>,$<TARGET_EXISTS:de_cooker>>")
    target_link_libraries(de_render PRIVATE "$<${_de_render_with_cooker}:de_cooker>")
    set_property(TARGET de_render APPEND PROPERTY DE_MODULE_LIBS "$<${_de_render_with_cooker}:de_cooker>")
    target_include_directories(de_render PRIVATE "${DE_ROOT_DIR}/tools")
endif()

# DENG-174: the renderer (renderer.c: create/destroy, attach, the frame skeleton; DENG-176 replaces
# the frame graph body) and its settings (settings.c: the quality preset tables, the clamp to the
# device caps, the caps-only auto selection DENG-276 builds on, the r.* cvars).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        renderer_internal.h
        renderer.c
        settings.c)
endif()

# DENG-178: the render components of de_render.h (DeCamera, DeMeshRenderer, DeLight, ...):
# reflection, construct hooks and ECS registration (components.c; de_render_register_components,
# called by de_renderer_attach and by de-cook's tool world). components_internal.h holds the
# per-world component ids (the DeRenderComponents resource) for Extract (DENG-175).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        components_internal.h
        components.c)
endif()

# DENG-175: the render world (world/render_world.c: the f32 render-world buffers, slot pools and the
# render origin), the Extract systems (world/extract.c, added by de_renderer_attach) and the render
# thread with the Extract handoff (world/render_thread.c; de__render_extract_and_kick, called by the
# host main loop at step 5).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        world/render_world.h
        world/render_world.c
        world/extract.c
        world/render_thread.c)
endif()

# DENG-176: views and the per-view frame pipeline (views.c, views_internal.h: the view table, the
# pass-registration list, de__views_render, called by de_renderer_render_frame) and the view
# composite (passes/composite.c; shaders/common/composite.slang, the engine shader "composite",
# loaded through DENG-421's engine shaders below; tools/check_layers/layers.toml: render -> cook,
# render -> gpu/shader).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        views_internal.h
        views.c
        passes/composite.c)
    set(_de_render_shader_lib
        "$<$<AND:${_de_render_with_cooker},$<TARGET_EXISTS:de_gpu_shader>>:de_gpu_shader>")
    target_link_libraries(de_render PRIVATE "${_de_render_shader_lib}")
    set_property(TARGET de_render APPEND PROPERTY DE_MODULE_LIBS "${_de_render_shader_lib}")
endif()

# DENG-177: the GPU scene (scene/gpu_table.c: one persistent table with its slot allocator, dirty
# set and GPU-side growth; scene/gpu_scene.c: the instance, mesh, material, light and per-view
# tables mirrored from the render world, staged in the DENG-105 upload ring and uploaded by
# render-graph passes; the Slang side is shaders/scene/). de_renderer_create makes one and
# de_renderer_render_frame drives it: the uploads once per frame before the views, the scene
# constants in a stage of each view's graph (DENG-176).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        scene/gpu_scene.h
        scene/gpu_table.c
        scene/gpu_scene.c)
    # The scatter kernels are the engine shader "scene_scatter" (DENG-421, below).
endif()

# DENG-421: engine shaders in every build type (shaders/engine_shaders.cmake, one line per shader;
# shaders/engine_shaders.h has the load order). At build time tools/engine_shaders/
# (de-engine-shaders) cooks each listed shader for this platform's default targets, through the
# build tree's .decache (DE_RENDER_ENGINE_SHADER_CACHE), and shaders/embed_engine_shaders.cmake
# embeds the blobs and the table in de_render (engine_shaders_data.c). That needs the tools, Slang
# and a host that can run them (not a cross build); otherwise the table is embedded without blobs.
# Debug and Dev builds with the cooker also cook the source in-process through the same cache
# (DE_RENDER_ENGINE_SHADER_ROOT). The cooked format is read through de_gpu_shader in every build.
if(TARGET de_render)
    set(_de_esh_dir "${CMAKE_CURRENT_BINARY_DIR}/engine_shaders")
    set(_de_esh_cache "${CMAKE_BINARY_DIR}/engine_shaders/decache")
    file(MAKE_DIRECTORY "${_de_esh_dir}" "${_de_esh_cache}")
    set(_de_esh_cook OFF)
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND NOT CMAKE_CROSSCOMPILING)
        set(_de_esh_cook ON)
    endif()
    if(NOT _de_esh_cook)
        message(STATUS "Engine shaders: not cooked at build time (tools off, no Slang, or a cross "
                       "build): only Debug and Dev builds with the cooker have the renderer's own "
                       "shaders (DENG-421)")
    endif()
    set(DE_ENGINE_SHADERS_BUILD_COOK ${_de_esh_cook} CACHE INTERNAL
        "DENG-421: engine shaders are cooked at build time and embedded")

    set(_de_esh_names "")
    set(_de_esh_sources "")
    set(_de_esh_perms "")
    macro(de_engine_shader _name _source)
        cmake_parse_arguments(_de_esh "" "" "PERMUTATIONS" ${ARGN})
        list(APPEND _de_esh_names "${_name}")
        list(APPEND _de_esh_sources "${_source}")
        if(_de_esh_PERMUTATIONS)
            string(REPLACE ";" "/" _de_esh_p "${_de_esh_PERMUTATIONS}")
        else()
            set(_de_esh_p "-")
        endif()
        list(APPEND _de_esh_perms "${_de_esh_p}")
    endmacro()
    include("${DE_ROOT_DIR}/shaders/engine_shaders.cmake")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${DE_ROOT_DIR}/shaders/engine_shaders.cmake")
    file(GLOB_RECURSE _de_esh_slang CONFIGURE_DEPENDS "${DE_ROOT_DIR}/shaders/*.slang")

    set(_de_esh_blobs "")
    if(_de_esh_cook)
        list(LENGTH _de_esh_names _de_esh_n)
        math(EXPR _de_esh_last "${_de_esh_n} - 1")
        foreach(_i RANGE ${_de_esh_last})
            list(GET _de_esh_names ${_i} _name)
            list(GET _de_esh_sources ${_i} _source)
            list(GET _de_esh_perms ${_i} _p)
            set(_args "")
            if(NOT _p STREQUAL "-")
                string(REPLACE "/" ";" _plist "${_p}")
                foreach(_one IN LISTS _plist)
                    list(APPEND _args --permutation "${_one}")
                endforeach()
            endif()
            add_custom_command(
                OUTPUT "${_de_esh_dir}/${_name}.dshd"
                COMMAND de_engine_shaders_tool --root "${DE_ROOT_DIR}/shaders"
                        --cache "${_de_esh_cache}" --name "${_name}" --source "${_source}"
                        --out "${_de_esh_dir}/${_name}.dshd" ${_args}
                DEPENDS de_engine_shaders_tool ${_de_esh_slang}
                COMMENT "Cooking engine shader ${_name} (${_source})"
                VERBATIM)
            list(APPEND _de_esh_blobs "${_de_esh_dir}/${_name}.dshd")
        endforeach()
    endif()
    file(CONFIGURE OUTPUT "${_de_esh_dir}/engine_shaders_list.cmake" CONTENT
        "set(ESH_NAMES \"${_de_esh_names}\")\nset(ESH_SOURCES \"${_de_esh_sources}\")\nset(ESH_PERMS \"${_de_esh_perms}\")\nset(ESH_DIR \"${_de_esh_dir}\")\nset(ESH_EMBED ${_de_esh_cook})\n")
    add_custom_command(
        OUTPUT "${_de_esh_dir}/engine_shaders_data.c"
        COMMAND "${CMAKE_COMMAND}" "-DLIST=${_de_esh_dir}/engine_shaders_list.cmake"
                "-DOUT=${_de_esh_dir}/engine_shaders_data.c"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_engine_shaders.cmake"
        DEPENDS ${_de_esh_blobs} "${_de_esh_dir}/engine_shaders_list.cmake"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/embed_engine_shaders.cmake"
        COMMENT "Embedding the engine shaders"
        VERBATIM)
    target_sources(de_render PRIVATE
        shaders/engine_shaders.h
        shaders/engine_shaders.c
        "${_de_esh_dir}/engine_shaders_data.c")
    target_compile_definitions(de_render PRIVATE
        "DE_RENDER_ENGINE_SHADER_ROOT=\"${DE_ROOT_DIR}/shaders\""
        "DE_RENDER_ENGINE_SHADER_CACHE=\"${_de_esh_cache}\"")
    target_link_libraries(de_render PRIVATE "$<TARGET_NAME_IF_EXISTS:de_gpu_shader>")
    set_property(TARGET de_render APPEND PROPERTY DE_MODULE_LIBS
        "$<TARGET_NAME_IF_EXISTS:de_gpu_shader>")
endif()

# DENG-229: the visibility-buffer frame (passes/vis/: vis_frame.c registers the view stage
# "vis.frame" and calls the stages of vis_stages.h in order; cull_instances.c is GPU instance
# culling; vis_stages_stub.c holds the stages later S13 tasks implement). Its shaders are engine
# shaders (shaders/engine_shaders.cmake, DENG-421): vis_instance_cull is
# shaders/culling/instance_cull.slang, which imports shaders/scene/ and shaders/visbuffer/.
if(TARGET de_render)
    target_sources(de_render PRIVATE
        passes/vis/vis_stages.h
        passes/vis/vis_frame.c
        passes/vis/vis_stages_stub.c
        passes/vis/cull_instances.c)
endif()

# DENG-231: GPU meshlet culling (passes/vis/cull_meshlets.c, the cull_meshlets stage of
# vis_stages.h): frustum, backface-cone and small-meshlet tests per meshlet of each work item, the
# visible-meshlet list and the raster arguments of both phases. Its shader is the engine shader
# vis_meshlet_cull (shaders/culling/meshlet_cull.slang).
if(TARGET de_render)
    target_sources(de_render PRIVATE passes/vis/cull_meshlets.c)
endif()

# DENG-233: the HZB of each view (passes/vis/hzb.c; engine shader vis_hzb_build,
# shaders/culling/hzb_build.slang) and two-phase occlusion culling (passes/vis/occlusion.c: the
# per-view visibility flags, camera-cut detection, the r.hzb_debug view; engine shader
# vis_occlusion, shaders/culling/occlusion.slang, which cull_common.slang imports for
# cull_occlusion_test).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        passes/vis/hzb.c
        passes/vis/occlusion.c)
endif()
