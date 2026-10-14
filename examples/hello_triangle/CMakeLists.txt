# examples/hello_triangle (DENG-327): first pixels through the public de_gpu API on every backend.
# The root CMakeLists.txt adds this directory when DE_BUILD_EXAMPLES is ON. See README.md.
#
#   hello_triangle        the example: main.c (public headers only) + hello_triangle_glue
#   hello_triangle_glue   shader_glue.c, the one TU that uses engine internals: it registers the
#                         `shader` asset type and hands out cooked shader code (both internal until
#                         the engine has a public shader API; DENG-409), and holds the D3D12 Agility
#                         exports. tools/check_layers/layers.toml lists it ([[example_internal]]).
#
# It links the module libraries, as the tests do (not the aggregate `dengine`): the glue calls
# internal functions, which a shared `dengine` does not export. Modules keep src/ as a PRIVATE
# include directory, and the glue is an OBJECT library with PRIVATE dependencies, so main.c only
# sees include/dengine/ (and third_party/stb/).

if(NOT TARGET de_gpu_shader)
    # DE_HEADLESS, or no GPU module in this configuration: nothing to draw with.
    return()
endif()

# The Slang importer (tools/cook/importers/shader/, needs DE_BUILD_TOOLS and Slang for the host).
# tools/ is added before examples/. SOURCE mode cooks only in Debug and Dev (DE_HAS_COOKER).
set(_ht_importer FALSE)
if(TARGET de_cook_shader)
    set(_ht_importer TRUE)
endif()

add_library(hello_triangle_glue OBJECT shader_glue.c shader_glue.h)
target_include_directories(hello_triangle_glue PRIVATE "${DE_ROOT_DIR}/src")
target_link_libraries(hello_triangle_glue PRIVATE de_gpu_shader de_gpu de_asset de_build_flags)
if(_ht_importer)
    target_link_libraries(hello_triangle_glue PRIVATE "$<$<CONFIG:Debug,Dev>:de_cook_shader>")
    target_compile_definitions(hello_triangle_glue PRIVATE HELLO_HAS_SHADER_IMPORTER=1)
endif()
if(TARGET de_gpu_d3d12)
    target_compile_definitions(hello_triangle_glue PRIVATE HELLO_D3D12_AGILITY=1)
endif()
set_target_properties(hello_triangle_glue PROPERTIES FOLDER "dengine/examples")

# One executable per call: `hello_triangle`, and the test copy below.
function(_ht_add_executable name)
    add_executable(${name} main.c)
    target_link_libraries(${name} PRIVATE hello_triangle_glue de_gpu de_gpu_shader de_asset
                                          de_task de_platform de_core de_tp_stb de_build_flags)
    target_compile_definitions(${name} PRIVATE
        "HELLO_ASSET_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
    set_target_properties(${name} PROPERTIES FOLDER "dengine/examples")
    if(COMMAND de_d3d12_agility_deploy)
        de_d3d12_agility_deploy(${name}) # D3D12Core.dll in <exe dir>/D3D12/ (DENG-285)
    endif()
endfunction()

_ht_add_executable(hello_triangle)

# ---- CTests (Test plan): each scene on each backend this build has, offscreen, 3 frames, a PNG
# screenshot and 4 pixel checks (--check). Exit 77 = skipped: no device for the backend on this
# machine, shaders not cooked for its target (DXIL without DXC), or a build that cannot cook
# (Release). Logged errors fail a run, so with the API validation of Debug and Dev builds
# (Vulkan layer, D3D12 debug layer, and the Metal debug layer through the environment below) a
# validation error fails the test.
if(NOT DE_BUILD_TESTS OR NOT _ht_importer)
    return()
endif()

# The D3D12 tests run a copy of the example that has the vendored WARP next to it (DENG-405;
# Windows CI has no GPU). WARP is test-only (third_party/README.md, rule 10): only this test copy,
# in its own directory, gets the DLL; `hello_triangle` itself never does.
set(_ht_d3d12_exe hello_triangle)
if(TARGET de_gpu_d3d12 AND DE_D3D12_WARP_DLL)
    _ht_add_executable(hello_triangle_warp)
    set_target_properties(hello_triangle_warp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_BINARY_DIR}/warp>")
    de_d3d12_warp_deploy(hello_triangle_warp)
    set(_ht_d3d12_exe hello_triangle_warp)
endif()

foreach(_ht_backend IN ITEMS metal vulkan d3d12)
    if(NOT TARGET de_gpu_${_ht_backend})
        continue()
    endif()
    set(_ht_exe hello_triangle)
    if(_ht_backend STREQUAL "d3d12")
        set(_ht_exe ${_ht_d3d12_exe})
    endif()
    foreach(_ht_scene IN ITEMS triangle quad)
        set(_ht_name example_hello_${_ht_scene}_${_ht_backend})
        set(_ht_out "${CMAKE_CURRENT_BINARY_DIR}/${_ht_name}")
        file(MAKE_DIRECTORY "${_ht_out}")
        add_test(NAME ${_ht_name}
            COMMAND ${_ht_exe} --backend ${_ht_backend} --scene ${_ht_scene} --offscreen
                    --frames 3 --screenshot "${_ht_out}/screenshot.png" --check
                    --cache-dir "${_ht_out}")
        set_tests_properties(${_ht_name} PROPERTIES
            LABELS "example;gpu"
            TIMEOUT 300 # the first run cooks the shaders (Slang)
            SKIP_RETURN_CODE 77
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
    endforeach()
endforeach()
