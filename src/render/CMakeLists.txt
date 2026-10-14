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
# composite (passes/composite.c; shaders/common/composite.slang). In Debug and Dev with the cooker
# the renderer cooks composite.slang in-process from DE_RENDER_SHADER_DIR with the cooker's
# built-in shader importer and reads the cooked format through de_gpu_shader
# (tools/check_layers/layers.toml: render -> cook, render -> gpu/shader).
if(TARGET de_render)
    target_sources(de_render PRIVATE
        views_internal.h
        views.c
        passes/composite.c)
    target_compile_definitions(de_render PRIVATE
        "DE_RENDER_SHADER_DIR=\"${DE_ROOT_DIR}/shaders/common\"")
    set(_de_render_shader_lib
        "$<$<AND:${_de_render_with_cooker},$<TARGET_EXISTS:de_gpu_shader>>:de_gpu_shader>")
    target_link_libraries(de_render PRIVATE "${_de_render_shader_lib}")
    set_property(TARGET de_render APPEND PROPERTY DE_MODULE_LIBS "${_de_render_shader_lib}")
endif()
