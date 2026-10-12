# tests/fuzz/reflect: fuzz targets of module `reflect` (DENG-297, SPEC §19.1). Scenes, prefabs
# and save games go through the text reader (DENG-295) and the binary reader (DENG-296), so both
# are attack surface (mods, downloaded saves). Each target replays its seed corpus under CTest
# (label `fuzz`); see cmake/DeTesting.cmake and the Trama page "Module: reflect" for real fuzzing
# with libFuzzer.
#
#   fuzz_reflect_text      de_deserialize_object(DE_SER_TEXT) into every test type
#   fuzz_reflect_text_doc  de_text_doc_parse / _count / _type / _read_object (multi-object files)
#   fuzz_reflect_binary    de_deserialize_binary / de_deserialize_object(DE_SER_BINARY)
#
# The targets share fuzz_reflect.h and the test types of tests/unit/reflect/test_types.h.
# Corpora: the golden files of tests/unit/reflect/data, objects written by the serializers for
# every test type (both schema modes, filters, mapped IDs, the "next" layouts) and hand-made edge
# cases (deep nesting, huge arrays, long strings, bad UTF-8, version skew).
de_add_fuzz_target(NAME reflect_text SOURCES fuzz_text_reader.c LIBS de_reflect
                   CORPUS corpus/text)
de_add_fuzz_target(NAME reflect_text_doc SOURCES fuzz_text_reader.c LIBS de_reflect
                   CORPUS corpus/text_doc)
de_add_fuzz_target(NAME reflect_binary SOURCES fuzz_binary_reader.c LIBS de_reflect
                   CORPUS corpus/binary)
foreach(_de_fuzz IN ITEMS fuzz_reflect_text fuzz_reflect_text_doc fuzz_reflect_binary)
    if(TARGET ${_de_fuzz})
        target_include_directories(${_de_fuzz} PRIVATE "${DE_ROOT_DIR}/tests/unit/reflect")
    endif()
endforeach()
if(TARGET fuzz_reflect_text_doc)
    target_compile_definitions(fuzz_reflect_text_doc PRIVATE FZ_TEXT_DOC=1)
endif()
