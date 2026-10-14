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
