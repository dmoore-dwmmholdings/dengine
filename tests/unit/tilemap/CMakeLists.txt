# tests/unit/tilemap: the tilemap importer (tools/cook/importers/tilemap/) and runtime
# (src/gameplay/tilemap/), DENG-80. CTest names tilemap.<test>. The importer is cooker code, so
# the tests exist only when tools are built; the runtime tests cook the fixtures in-process and
# skip in builds without the cooker (DE_HAS_COOKER: Debug and Dev). Fixtures: tests/data/tilemap/
# (generate.py), mounted read-only as tmd:/.
if(NOT DE_BUILD_TOOLS)
    return()
endif()

de_add_test(NAME tilemap
    SOURCES test_tilemap_import.c test_tilemap_runtime.c tilemap_test_util.h
    LIBS de_cooker de_cook_check de_tilemap de_asset de_ecs de_reflect de_task de_platform
         de_core
    TIMEOUT 120)
if(TARGET test_tilemap)
    target_include_directories(test_tilemap PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_tilemap PRIVATE
        "DE_TILEMAP_TEST_DATA_DIR=\"${DE_ROOT_DIR}/tests/data/tilemap\"")
    # DeSprite and DeCollider2D come from the renderer and 2D physics when they are built.
    target_link_libraries(test_tilemap PRIVATE
        "$<TARGET_NAME_IF_EXISTS:de_render>" "$<TARGET_NAME_IF_EXISTS:de_physics>")
endif()
