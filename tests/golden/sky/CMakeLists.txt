# DENG-837 (DENG-206 AC1): the physical sky at latitude 45 degrees at sunrise, noon, sunset and
# night, over a lit ground slab and four columns (sky/assets/, SOURCE assets cooked in-process; the
# scenes skip without the cooker). References: Metal on the macOS host; the other backends are
# needs-hw-verify until CI or a reviewer adds theirs (--update-refs on that backend).
target_sources(de_golden PRIVATE sky_scenes.c)
target_link_libraries(de_golden PRIVATE de_sky)
target_compile_definitions(de_golden PRIVATE
    "DE_GOLDEN_SKY_ASSETS=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\""
    "DE_GOLDEN_SKY_CACHE=\"${CMAKE_CURRENT_BINARY_DIR}/cache\"")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cache")
foreach(_backend IN ITEMS metal vulkan d3d12)
    foreach(_scene IN ITEMS sky_sunrise sky_noon sky_sunset sky_night)
        if(TEST golden.${_backend}.${_scene})
            set_tests_properties(golden.${_backend}.${_scene} PROPERTIES
                LABELS "golden;sky;gpu")
            if(NOT _backend STREQUAL "metal")
                set_property(TEST golden.${_backend}.${_scene} APPEND PROPERTY
                    LABELS "needs-hw-verify")
            endif()
        endif()
    endforeach()
endforeach()
