# tests/unit/ecs: unit tests of module `ecs` (de_ecs, SPEC §7). Shared file (append-only within
# S05): later S05 tasks add their own de_add_test() calls below, one block per task.
#
# One executable per suite, so the CTest names are world.<test>, entity.<test> and
# archetype.<test>. The tests include the internal header "ecs/ecs_internal.h" (src/ is an
# include root) and the shared fixture ecs_test_util.h. Test sources use the module's
# deterministic flags too (no FP contraction), like the code they check.

# DENG-344: storage (world, entities, components, archetypes, tables).
de_add_test(NAME world SOURCES test_world.c LIBS de_ecs)
de_add_test(NAME entity SOURCES test_entity.c LIBS de_ecs TIMEOUT 120)
de_add_test(NAME archetype SOURCES test_archetype.c LIBS de_ecs TIMEOUT 120)
foreach(_de_ecs_test test_world test_entity test_archetype)
    if(TARGET ${_de_ecs_test})
        target_include_directories(${_de_ecs_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_ecs_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()

# DENG-344: SPEC §5.7 rule 5 — no hash map is ever iterated in src/ecs/ (lookups only). A grep
# over the module's sources for the iteration API of core/de_hashmap.h.
if(DE_BUILD_TESTS)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ecs_no_hashmap_iter.cmake" [=[
file(GLOB_RECURSE _de_srcs "${DE_ECS_DIR}/*.c" "${DE_ECS_DIR}/*.h")
if(NOT _de_srcs)
    message(FATAL_ERROR "no sources found under ${DE_ECS_DIR}")
endif()
set(_de_bad "")
foreach(_de_src IN LISTS _de_srcs)
    file(STRINGS "${_de_src}" _de_hits REGEX "de_hashmap_iter_(begin|next)|de_hashmap_iter[ \t]")
    foreach(_de_hit IN LISTS _de_hits)
        string(APPEND _de_bad "  ${_de_src}: ${_de_hit}\n")
    endforeach()
endforeach()
if(_de_bad)
    message(FATAL_ERROR "src/ecs iterates a hash map (SPEC 5.7 rule 5):\n${_de_bad}")
endif()
list(LENGTH _de_srcs _de_n)
message(STATUS "src/ecs: no hash map iteration in ${_de_n} files")
]=])
    add_test(NAME ecs.no_hashmap_iteration
        COMMAND "${CMAKE_COMMAND}" "-DDE_ECS_DIR=${DE_ROOT_DIR}/src/ecs"
                -P "${CMAKE_CURRENT_BINARY_DIR}/ecs_no_hashmap_iter.cmake")
    set_tests_properties(ecs.no_hashmap_iteration PROPERTIES LABELS "unit;ecs" TIMEOUT 60)
endif()

# DENG-348: resources and events.
de_add_test(NAME resource SOURCES test_resource.c LIBS de_ecs)
de_add_test(NAME event SOURCES test_event.c LIBS de_ecs)
foreach(_de_ecs_test test_resource test_event)
    if(TARGET ${_de_ecs_test})
        target_include_directories(${_de_ecs_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_ecs_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()

# DENG-345: structural changes (add, remove, despawn, spawn_with) and the archetype edge cache.
de_add_test(NAME structural SOURCES test_structural.c LIBS de_ecs TIMEOUT 120)
if(TARGET test_structural)
    target_include_directories(test_structural PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_options(test_structural PRIVATE ${DE_DETERMINISTIC_FLAGS})
endif()

# DENG-349: command buffers (deferred structural changes, pending ids, deterministic apply).
de_add_test(NAME commands SOURCES test_commands.c LIBS de_ecs LABELS tsan TIMEOUT 300)
if(TARGET test_commands)
    target_include_directories(test_commands PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_options(test_commands PRIVATE ${DE_DETERMINISTIC_FLAGS})
endif()

# DENG-346: queries. qpar.* runs de_query_par_each on task systems of several sizes: label tsan.
de_add_test(NAME query SOURCES test_query.c LIBS de_ecs TIMEOUT 120)
de_add_test(NAME qpar SOURCES test_query_par.c LIBS de_ecs LABELS unit tsan TIMEOUT 300)
foreach(_de_ecs_test test_query test_qpar)
    if(TARGET ${_de_ecs_test})
        target_include_directories(${_de_ecs_test} PRIVATE "${DE_ROOT_DIR}/src")
        target_compile_options(${_de_ecs_test} PRIVATE ${DE_DETERMINISTIC_FLAGS})
    endif()
endforeach()

# DENG-347: change detection. change.write_iter_marks runs de_query_par_each on task systems: tsan.
de_add_test(NAME change SOURCES test_change.c LIBS de_ecs LABELS unit tsan TIMEOUT 120)
if(TARGET test_change)
    target_include_directories(test_change PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_options(test_change PRIVATE ${DE_DETERMINISTIC_FLAGS})
endif()
