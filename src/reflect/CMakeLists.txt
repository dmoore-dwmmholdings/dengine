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

# DENG-296: binary serializer (ser_binary.c) and its documented byte layout.
target_sources(de_reflect PRIVATE ser_binary_format.h ser_binary.c)

# DENG-624: inspector hints beside the registry (de_field_hints_set / _get).
target_sources(de_reflect PRIVATE hints.c)

# DENG-586: embed the reviewed Python runtime as separate short C literals (C11's 4095 limit).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/debugger_lldb.py")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/debugger_lldb.py" _de_debugger_python)
string(REPLACE "\\" "\\\\" _de_debugger_python "${_de_debugger_python}")
string(REPLACE "\"" "\\\"" _de_debugger_python "${_de_debugger_python}")
string(REPLACE "\n" "\\n\",\n\"" _de_debugger_python "${_de_debugger_python}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/debugger_lldb.inc"
    CONTENT "static const char *const de_debugger_python[] = {\n\"@_de_debugger_python@\"\n};\n" @ONLY)
target_include_directories(de_reflect PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_sources(de_reflect PRIVATE debugger.c)
