# tests/unit/render: unit tests of module `render` (de_render, SPEC §12). Later S12 tasks append
# their test sources to the SOURCES list (one per line) or add executables below.
#
# DENG-171: the render graph builder. CTest names: render.rg_builder.<test>. The tests include
# the internal header "render/graph/rg_internal.h" to check versions, edges and culling, and use
# the null GPU backend for imported resources. The render and gpu modules do not exist in
# DE_HEADLESS builds (cmake/DeOptions.cmake), so neither do these.
if(NOT DE_HAS_RENDER OR NOT DE_HAS_GPU)
    return()
endif()

de_add_test(NAME render
    SOURCES
        test_rg_builder.c
        test_rg_compile.c
    LIBS de_render de_gpu de_test_support)
if(TARGET test_render)
    target_include_directories(test_render PRIVATE "${DE_ROOT_DIR}/src")
    # r.rg_dump test output (a writable mount).
    target_compile_definitions(test_render PRIVATE DE_RG_TEST_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
endif()

# DENG-172: the render graph compiler (test_rg_compile.c in test_render). CTest names:
# render.rg_compile.<test>, render.rg_alias.<test>, render.rg_barriers.<test> and
# render.rg_cache.<test>. The barrier goldens read the null backend's command log
# (src/gpu/null/, "gpu/gpu_backend.h").

# DENG-105: the runtime texture, mesh, material and model types and their upload path
# (src/render/loaders/), executable test_render_loaders; CTest names
# render_loaders.upload_ring.<test> (test_upload_ring.c: the ring, the queue and the per-frame copy
# list on the null backend) and render_loaders.loaders.<test> (test_render_loaders.c: the types
# through the asset system; readback on every GPU backend compiled in, skipped without a device;
# cook-to-load tests with the cooker, DE_HAS_COOKER). loader_test_util.h builds the cooked blobs.
# Scratch files go below <build>/tests/unit/render/test_render_loaders_tmp/ (mounted as rl:/); the
# glTF and EXR fixtures are read from tests/unit/import/data/. CI runs the Vulkan tests on lavapipe
# in the golden job and the D3D12 ones on the vendored WARP.
set(_de_rl_libs de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support)
if(TARGET de_gpu_vulkan)
    list(APPEND _de_rl_libs de_gpu_vulkan de_tp_vulkan_headers)
endif()
if(TARGET de_gpu_metal)
    list(APPEND _de_rl_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
if(DE_HAS_GPU_D3D12)
    list(APPEND _de_rl_libs de_gpu_d3d12 de_tp_d3d12_agility)
endif()
de_add_test(NAME render_loaders
    SOURCES test_upload_ring.c test_render_loaders.c
    LIBS ${_de_rl_libs}
    LABELS unit gpu
    TIMEOUT 300)
if(TARGET test_render_loaders)
    target_include_directories(test_render_loaders PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_render_loaders_tmp")
    target_compile_definitions(test_render_loaders PRIVATE
        "DE_RL_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_render_loaders_tmp\""
        "DE_RL_TEST_IMPORT_DATA_DIR=\"${DE_ROOT_DIR}/tests/unit/import/data\"")
    # The cook-to-load tests call the cooker library (Debug and Dev, as de_asset links it).
    target_link_libraries(test_render_loaders PRIVATE
        "$<$<AND:$<CONFIG:Debug,Dev>,$<TARGET_EXISTS:de_cooker>>:de_cooker>")
    if(DE_HAS_GPU_D3D12)
        de_d3d12_agility_deploy(test_render_loaders)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_loaders)
        endif()
    endif()
endif()

# DENG-174: the renderer, its settings, tiers, quality presets and cvars, executable
# test_renderer; CTest names renderer.<test>. Pure settings tests on synthetic caps; the renderer
# on the null backend and on Metal (macOS; skipped without a device or below the Base tier); 100
# frames under the alloc guard. Scratch files (the asset cook cache) go below
# <build>/tests/unit/render/test_renderer_tmp/ (mounted as rr:/).
set(_de_rr_libs de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support)
if(TARGET de_gpu_metal)
    list(APPEND _de_rr_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
de_add_test(NAME renderer
    SOURCES test_renderer_settings.c
    LIBS ${_de_rr_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_renderer)
    target_include_directories(test_renderer PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_renderer_tmp")
    target_compile_definitions(test_renderer PRIVATE
        "DE_RR_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_renderer_tmp\"")
    target_link_libraries(test_renderer PRIVATE
        "$<$<AND:$<CONFIG:Debug,Dev>,$<TARGET_EXISTS:de_cooker>>:de_cooker>")
endif()

# DENG-178: the render components (src/render/components.c), executable test_render_components;
# CTest names render_components.<test>: registration (also through de_renderer_attach on the null
# backend), the TOML round trip, the construct defaults, the field flags, and, with the cooker
# (DE_HAS_COOKER), a glTF model's prefab instantiated with DeMeshRenderer. Scratch files go below
# <build>/tests/unit/render/test_render_components_tmp/ (mounted as rc:/); hierarchy.gltf is read
# from tests/unit/import/data/ (mounted as rcimp:/).
de_add_test(NAME render_components
    SOURCES test_render_components.c
    LIBS de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support
    LABELS unit render
    TIMEOUT 120)
if(TARGET test_render_components)
    target_include_directories(test_render_components PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_render_components_tmp")
    target_compile_definitions(test_render_components PRIVATE
        "DE_RC_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_render_components_tmp\""
        "DE_RC_TEST_IMPORT_DATA_DIR=\"${DE_ROOT_DIR}/tests/unit/import/data\"")
endif()

# DENG-172: the render graph compiler on real GPU backends (test_rg_gpu.c), executable
# test_render_graph_gpu, CTest names render_graph_gpu.rg_gpu.smoke_<backend>: clears, copies, a
# copy-queue ownership transfer and an aliased transient, read back. Each backend SKIPs when it is
# not built or has no device (CI: Metal on the macOS runner, D3D12 on the vendored WARP).
set(_de_rgg_libs de_render de_gpu de_test_support)
if(TARGET de_gpu_vulkan)
    list(APPEND _de_rgg_libs de_gpu_vulkan de_tp_vulkan_headers)
endif()
if(TARGET de_gpu_metal)
    list(APPEND _de_rgg_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
if(DE_HAS_GPU_D3D12)
    list(APPEND _de_rgg_libs de_gpu_d3d12 de_tp_d3d12_agility)
endif()
de_add_test(NAME render_graph_gpu
    SOURCES test_rg_gpu.c
    LIBS ${_de_rgg_libs}
    LABELS unit gpu
    TIMEOUT 120)
if(TARGET test_render_graph_gpu)
    target_include_directories(test_render_graph_gpu PRIVATE "${DE_ROOT_DIR}/src")
    if(DE_HAS_GPU_D3D12)
        de_d3d12_agility_deploy(test_render_graph_gpu)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_graph_gpu)
        endif()
    endif()
endif()

# DENG-175: the render world, the Extract systems and the pipelined render thread
# (src/render/world/), executable test_render_world; CTest names render_world.extract.<test>
# (test_extract.c: f32 transforms relative to the render origin, rebases, change lists, removals,
# no heap allocation once warm) and render_world.render_thread.<test> (test_render_thread.c: the
# simulation runs exactly one frame ahead of a slow render thread; r.pipelined 0 and 1 give the
# same render world). test_render_world.h is their shared fixture (defined in test_extract.c), on
# the null backend. The pipelined test is an S12 exit check under TSan (ci-linux-tsan).
de_add_test(NAME render_world
    SOURCES test_extract.c test_render_thread.c
    LIBS de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support
    LABELS unit render
    TIMEOUT 300)
if(TARGET test_render_world)
    target_include_directories(test_render_world PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-173: render graph record + submit (test_rg_execute.c), executable test_render_graph_execute,
# CTest names render_graph_execute.rg_execute.<test>: the fixed partition and parallel recording
# on the task system, the per-pass markers, timestamps and breadcrumbs (null backend command log),
# culled passes, the published GPU timings and their latency, no heap allocation after warm-up
# (null backend, every host), and metal_three_pass_readback (clear -> compute -> copy-queue
# readback on Metal with API validation, DENG-323's compute shader cooked when the build has the
# importer; SKIPs without a Metal device, below the Base tier or without the importer).
set(_de_rgx_libs de_render de_gpu de_task de_platform de_test_support)
if(TARGET de_gpu_metal)
    list(APPEND _de_rgx_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
de_add_test(NAME render_graph_execute
    SOURCES test_rg_execute.c "${DE_ROOT_DIR}/tests/unit/gpu/data/pipelines/pipeline_shaders.c"
    LIBS ${_de_rgx_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_graph_execute)
    target_include_directories(test_render_graph_execute PRIVATE "${DE_ROOT_DIR}/src"
        "${DE_ROOT_DIR}/tests/unit/gpu/data/pipelines")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_render_graph_execute PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_render_graph_execute PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_render_graph_execute PRIVATE DE_PIPE_TEST_COOK=1
            "DE_PIPE_TEST_DATA_DIR=\"${DE_ROOT_DIR}/tests/unit/gpu/data/pipelines\"")
    endif()
endif()

# DENG-176: views and the per-view frame pipeline (src/render/views.c, passes/composite.c),
# executable test_views; CTest names views.<test>: the main view and camera binding, split screen
# (two graph instances), render to texture consumed in the same frame, independent render scales,
# the compiled-graph cache in a steady frame (no heap allocation), deferred release, hooks and
# registered stages in slot then registration order, per-view settings (null backend); and on
# Metal (macOS, API validation on; SKIPs without a device or below the Base tier) a split-screen
# + render-to-texture readback and views destroyed while frames are in flight.
set(_de_rv_libs de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support)
if(TARGET de_gpu_metal)
    list(APPEND _de_rv_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
de_add_test(NAME views
    SOURCES test_views.c
    LIBS ${_de_rv_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_views)
    target_include_directories(test_views PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-177: the GPU scene (src/render/scene/), executable test_gpu_scene; CTest names
# render_gpu_scene.gpu_scene.<test>. Synthetic render-world buffers and fake mesh/material
# records drive it. On the null backend every frame's upload plan is applied to CPU copies of the
# tables; random_ops_readback_{metal,vulkan,d3d12} read the tables back from the GPU (each SKIPs
# without its device; CI: Vulkan on lavapipe in the golden job, D3D12 on the vendored WARP) with
# shaders/scene/scatter.slang cooked at test time when the build has the shader importer
# (DE_GS_TEST_COOK; copy-only otherwise). layout_match parses shaders/scene/scene_types.slang.
set(_de_gs_libs de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support)
if(TARGET de_gpu_vulkan)
    list(APPEND _de_gs_libs de_gpu_vulkan de_tp_vulkan_headers)
endif()
if(TARGET de_gpu_metal)
    list(APPEND _de_gs_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
if(DE_HAS_GPU_D3D12)
    list(APPEND _de_gs_libs de_gpu_d3d12 de_tp_d3d12_agility)
endif()
de_add_test(NAME render_gpu_scene
    SOURCES test_gpu_scene.c
    LIBS ${_de_gs_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_gpu_scene)
    target_include_directories(test_render_gpu_scene PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_gpu_scene PRIVATE
        "DE_GS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_render_gpu_scene PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_render_gpu_scene PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_render_gpu_scene PRIVATE DE_GS_TEST_COOK=1)
    endif()
    if(DE_HAS_GPU_D3D12)
        # DENG-420: test_gpu_scene.c includes the Agility exports, so the Agility runtime (not the
        # OS one) loads and the vendored WARP is Base tier; version.lib reads the DLL versions it
        # logs.
        target_link_libraries(test_render_gpu_scene PRIVATE version)
        de_d3d12_agility_deploy(test_render_gpu_scene)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_gpu_scene)
        endif()
    endif()
endif()
