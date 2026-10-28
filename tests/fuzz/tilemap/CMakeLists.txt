# tests/fuzz/tilemap: fuzz targets of the tilemap importer (DENG-80, SPEC §19.1): the strict JSON
# reader, the TMX-subset XML reader, and the LDtk and Tiled front ends through the whole import
# (each cooked result is checked with the runtime's parser). Each replays its seed corpus
# corpus/<target>/ under CTest (label `fuzz`). The importer is cooker code: tools builds only.
if(NOT DE_BUILD_TOOLS)
    return()
endif()

foreach(_de_fz IN ITEMS tilemap_json tilemap_xml ldtk tiled)
    de_add_fuzz_target(NAME ${_de_fz} SOURCES fuzz_${_de_fz}.c LIBS de_cooker de_asset de_platform
                       CORPUS corpus/${_de_fz})
    if(TARGET fuzz_${_de_fz})
        target_include_directories(fuzz_${_de_fz} PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    endif()
endforeach()
