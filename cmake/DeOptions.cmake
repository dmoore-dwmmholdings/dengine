# DeOptions.cmake: user-facing build options (SPEC §3.2, §4.2, §21.1).
#
# Each module that can be switched off has a DE_HAS_<MOD> variable (TRUE/FALSE) computed here
# from the options. de_add_module() skips a module whose DE_HAS_<NAME> is FALSE, and every
# enabled one is exported to C code as DE_HAS_<MOD>=1 through the `de_config` target.
# Include after DeBuildTypes.cmake.
include_guard(GLOBAL)

# ---- Build products ----
option(DE_BUILD_SHARED "Build the aggregate dengine library as a shared library" OFF)
option(DE_BUILD_TESTS "Build tests (tests/)" ON)
option(DE_BUILD_TOOLS "Build tools (tools/)" ON)
if(DE_IS_SHIPPING)
    set(_de_examples_default OFF)
else()
    set(_de_examples_default ON)
endif()
option(DE_BUILD_EXAMPLES "Build examples (examples/)" ${_de_examples_default})
option(DE_BUILD_DOCS "Build documentation (docs/CMakeLists.txt, if present)" OFF)

# Shipping is always a static link (SPEC §3.2). A plain variable shadows the cache entry, so the
# user's cached choice is kept for other build directories but ignored here.
if(DE_IS_SHIPPING AND DE_BUILD_SHARED)
    message(STATUS "dengine: Shipping build forces DE_BUILD_SHARED=OFF (static link)")
    set(DE_BUILD_SHARED OFF)
endif()

# ---- Server build ----
option(DE_HEADLESS "Headless server build: no renderer, GPU, audio or UI (SPEC §4.2)" OFF)

# ---- Removable modules (SPEC §4.2) ----
option(DE_WITH_AUDIO "Build the audio module" ON)
option(DE_WITH_NET "Build the networking module" ON)
option(DE_WITH_PHYSICS "Build the physics module" ON)
option(DE_WITH_UI "Build the in-game UI module" ON)
option(DE_WITH_VFX "Build the VFX module" ON)
option(DE_WITH_NAV "Build the navigation module" ON)
option(DE_WITH_VIDEO "Build the video module" ON)
# Off by default: the Steamworks SDK is never vendored; the user supplies its path (§21.1).
option(DE_WITH_STEAM "Build the Steam services module (needs the Steamworks SDK)" OFF)

# ---- GPU backends, defaulted per platform (SPEC §3.1) ----
set(_de_d3d12_default OFF)
set(_de_vulkan_default OFF)
set(_de_metal_default OFF)
if(WIN32)
    set(_de_d3d12_default ON)
elseif(APPLE)
    set(_de_metal_default ON)
    # Vulkan on macOS runs through MoltenVK, for tests only. Default it ON when the SDK exists.
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(_de_vulkan_default ON)
    endif()
elseif(UNIX)
    set(_de_vulkan_default ON)
endif()
option(DE_GPU_D3D12 "Build the D3D12 RHI backend" ${_de_d3d12_default})
option(DE_GPU_VULKAN "Build the Vulkan RHI backend" ${_de_vulkan_default})
option(DE_GPU_METAL "Build the Metal RHI backend" ${_de_metal_default})

# ---- Effective module switches ----
macro(de__has var)
    if(${ARGN})
        set(${var} TRUE)
    else()
        set(${var} FALSE)
    endif()
endmacro()

de__has(DE_HAS_GPU NOT DE_HEADLESS)
de__has(DE_HAS_RENDER NOT DE_HEADLESS)
de__has(DE_HAS_GPU_D3D12 DE_HAS_GPU AND DE_GPU_D3D12)
de__has(DE_HAS_GPU_VULKAN DE_HAS_GPU AND DE_GPU_VULKAN)
de__has(DE_HAS_GPU_METAL DE_HAS_GPU AND DE_GPU_METAL)
de__has(DE_HAS_AUDIO DE_WITH_AUDIO AND NOT DE_HEADLESS)
de__has(DE_HAS_UI DE_WITH_UI AND NOT DE_HEADLESS)
de__has(DE_HAS_NET DE_WITH_NET)
de__has(DE_HAS_PHYSICS DE_WITH_PHYSICS)
de__has(DE_HAS_VFX DE_WITH_VFX)
de__has(DE_HAS_NAV DE_WITH_NAV)
de__has(DE_HAS_VIDEO DE_WITH_VIDEO)
de__has(DE_HAS_STEAM DE_WITH_STEAM)

set(DE_OPTIONAL_MODULES
    GPU RENDER GPU_D3D12 GPU_VULKAN GPU_METAL AUDIO UI NET PHYSICS VFX NAV VIDEO STEAM)

foreach(_m IN LISTS DE_OPTIONAL_MODULES)
    if(DE_HAS_${_m})
        target_compile_definitions(de_config INTERFACE DE_HAS_${_m}=1)
    endif()
endforeach()
if(DE_HEADLESS)
    target_compile_definitions(de_config INTERFACE DE_HEADLESS=1)
endif()

# Static build: DE_STATIC turns DE_API into nothing for the engine and its consumers. Without it a
# Windows consumer of the static library would see __declspec(dllimport) (de_base.h).
if(NOT DE_BUILD_SHARED)
    target_compile_definitions(de_config INTERFACE DE_STATIC)
endif()
