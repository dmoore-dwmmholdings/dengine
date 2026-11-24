# tests/unit/nav: navmesh generation (src/gameplay/nav/), DENG-76, and queries, DENG-77.
# CTest names nav.<test> and nav_query.<test>.
# Fixtures: tests/data/nav/, mounted read-only as navd:/. The asset test cooks in-process and
# runs its asset part only in builds with the cooker (DE_HAS_COOKER: Debug and Dev).
if(NOT TARGET de_nav)
    return()
endif()

de_add_test(NAME nav
    SOURCES test_nav.c
    LIBS de_nav de_nav_build de_asset de_ecs de_reflect de_task de_platform de_core
    TIMEOUT 180)
if(TARGET test_nav)
    target_include_directories(test_nav PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools"
        "${DE_ROOT_DIR}/tests/unit")
    target_compile_definitions(test_nav PRIVATE
        "DE_NAV_TEST_DATA_DIR=\"${DE_ROOT_DIR}/tests/data/nav\"")
    target_link_libraries(test_nav PRIVATE
        "$<TARGET_NAME_IF_EXISTS:de_physics>" "$<TARGET_NAME_IF_EXISTS:de_cooker>")
endif()

# Queries, off-mesh links and dynamic obstacles (DENG-77). CTest names nav_query.<test>.
de_add_test(NAME nav_query
    SOURCES test_nav_query.c
    LIBS de_nav de_nav_build de_ecs de_reflect de_task de_platform de_core
    TIMEOUT 180)
if(TARGET test_nav_query)
    target_include_directories(test_nav_query PRIVATE "${DE_ROOT_DIR}/src"
        "${DE_ROOT_DIR}/tests/unit")
endif()

# 2D navigation: grid A*/JPS and the polygon navmesh (DENG-79). CTest names nav2d.<test>.
de_add_test(NAME nav2d
    SOURCES test_nav2d.c
    LIBS de_nav de_platform de_core
    TIMEOUT 180)
if(TARGET test_nav2d)
    target_include_directories(test_nav2d PRIVATE "${DE_ROOT_DIR}/src"
        "${DE_ROOT_DIR}/tests/unit")
endif()

# The DeNavGrid2D system `nav2d.grid` (DENG-832). CTest names nav2d_grid.<test>.
de_add_test(NAME nav2d_grid
    SOURCES test_nav2d_grid_system.c
    LIBS de_nav de_ecs de_reflect de_platform de_core
    TIMEOUT 180)
if(TARGET test_nav2d_grid)
    target_include_directories(test_nav2d_grid PRIVATE "${DE_ROOT_DIR}/src"
        "${DE_ROOT_DIR}/tests/unit")
endif()

# The project nav settings file nav.toml (DENG-833). CTest names nav_settings.<test>.
de_add_test(NAME nav_settings
    SOURCES test_nav_settings.c
    LIBS de_nav de_nav_build de_asset de_ecs de_reflect de_task de_platform de_core
    TIMEOUT 60)
if(TARGET test_nav_settings)
    # DENG-864: the nav.tiles format tests use the internal nav_build.h.
    target_include_directories(test_nav_settings PRIVATE "${DE_ROOT_DIR}/src")
endif()
