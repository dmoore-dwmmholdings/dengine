# src/gameplay/nav: module `nav` (target de_nav), layer gameplay. SPEC §21.2; DENG-76.
# Tiled navmesh generation with the vendored Recast / Detour (third_party/recastnavigation): the
# runtime de_nav (nav.c), its input from static DeColliders (nav_input.c) and the `navmesh` asset
# type (nav_asset.c). Queries and deterministic mode (nav_query.c), off-mesh links and dynamic
# obstacles (nav_obstacles.c) are DENG-77; crowds (DENG-78) come later.
# 2D grid A* / JPS and the 2D polygon navmesh (nav2d_*.c) are DENG-79: pure C, no Recast.
# The DeNavGrid2D system `nav2d.grid` (nav2d_grid_system.c) is DENG-832.
de_add_module(
    NAME nav
    LAYER gameplay
    SOURCES
        nav_internal.h
        nav.c
        nav_input.c
        nav_asset.c
        nav_query.c
        nav_obstacles.c
        nav_components.h
        nav_components.c
        nav2d_internal.h
        nav2d_grid.c
        nav2d_mesh.c
        nav2d_grid_system.c
    DEPS ecs asset
    OPTIONAL_DEPS physics
    OPTIONAL_OPTION DE_WITH_NAV
    DETERMINISTIC
)

# de_nav_build: the build code shared by the runtime and the cooker's `navmesh` importer
# (tools/cook/importers/navmesh/), so it is built even when DE_WITH_NAV is off. The C++ is only in
# nav_recast_bridge.cpp, behind the extern "C" nav_recast_bridge.h (DENG-2 Q1). As with
# de_audio_dsp, aggregate module-object consumers also need this non-module library.
add_library(de_nav_build STATIC
    nav_build.h nav_build.c nav_cook.c nav_settings.c nav_tiles.c
    nav_recast_bridge.h nav_recast_bridge.cpp)
target_include_directories(de_nav_build PUBLIC "${DE_ROOT_DIR}/src")
target_link_libraries(de_nav_build PUBLIC de_core PRIVATE de_tp_recastnavigation de_tp_tomlc17
    de_build_flags)
target_compile_options(de_nav_build PRIVATE ${DE_DETERMINISTIC_FLAGS})
set_target_properties(de_nav_build PROPERTIES LINKER_LANGUAGE CXX FOLDER "dengine/gameplay")
if(TARGET de_nav)
    target_link_libraries(de_nav PRIVATE de_nav_build)
    set_property(TARGET de_nav APPEND PROPERTY DE_MODULE_LIBS
        de_nav_build de_tp_recastnavigation de_tp_tomlc17)
    # Debug and Dev builds with the tools (DE_HAS_COOKER): the runtime type takes the cooker's
    # built-in navmesh importer, as src/gameplay/tilemap/ does.
    target_link_libraries(de_nav PRIVATE
        "$<$<AND:$<CONFIG:Debug,Dev>,$<TARGET_EXISTS:de_cooker>>:de_cooker>")
endif()
