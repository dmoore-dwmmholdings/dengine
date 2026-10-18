de_add_module(NAME text LAYER gameplay SOURCES text_system.c DEPS asset)
# The same reentrant MSDF C interface is used by import and runtime glyphs.
add_library(de_text_msdf STATIC msdf/msdf_wrap.cpp)
target_include_directories(de_text_msdf PUBLIC "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/include")
target_link_libraries(de_text_msdf PRIVATE de_tp_msdfgen de_build_flags PUBLIC de_config)
set_target_properties(de_text_msdf PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(de_text PRIVATE font_runtime.c fallback.c glyph_lookup.c)
target_link_libraries(de_text PRIVATE de_tp_stb)

target_include_directories(de_text PRIVATE "${DE_ROOT_DIR}/tools")

target_sources(de_text PRIVATE shape/itemize.c shape/shaper.c shape/shape_cache.c)
target_link_libraries(de_text PRIVATE de_tp_harfbuzz de_tp_xxhash)

target_sources(de_text PRIVATE glyph_cache.c atlas_packer.c)
target_link_libraries(de_text PRIVATE de_text_msdf de_task)
# The aggregate consumes module objects rather than de_text's link interface.
set_property(TARGET de_text APPEND PROPERTY DE_MODULE_LIBS
    de_tp_stb de_tp_harfbuzz de_tp_xxhash de_text_msdf)

# CPU text remains available in headless builds; these symbols provide the documented no-op attach.
target_sources(de_text PRIVATE render/text_render.c)
if(DE_HAS_RENDER)
 target_sources(de_text PRIVATE render/atlas_upload.c render/world_text.c)
 target_link_libraries(de_text PRIVATE de_render de_gpu de_ecs)
endif()
