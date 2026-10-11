# DeBuildTypes.cmake: the four DEngine build types (SPEC §3.2).
#
#   Debug     no optimization, all asserts            DE_DEBUG=1
#   Dev       optimized + debug info, asserts, tools  DE_DEV=1 DE_PROFILE=1
#   Release   optimized, cooked assets                DE_PROFILE=1 only if DE_ENABLE_PROFILE
#   Shipping  optimized, no dev tooling, static link  DE_SHIPPING=1 (never DE_PROFILE)
#
# Dev copies the RelWithDebInfo flags and Shipping copies the Release flags. The defines live on
# the INTERFACE target `de_config`, which every engine module links PUBLIC, so consumers see the
# same defines as the engine. Include this file after project().
include_guard(GLOBAL)

set(DE_BUILD_TYPES Debug Dev Release Shipping)

option(DE_ENABLE_PROFILE "Define DE_PROFILE (Tracy zones) in Release builds" ON)

get_property(DE_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(DE_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES ${DE_BUILD_TYPES} CACHE STRING "DEngine build types" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Dev CACHE STRING "Debug, Dev, Release or Shipping" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${DE_BUILD_TYPES})
    # Configs are case-insensitive in CMake; accept any spelling of the four names, nothing else.
    set(_de_known FALSE)
    foreach(_t IN LISTS DE_BUILD_TYPES)
        string(TOUPPER "${_t}" _tu)
        string(TOUPPER "${CMAKE_BUILD_TYPE}" _bu)
        if(_tu STREQUAL _bu)
            set(_de_known TRUE)
        endif()
    endforeach()
    if(NOT _de_known)
        message(FATAL_ERROR
            "dengine: CMAKE_BUILD_TYPE '${CMAKE_BUILD_TYPE}' is not one of: ${DE_BUILD_TYPES}")
    endif()
endif()

# True when the (single-config) build type is Shipping. Multi-config generators cannot know this
# at configure time; they only get the per-config defines below.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _de_bt_upper)
if(NOT DE_MULTI_CONFIG AND _de_bt_upper STREQUAL "SHIPPING")
    set(DE_IS_SHIPPING TRUE)
else()
    set(DE_IS_SHIPPING FALSE)
endif()

# Flags for the custom configs, copied from the built-in ones of every enabled language. CMake
# creates an empty CMAKE_<LANG>_FLAGS_<CONFIG> cache entry for a custom CMAKE_BUILD_TYPE while it
# enables the language, so the copy is forced, but only once per language: edits made later in
# the cache are kept.
macro(de__copy_config_flags var from)
    if(NOT DE__INIT_${var})
        set(${var} "${${from}}" CACHE STRING "Flags for this build type (from ${from})" FORCE)
        set(DE__INIT_${var} TRUE CACHE INTERNAL "")
    endif()
    mark_as_advanced(${var})
endmacro()
get_property(_de_langs GLOBAL PROPERTY ENABLED_LANGUAGES)
foreach(_lang IN LISTS _de_langs)
    if(_lang STREQUAL "NONE")
        continue()
    endif()
    de__copy_config_flags(CMAKE_${_lang}_FLAGS_DEV CMAKE_${_lang}_FLAGS_RELWITHDEBINFO)
    de__copy_config_flags(CMAKE_${_lang}_FLAGS_SHIPPING CMAKE_${_lang}_FLAGS_RELEASE)
endforeach()
foreach(_kind EXE SHARED MODULE STATIC)
    set(_lf CMAKE_${_kind}_LINKER_FLAGS)
    de__copy_config_flags(${_lf}_DEV ${_lf}_RELWITHDEBINFO)
    de__copy_config_flags(${_lf}_SHIPPING ${_lf}_RELEASE)
endforeach()

# MSVC/clang-cl: with policy CMP0141 the debug-info flag is not in the flag strings, so name the
# configs that carry debug info explicitly.
#
# ProgramDatabase (/Zi) makes every compile of a target write into one shared .pdb. A compiler
# cache such as sccache cannot cache such a compile with cl, so when a compiler launcher is set
# (CMAKE_<LANG>_COMPILER_LAUNCHER, as a variable or in the environment, which is how CI sets
# sccache) use Embedded (/Z7): the debug info goes into each .obj and the linker still writes the
# .pdb. A -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=... on the command line wins over both.
if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
    set(_de_msvc_debug_format ProgramDatabase)
    foreach(_lang C CXX)
        if(CMAKE_${_lang}_COMPILER_LAUNCHER
           OR NOT "$ENV{CMAKE_${_lang}_COMPILER_LAUNCHER}" STREQUAL "")
            set(_de_msvc_debug_format Embedded)
        endif()
    endforeach()
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<$<CONFIG:Debug,Dev,RelWithDebInfo>:${_de_msvc_debug_format}>")
endif()

# Imported third-party targets (Vulkan SDK, ...) usually ship only Debug/Release configs.
set(CMAKE_MAP_IMPORTED_CONFIG_DEV RelWithDebInfo Release "")
set(CMAKE_MAP_IMPORTED_CONFIG_SHIPPING Release RelWithDebInfo "")

# de_config: usage requirements shared by every engine target and its consumers.
if(NOT TARGET de_config)
    add_library(de_config INTERFACE)
    target_include_directories(de_config INTERFACE "$<BUILD_INTERFACE:${DE_ROOT_DIR}/include>")
    target_compile_definitions(de_config INTERFACE
        $<$<CONFIG:Debug>:DE_DEBUG=1>
        $<$<CONFIG:Dev>:DE_DEV=1>
        $<$<CONFIG:Dev>:DE_PROFILE=1>
        $<$<AND:$<CONFIG:Release>,$<BOOL:${DE_ENABLE_PROFILE}>>:DE_PROFILE=1>
        $<$<CONFIG:Shipping>:DE_SHIPPING=1>)
endif()
