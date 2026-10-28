# DENG-80: tilemap_lit_two_layers, the two-layer sample map of examples/scenes/tilemap/ drawn by
# the tilemap runtime through the Sprite material path, lit (SOURCE assets cooked in-process; the
# scene skips without the cooker). References: Metal on the macOS host; the other backends are
# needs-hw-verify until CI or a reviewer adds theirs (--update-refs on that backend).
target_sources(de_golden PRIVATE tilemap_scenes.c)
target_link_libraries(de_golden PRIVATE de_tilemap)
target_compile_definitions(de_golden PRIVATE
    "DE_TILEMAP_GOLDEN_ASSETS=\"${DE_ROOT_DIR}/examples/scenes/tilemap\""
    "DE_TILEMAP_GOLDEN_CACHE=\"${CMAKE_CURRENT_BINARY_DIR}/cache\"")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cache")
foreach(_backend IN ITEMS metal vulkan d3d12)
    if(TEST golden.${_backend}.tilemap_lit_two_layers)
        set_tests_properties(golden.${_backend}.tilemap_lit_two_layers PROPERTIES
            LABELS "golden;tilemap;gpu")
        if(NOT _backend STREQUAL "metal")
            set_property(TEST golden.${_backend}.tilemap_lit_two_layers APPEND PROPERTY
                LABELS "needs-hw-verify")
        endif()
    endif()
endforeach()
