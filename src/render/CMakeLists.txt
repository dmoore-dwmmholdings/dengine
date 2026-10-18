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

# DENG-234: the mesh-shader vis-buffer raster (passes/vis/raster_mesh.c; engine shader vis_mesh,
# shaders/visbuffer/vis_mesh.slang, which imports shaders/visbuffer/vis_decode.slang). The raster
# stage in vis_stages_stub.c picks the mesh or the compute path (r.raster_path).
if(TARGET de_render)
    target_sources(de_render PRIVATE passes/vis/raster_mesh.c)
endif()

# DENG-235: the compute-expansion raster path (passes/vis/raster_compute.c), the fallback where
# mesh shaders are missing (and on the CI software rasterizers): per visible meshlet a compute
# group culls triangles and appends them to a triangle buffer (engine shader vis_raster_expand,
# shaders/visbuffer/vis_expand.slang), then one indexed indirect draw per phase writes the vis ID
# and depth (engine shader vis_raster_draw, shaders/visbuffer/vis_vs_ps.slang). Both import
# DENG-234's shaders/visbuffer/vis_decode.slang.
if(TARGET de_render)
    target_sources(de_render PRIVATE passes/vis/raster_compute.c)
endif()

# DENG-236: the alpha-tested raster (passes/vis/raster_alpha.c; engine shader vis_alpha,
# shaders/visbuffer/vis_alpha.slang): mask materials' meshlets, routed to their own bucket by
# meshlet culling, drawn with an alpha test after each phase's opaque raster, on both paths.
if(TARGET de_render)
    target_sources(de_render PRIVATE passes/vis/raster_alpha.c)
endif()

# DENG-237: material classification (passes/vis/classify.c): the vis IDs copied into a raw buffer,
# then one group per screen tile bins the tiles by material model (DeGpuMaterial.model) into
# per-model tile lists with indirect dispatch arguments for resolve (DENG-238). Engine shader
# vis_classify (shaders/visbuffer/classify.slang).
if(TARGET de_render)
    target_sources(de_render PRIVATE passes/vis/classify.c)
endif()

# DENG-239: vis-buffer picking (passes/vis/picking.c; engine shader vis_pick,
# shaders/visbuffer/pick.slang): de_view_request_pick / de_view_pick_result read the entity and
# triangle under a pixel back from the view's vis buffer, one to three frames later.
if(TARGET de_render)
    target_sources(de_render PRIVATE passes/vis/picking.c)
endif()

# DENG-238: the material resolve (passes/vis/resolve.c, gbuffer.h): per material model, an
# indirect dispatch over classification's tile list reconstructs each pixel's surface and writes
# the thin G-buffer, motion vectors, emissive and the texture-streaming feedback. Engine shader
# vis_resolve (shaders/visbuffer/resolve_standard.slang, which imports resolve_common.slang,
# gbuffer.slang and feedback.slang).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        passes/vis/gbuffer.h
        passes/vis/resolve.c)
endif()

# DENG-298: the material model registry (materials/material_models.c: the built-in ids, game
# models from de_material_model_register with their resolve pipelines, the material feature
# flags). The Slang side: shaders/materials/material_interface.slang (IMaterialModel) and the BSDF
# modules of shaders/brdf/; the resolve kernel is generic over the model
# (shaders/visbuffer/resolve_model.slang).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        materials/material_models.h
        materials/material_models.c)
endif()

# DENG-299: the Standard material model's energy LUT (materials/brdf_luts.c: the split-sum DFG and
# the multi-scatter averages, generated at renderer start by the engine shader brdf_lut_gen,
# shaders/brdf/lut_gen.slang, and a CPU reference). The model itself is
# shaders/materials/standard/ (standard.slang, standard_base.slang) on shaders/brdf/energy.slang.
if(TARGET de_render)
    target_sources(de_render PRIVATE
        materials/brdf_luts.h
        materials/brdf_luts.c)
endif()

# DENG-303: clustered lighting (lighting/: lighting.h the internal API; clusters.c the froxel bounds
# and light lists, light_data.c their arithmetic in C, lighting_pass.c the stage "lighting" and
# the deferred pass over the thin G-buffer, ltc_data.c the GGX LTC tables generated by
# tools/ltc_fit/ltc_fit.c, lighting_inputs.h the shadow / AO / indirect inputs contract). Engine
# shaders lighting_clusters, lighting_cull and lighting_deferred (shaders/lighting/).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        lighting/lighting.h
        lighting/lighting_inputs.h
        lighting/light_data.c
        lighting/clusters.c
        lighting/lighting_pass.c
        lighting/ltc_data.c)
endif()

# DENG-304: image-based lighting (lighting/ibl.c: the environment snapshot and its 2D strip;
# shaders/lighting/ibl.slang), physical exposure (lighting/exposure.c: EV100, per-view current and
# previous exposure) and the basic output pass (passes/output_basic.c; engine shader
# post_output_basic, shaders/post/output_basic.slang) until the S18 tonemappers.
if(TARGET de_render)
    target_sources(de_render PRIVATE
        lighting/ibl.c
        lighting/exposure.c
        passes/output_basic.c)
endif()

# DENG-302: material instances (materials/material_instances.c: the 256-byte parameter block of
# materials/material_params.h and shaders/materials/material_params.slang, the feature bits and
# resolve permutations, the runtime instance pool, de_material_set_params and friends). The GPU
# scene packs each referenced material's block (scene/gpu_scene.c); the `.mat.toml` importer is
# cooker code (tools/cook/importers/material/).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        materials/material_params.h
        materials/material_instances.h
        materials/material_instances.c)
endif()

# DENG-305: the forward transparent pass (passes/forward/: forward.h the internal API;
# transparent_sort.c the per-view list and its back-to-front sort, forward_transparent.c the stage
# "forward.transparent" in DE__VIEW_SLOT_TRANSPARENT and its draws). Engine shader
# forward_transparent (shaders/forward/forward_transparent.slang); meshlet culling keeps forward
# materials out of the vis buffer through shaders/forward/forward_routing.slang.
if(TARGET de_render)
    target_sources(de_render PRIVATE
        passes/forward/forward.h
        passes/forward/transparent_sort.c
        passes/forward/forward_transparent.c)
endif()

# DENG-306: screen-space separable subsurface scattering with the Burley profile
# (passes/sss/sss.c: the profile in double precision, the kernel tables, the view stage "sss"
# after lighting; engine shader sss_separable, shaders/sss/sss_separable.slang with the pure
# profile functions of shaders/sss/burley_profile.slang that the path tracer reuses, DENG-73).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        passes/sss/sss.h
        passes/sss/sss.c)
endif()

# DENG-340: shared probe capture, cube pool and versioned bake infrastructure.
if(TARGET de_render)
    target_sources(de_render PRIVATE probes/probe_capture.c probes/probe_store.c probes/probe_bake.c probes/reflection_probes.c)
endif()

# DENG-339: reusable final-depth pyramid and lit HDR scene history, with SSR consumers.
if(TARGET de_render)
    target_sources(de_render PRIVATE passes/ssr/scene_history.c passes/ssr/ssr.c)
endif()

# DENG-341: DDGI-compatible irradiance volumes and screen-space diffuse GI.
if(TARGET de_render)
    target_sources(de_render PRIVATE probes/irradiance_volume.c)
endif()

if(TARGET de_render)
    target_sources(de_render PRIVATE passes/ssgi/ssgi.c)
endif()

if(TARGET de_render)
    target_sources(de_render PRIVATE shadows/shadows_internal.h shadows/shadow_views.c)
endif()
if(TARGET de_render)
    target_sources(de_render PRIVATE shadows/csm.c)
endif()

if(TARGET de_render)
    target_sources(de_render PRIVATE passes/ao/gtao.c passes/ao/gtao_internal.h)
endif()

# DENG-342: current-frame opaque HDR Gaussian pyramid for refraction.
if(TARGET de_render)
    target_sources(de_render PRIVATE passes/forward/scene_color_pyramid.c passes/forward/scene_color_pyramid.h)
endif()

if(TARGET de_render)
    target_sources(de_render PRIVATE shadows/atlas_alloc.c shadows/shadow_atlas.c)
endif()

if(TARGET de_render)
    target_sources(de_render PRIVATE shadows/contact_shadows.c shadows/contact_internal.h)
endif()

# DENG-44: renderer-owned RT acceleration structures and common query infrastructure.
if(TARGET de_render)
    target_sources(de_render PRIVATE rt/scene_as.c rt/rt_frame.c rt/rt_internal.h)
endif()
if(TARGET de_render AND APPLE AND DE_GPU_METAL)
    set(_de_rt_library "${CMAKE_CURRENT_BINARY_DIR}/rt_trace.metallib")
    set(_de_rt_embed "${CMAKE_CURRENT_BINARY_DIR}/rt_trace_metal.c")
    add_custom_command(OUTPUT "${_de_rt_library}"
        COMMAND xcrun metal -std=metal3.1
            "-fmodules-cache-path=${CMAKE_CURRENT_BINARY_DIR}/rt-metal-cache"
            "${DE_ROOT_DIR}/shaders/rt/trace_metal.metal" -o "${_de_rt_library}"
        DEPENDS "${DE_ROOT_DIR}/shaders/rt/trace_metal.metal"
            "${DE_ROOT_DIR}/shaders/rt/rt_opacity_metal.generated" VERBATIM)
    add_custom_command(OUTPUT "${_de_rt_embed}"
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${_de_rt_library}" "-DOUT=${_de_rt_embed}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/rt/embed_metal.cmake"
        DEPENDS "${_de_rt_library}" "${CMAKE_CURRENT_SOURCE_DIR}/rt/embed_metal.cmake" VERBATIM)
    target_sources(de_render PRIVATE "${_de_rt_embed}")
endif()

if(TARGET de_render)
    target_sources(de_render PRIVATE rt/rt_shadows.c rt/rt_shadows.h)
endif()

if(TARGET de_render)
    target_sources(de_render PRIVATE rt/restir_di.c rt/restir_di.h)
endif()

# DENG-47: shared-opacity ray-traced ambient visibility.
if(TARGET de_render)
    target_sources(de_render PRIVATE rt/rtao.c rt/rtao.h)
endif()

if(TARGET de_render)
    target_sources(de_render PRIVATE rt/rt_reflections.c rt/rt_reflections.h)
endif()

if(TARGET de_render)
    target_sources(de_render PRIVATE rt/rt_gi_probes.c rt/rt_gi_probes.h)
endif()

if(TARGET de_render)
    target_sources(de_render PRIVATE rt/rt_refraction.c rt/rt_refraction.h)
endif()
