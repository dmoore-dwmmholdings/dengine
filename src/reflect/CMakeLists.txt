# src/reflect: module `reflect` (target de_reflect), layer runtime. SPEC §9.
# Shared file: later S04 tasks (DENG-295 text serializer, DENG-296 binary serializer) append
# their sources to the SOURCES list, one per line, or add target_sources() below.
de_add_module(
    NAME reflect
    LAYER runtime
    SOURCES
        reflect_internal.h
        registry.c
        validate.c
        schema_hash.c
        path.c
        ser_internal.h
        ser_common.c
        ser_text_writer.c
        ser_text_reader.c
    DEPS core
)

# DENG-295: the text serializer parses TOML with the vendored tomlc17, through core's entry point
# (core/de_toml_internal.h). Recorded in DE_MODULE_LIBS too, so the aggregate `dengine` links it.
target_link_libraries(de_reflect PRIVATE de_tp_tomlc17)
set_property(TARGET de_reflect APPEND PROPERTY DE_MODULE_LIBS de_tp_tomlc17)
