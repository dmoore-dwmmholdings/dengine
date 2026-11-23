# third_party/recastnavigation: Recast and Detour (Zlib), pinned in third_party/vendor.toml.
# Navmesh generation and the navmesh runtime for the nav module (src/gameplay/nav/, DENG-76).
# Recast voxelizes input triangles into tiles of polygons; Detour holds the tiles and answers
# queries. DetourTileCache and DetourCrowd are compiled too, for obstacles and crowds (DENG-78).
#
# The library is C++ (DENG-2 Q1). Only src/gameplay/nav/*.cpp includes its headers; the engine
# reaches it through the extern "C" bridge src/gameplay/nav/nav_recast_bridge.h. C code that
# links de_tp_recastnavigation is linked with the C++ linker (LINKER_LANGUAGE below).
#
# DT_POLYREF64: 64-bit polygon references (16 salt, 28 tile, 20 polygon bits), so a navmesh can
# hold large tile counts. It is PUBLIC: every TU that includes DetourNavMesh.h must agree.
#
# Contraction of a*b+c into FMA is off (DE_DETERMINISTIC_FLAGS), so a tile build makes the same
# float decisions on hosts whose compilers contract by default. Recast still calls libm (cosf,
# sqrtf, floorf), so tiles are byte-identical on one platform, best effort across platforms.
file(GLOB _de_recast_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Recast/Source/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Detour/Source/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/DetourTileCache/Source/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/DetourCrowd/Source/*.cpp")
list(SORT _de_recast_sources)
add_library(de_tp_recastnavigation STATIC ${_de_recast_sources})
target_include_directories(de_tp_recastnavigation SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Recast/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Detour/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/DetourTileCache/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/DetourCrowd/Include")
target_compile_definitions(de_tp_recastnavigation PUBLIC DT_POLYREF64=1)
# Vendored code: no engine warning flags, and its own warnings are off (CONVENTIONS §6).
target_compile_options(de_tp_recastnavigation PRIVATE
    $<IF:$<STREQUAL:${DE_COMPILER_FRONTEND},MSVC>,/w,-w>
    ${DE_DETERMINISTIC_FLAGS})
set_target_properties(de_tp_recastnavigation PROPERTIES
    POSITION_INDEPENDENT_CODE ${DE_BUILD_SHARED}
    LINKER_LANGUAGE CXX
    FOLDER "dengine/third_party")

# Smoke test: a C11 test (engine strict flags) links a small C++ helper that builds one tile of a
# flat quad with Recast and loads it into a Detour navmesh. CTest entries:
# de_tp_smoke_recastnavigation.<test>.
de_add_test(NAME de_tp_smoke_recastnavigation
    SOURCES smoke/smoke_recastnavigation.c smoke/de_smoke_recastnavigation.cpp
    LIBS de_tp_recastnavigation
    LABELS unit third_party)
