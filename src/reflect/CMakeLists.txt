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
    DEPS core
)
