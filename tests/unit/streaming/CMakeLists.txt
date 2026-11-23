# tests/unit/streaming: the world partition (src/world/streaming, DENG-271). CTest names
# streaming.<test>.
#   test_cell_grid.c       cell math: coordinates, names, neighbour order, camera-relative origins
#   test_world_cook.c      `de-cook world` (tools/cook/world_cells.c): determinism, the golden
#                          fixture tests/assets/world_small/, manifest checks. Tools builds only.
#   test_world_partition.c streaming over a real asset system (SOURCE mode, in-process cooker):
#                          load order, per-frame cap, hysteresis, memory, far origin, proxies.
set(_de_streaming_tests test_cell_grid.c test_world_partition.c)
set(_de_streaming_libs de_streaming de_asset de_ecs de_reflect de_task de_platform)
if(DE_BUILD_TOOLS)
    list(APPEND _de_streaming_tests test_world_cook.c)
endif()
de_add_test(NAME streaming SOURCES ${_de_streaming_tests} LIBS ${_de_streaming_libs} TIMEOUT 300)
if(TARGET test_streaming)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_streaming_tmp")
    target_include_directories(test_streaming PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_streaming PRIVATE
        "DE_STREAMING_FIXTURE_DIR=\"${DE_ROOT_DIR}/tests/assets/world_small\""
        "DE_STREAMING_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_streaming_tmp\"")
    if(DE_BUILD_TOOLS)
        target_compile_definitions(test_streaming PRIVATE DE_STREAMING_HAS_WORLD_COOK=1)
        # de_cook_world is defined later (tools/ sorts after tests/); resolved at generate time.
        target_link_libraries(test_streaming PRIVATE de_cook_world
            "$<$<CONFIG:Debug,Dev>:de_cooker>")
    endif()
endif()
