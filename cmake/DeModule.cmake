# DeModule.cmake: engine module helpers. de_add_module() is the only way to define an engine
# target.
#
# de_add_module(NAME <name> LAYER <band>
#               [SOURCES <file>...]
#               [DEPS <module>...]
#               [OPTIONAL_DEPS <module>...]
#               [LIBS <target-or-lib>...]
#               [DETERMINISTIC]
#               [OPTIONAL_OPTION <variable>])
#
#   NAME           Module name. Creates the static library `de_<name>`. Nested module dirs use
#                  their leaf name (src/world/vfx -> vfx -> de_vfx) except the GPU backends
#                  (src/gpu/vulkan -> gpu_vulkan -> de_gpu_vulkan).
#   LAYER          SPEC §4.1 band: foundation, runtime, content, gameplay or host. Stored in the
#                  target property LAYER (read by the layering check, DENG-65).
#   SOURCES        Source files, relative to the calling CMakeLists.txt (.c, .cpp, .m).
#   DEPS           Engine modules this one needs (names, not targets): linked PUBLIC as de_<dep>.
#                  It is a configure error if a DEP is not built (for example removed by
#                  DE_HEADLESS or DE_WITH_<X>=OFF); use OPTIONAL_DEPS for those.
#   OPTIONAL_DEPS  Engine modules linked PUBLIC only when they are built. Guard the code that
#                  uses them with `#if DE_HAS_<X>`.
#   LIBS           Other link items (de_tp_* vendored targets, system libraries, frameworks),
#                  linked PRIVATE. They also reach consumers of the aggregate `dengine` library.
#   DETERMINISTIC  SPEC §5.7: every TU of the module gets -ffp-contract=off (Clang/GCC) or
#                  /fp:precise (MSVC). Sets the target property DE_DETERMINISTIC.
#   OPTIONAL_OPTION  Name of a variable (usually DE_WITH_<X> or DE_HAS_<X>). The module is skipped
#                  when it is false. Independently of this, a module is always skipped when
#                  DE_HAS_<NAME> (cmake/DeOptions.cmake) is defined and false.
#
# Include directories: PUBLIC <root>/include (so `#include <dengine/de_x.h>`) and PRIVATE
# <root>/src (so internal cross-module headers are `#include "core/xxx.h"`).
#
# Other helpers:
#   de_mark_deterministic_sources(<file>...)  per-file deterministic flags, for single files in
#                                             a non-deterministic module (src/core/math/dmath*.c).
#   de_add_subdirectories([DIR <dir>] [EXCLUDE <name>...])  add_subdirectory() for every
#                                             immediate child dir of DIR (default: the current
#                                             source dir) that has a CMakeLists.txt, sorted.
#   de_finalize_modules()                     called once by src/CMakeLists.txt: checks DEPS and
#                                             creates the aggregate `dengine` library.
#   de_write_config_dump()                    called once by the root: writes
#                                             <build>/de_config_dump.txt and adds the target
#                                             de_print_config.
include_guard(GLOBAL)

set(DE_LAYERS foundation runtime content gameplay host)

function(de_add_module)
    cmake_parse_arguments(PARSE_ARGV 0 M
        "DETERMINISTIC"
        "NAME;LAYER;OPTIONAL_OPTION"
        "SOURCES;DEPS;OPTIONAL_DEPS;LIBS")

    if(M_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "de_add_module: unknown arguments: ${M_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT M_NAME OR NOT M_NAME MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "de_add_module: NAME must be lower_snake_case (got '${M_NAME}')")
    endif()
    if(NOT M_LAYER IN_LIST DE_LAYERS)
        message(FATAL_ERROR
            "de_add_module(${M_NAME}): LAYER must be one of ${DE_LAYERS} (got '${M_LAYER}')")
    endif()
    if(NOT M_SOURCES)
        message(FATAL_ERROR "de_add_module(${M_NAME}): SOURCES is empty")
    endif()

    set(target de_${M_NAME})
    file(RELATIVE_PATH dir "${DE_ROOT_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")

    # ---- Enabled? ----
    string(TOUPPER "${M_NAME}" upper)
    set(reason "")
    if(DEFINED DE_HAS_${upper} AND NOT DE_HAS_${upper})
        set(reason "DE_HAS_${upper} is off")
    elseif(M_OPTIONAL_OPTION AND NOT ${M_OPTIONAL_OPTION})
        set(reason "${M_OPTIONAL_OPTION} is off")
    endif()
    if(reason)
        message(STATUS "dengine: module ${M_NAME} (src/${dir}) skipped: ${reason}")
        set_property(GLOBAL APPEND PROPERTY DE_DISABLED_MODULES ${M_NAME})
        return()
    endif()

    if(TARGET ${target})
        message(FATAL_ERROR "de_add_module: module '${M_NAME}' is defined twice")
    endif()

    add_library(${target} STATIC ${M_SOURCES})
    target_include_directories(${target}
        PUBLIC "$<BUILD_INTERFACE:${DE_ROOT_DIR}/include>"
        PRIVATE "${DE_ROOT_DIR}/src")
    target_link_libraries(${target} PUBLIC de_config PRIVATE de_build_flags)

    foreach(dep IN LISTS M_DEPS)
        target_link_libraries(${target} PUBLIC de_${dep})
    endforeach()
    foreach(dep IN LISTS M_OPTIONAL_DEPS)
        # Resolved at generate time, so the order in which modules are defined does not matter.
        target_link_libraries(${target} PUBLIC "$<TARGET_NAME_IF_EXISTS:de_${dep}>")
    endforeach()
    if(M_LIBS)
        target_link_libraries(${target} PRIVATE ${M_LIBS})
    endif()

    if(DE_BUILD_SHARED)
        # The module's objects go into the shared `dengine` library: export DE_API symbols only.
        target_compile_definitions(${target} PRIVATE DE_BUILD_SHARED)
        set_target_properties(${target} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            C_VISIBILITY_PRESET hidden
            CXX_VISIBILITY_PRESET hidden
            OBJC_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
    endif()

    if(M_DETERMINISTIC)
        target_compile_options(${target} PRIVATE ${DE_DETERMINISTIC_FLAGS})
        set(det ON)
    else()
        set(det OFF)
    endif()

    set_target_properties(${target} PROPERTIES
        LAYER "${M_LAYER}"
        DE_MODULE "${M_NAME}"
        DE_MODULE_DIR "src/${dir}"
        DE_MODULE_DEPS "${M_DEPS}"
        DE_MODULE_LIBS "${M_LIBS}"
        DE_DETERMINISTIC ${det}
        FOLDER "dengine/modules")

    set_property(GLOBAL APPEND PROPERTY DE_MODULES ${M_NAME})
endfunction()

function(de_mark_deterministic_sources)
    if(ARGN)
        set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS ${DE_DETERMINISTIC_FLAGS})
    endif()
endfunction()

function(de_add_subdirectories)
    cmake_parse_arguments(PARSE_ARGV 0 A "" "DIR" "EXCLUDE")
    if(NOT A_DIR)
        set(A_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    cmake_path(ABSOLUTE_PATH A_DIR BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
               NORMALIZE OUTPUT_VARIABLE A_DIR)
    # CONFIGURE_DEPENDS: a new <dir>/<child>/CMakeLists.txt re-runs configure on the next build.
    file(GLOB lists CONFIGURE_DEPENDS "${A_DIR}/*/CMakeLists.txt")
    list(SORT lists)
    foreach(list_file IN LISTS lists)
        cmake_path(GET list_file PARENT_PATH child)
        cmake_path(GET child FILENAME name)
        if(name IN_LIST A_EXCLUDE)
            continue()
        endif()
        file(RELATIVE_PATH rel "${CMAKE_CURRENT_SOURCE_DIR}" "${child}")
        add_subdirectory("${child}" "${CMAKE_CURRENT_BINARY_DIR}/${rel}")
    endforeach()
endfunction()

function(de__anchor_source out name)
    set(file "${CMAKE_CURRENT_BINARY_DIR}/${name}_anchor.c")
    string(JOIN "\n" content
        "/* Generated by cmake/DeModule.cmake: the ${name} library needs one source of its own. */"
        "int de__${name}_anchor(void);"
        "int de__${name}_anchor(void) { return 0; }"
        "")
    file(CONFIGURE OUTPUT "${file}" CONTENT "${content}")
    set(${out} "${file}" PARENT_SCOPE)
endfunction()

function(de_finalize_modules)
    get_property(modules GLOBAL PROPERTY DE_MODULES)
    get_property(disabled GLOBAL PROPERTY DE_DISABLED_MODULES)

    # Every hard DEP must be a module that is actually built.
    foreach(m IN LISTS modules)
        get_target_property(deps de_${m} DE_MODULE_DEPS)
        foreach(dep IN LISTS deps)
            if(dep IN_LIST disabled)
                message(FATAL_ERROR "dengine: module '${m}' DEPS on '${dep}', which is disabled "
                    "in this configuration. Move it to OPTIONAL_DEPS or gate '${m}' too.")
            elseif(NOT TARGET de_${dep})
                message(FATAL_ERROR "dengine: module '${m}' DEPS on unknown module '${dep}'")
            endif()
        endforeach()
    endforeach()

    # Aggregate library. It holds the objects of every module, so consumers link one library.
    de__anchor_source(anchor dengine)
    if(DE_BUILD_SHARED)
        add_library(dengine SHARED "${anchor}")
        target_compile_definitions(dengine PRIVATE DE_BUILD_SHARED)
        set_target_properties(dengine PROPERTIES
            C_VISIBILITY_PRESET hidden
            CXX_VISIBILITY_PRESET hidden
            OBJC_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR})
        set(libs_scope PRIVATE)
    else()
        add_library(dengine STATIC "${anchor}")
        # Also in de_config; stated here so the contract is visible on the consumer-facing target.
        target_compile_definitions(dengine PUBLIC DE_STATIC)
        set(libs_scope PUBLIC)
    endif()
    target_link_libraries(dengine PUBLIC de_config PRIVATE de_build_flags)
    target_include_directories(dengine PUBLIC "$<BUILD_INTERFACE:${DE_ROOT_DIR}/include>")
    foreach(m IN LISTS modules)
        target_sources(dengine PRIVATE "$<TARGET_OBJECTS:de_${m}>")
        add_dependencies(dengine de_${m})
        get_target_property(libs de_${m} DE_MODULE_LIBS)
        if(libs)
            target_link_libraries(dengine ${libs_scope} ${libs})
        endif()
    endforeach()
    add_library(dengine::dengine ALIAS dengine)

    list(LENGTH modules n)
    message(STATUS "dengine: ${n} modules: ${modules}")
    if(disabled)
        message(STATUS "dengine: disabled modules: ${disabled}")
    endif()
endfunction()

function(de_write_config_dump)
    get_property(modules GLOBAL PROPERTY DE_MODULES)
    get_property(disabled GLOBAL PROPERTY DE_DISABLED_MODULES)
    set(det "")
    set(layers "")
    foreach(m IN LISTS modules)
        get_target_property(d de_${m} DE_DETERMINISTIC)
        if(d)
            list(APPEND det ${m})
        endif()
        get_target_property(layer de_${m} LAYER)
        get_target_property(mdir de_${m} DE_MODULE_DIR)
        list(APPEND layers "${m}:${layer}:${mdir}")
    endforeach()
    if(DE_MULTI_CONFIG)
        set(out "${CMAKE_BINARY_DIR}/de_config_dump-$<CONFIG>.txt")
    else()
        set(out "${CMAKE_BINARY_DIR}/de_config_dump.txt")
    endif()
    set(defs "$<TARGET_PROPERTY:dengine,INTERFACE_COMPILE_DEFINITIONS>")
    string(JOIN "\n" content
        "build_type=$<CONFIG>"
        "compiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}"
        "system=${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}"
        "DE_BUILD_SHARED=${DE_BUILD_SHARED}"
        "DE_HEADLESS=${DE_HEADLESS}"
        "DE_BUILD_TESTS=${DE_BUILD_TESTS}"
        "DE_BUILD_TOOLS=${DE_BUILD_TOOLS}"
        "DE_BUILD_EXAMPLES=${DE_BUILD_EXAMPLES}"
        "DE_BUILD_DOCS=${DE_BUILD_DOCS}"
        "DE_SANITIZE=${DE_SANITIZE}"
        "defines=$<JOIN:$<REMOVE_DUPLICATES:${defs}>,$<SEMICOLON>>"
        "dengine_type=$<TARGET_PROPERTY:dengine,TYPE>"
        "modules=${modules}"
        "deterministic=${det}"
        "layers=${layers}"
        "disabled=${disabled}"
        "")
    file(GENERATE OUTPUT "${out}" CONTENT "${content}")
    add_custom_target(de_print_config
        COMMAND "${CMAKE_COMMAND}" -E cat "${out}"
        VERBATIM)
endfunction()
