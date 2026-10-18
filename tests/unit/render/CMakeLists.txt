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
    SOURCES test_upload_ring.c test_render_loaders.c render_d3d12_test.c
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
        # DENG-423: the deploy adds the Agility exports; version.lib for render_d3d12_test.h.
        target_link_libraries(test_render_loaders PRIVATE version)
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
    SOURCES test_rg_gpu.c render_d3d12_test.c
    LIBS ${_de_rgg_libs}
    LABELS unit gpu
    TIMEOUT 120)
if(TARGET test_render_graph_gpu)
    target_include_directories(test_render_graph_gpu PRIVATE "${DE_ROOT_DIR}/src")
    if(DE_HAS_GPU_D3D12)
        # DENG-423: the deploy adds the Agility exports; version.lib for render_d3d12_test.h.
        target_link_libraries(test_render_graph_gpu PRIVATE version)
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
    SOURCES test_gpu_scene.c render_d3d12_test.c
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
        # DENG-420, DENG-423: de_d3d12_agility_deploy gives the executable the Agility exports,
        # so the Agility runtime (not the OS one) loads and the vendored WARP is Base tier;
        # version.lib reads the DLL versions render_d3d12_test.h logs.
        target_link_libraries(test_render_gpu_scene PRIVATE version)
        de_d3d12_agility_deploy(test_render_gpu_scene)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_gpu_scene)
        endif()
    endif()
endif()

# DENG-421: the engine shaders (src/render/shaders/engine_shaders.{h,c}; shaders/engine_shaders.cmake),
# executable test_engine_shaders; CTest names engine_shaders.<test>: the generated table, errors,
# the null placeholder, the Dev cook cache (a cold cache cooks, the next load hits), the embedded
# blobs making pipelines with the in-process cook off, and a second renderer's composite coming
# from the cache (Dev) or the embedded blob (Release, Shipping). GPU tests use the first backend
# with a Base-tier device and SKIP without one. DE_ESH_TEST_EMBEDDED: this build embeds blobs.
set(_de_esh_libs de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support)
if(TARGET de_gpu_metal)
    list(APPEND _de_esh_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
if(TARGET de_gpu_vulkan)
    list(APPEND _de_esh_libs de_gpu_vulkan de_tp_vulkan_headers)
endif()
if(DE_HAS_GPU_D3D12)
    list(APPEND _de_esh_libs de_gpu_d3d12 de_tp_d3d12_agility)
endif()
de_add_test(NAME engine_shaders
    SOURCES test_engine_shaders.c
    LIBS ${_de_esh_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_engine_shaders)
    target_include_directories(test_engine_shaders PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_engine_shaders_tmp")
    if(DE_ENGINE_SHADERS_BUILD_COOK)
        set(_de_esh_embedded 1)
    else()
        set(_de_esh_embedded 0)
    endif()
    target_compile_definitions(test_engine_shaders PRIVATE
        "DE_ESH_TEST_EMBEDDED=${_de_esh_embedded}"
        "DE_ESH_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_engine_shaders_tmp\"")
    if(DE_HAS_GPU_D3D12)
        de_d3d12_agility_deploy(test_engine_shaders)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_engine_shaders)
        endif()
    endif()
endif()

# DENG-229: GPU instance culling and the visibility-buffer frame skeleton (src/render/passes/vis/),
# executable test_render_vis; CTest names render_vis.<suite>.<test>. A fake render world and fake
# mesh records drive a GPU scene; the vis frame is built into the same graph and a test pass reads
# back the work list, counters and dispatch arguments, compared field by field with the CPU
# reference (vis_stages.h). instance_cull.*_{metal,vulkan,d3d12} run on each GPU backend (SKIP
# without the device; the engine shader vis_instance_cull, DENG-421; CI: Vulkan on lavapipe and
# D3D12 on the vendored WARP in the golden jobs); vis_frame.* and the CPU
# tests run on the null backend. vis.layout_match parses shaders/visbuffer/vis_types.slang and
# shaders/culling/cull_common.slang.
set(_de_vis_libs de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support)
if(TARGET de_gpu_vulkan)
    list(APPEND _de_vis_libs de_gpu_vulkan de_tp_vulkan_headers)
endif()
if(TARGET de_gpu_metal)
    list(APPEND _de_vis_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
if(DE_HAS_GPU_D3D12)
    list(APPEND _de_vis_libs de_gpu_d3d12 de_tp_d3d12_agility)
endif()
de_add_test(NAME render_vis
    SOURCES test_instance_cull.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_vis)
    target_include_directories(test_render_vis PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_vis PRIVATE
        "DE_VIS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_HAS_GPU_D3D12)
        # DENG-423: the Agility exports and runtime (and the vendored WARP on CI), as
        # test_render_gpu_scene; version.lib reads the DLL versions render_d3d12_test.h logs.
        target_link_libraries(test_render_vis PRIVATE version)
        de_d3d12_agility_deploy(test_render_vis)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_vis)
        endif()
    endif()
endif()

# DENG-422: asset unloads against the render thread (src/render/loaders/ retire list, the GPU
# scene's prepare at the kick), executable test_render_churn; CTest names render_churn.churn.<test>:
# an unloaded mesh waits for the frame fence, and 1000 frames of load/unload/spawn churn with
# r.pipelined = 1 (null backend, the real asset system and ECS loop). The data-race and
# use-after-free part runs under TSan (ci-linux-tsan) and ASan (CI and ci-macos-asan).
de_add_test(NAME render_churn
    SOURCES test_render_churn.c
    LIBS de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support
    LABELS unit render
    TIMEOUT 300)
if(TARGET test_render_churn)
    target_include_directories(test_render_churn PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_render_churn_tmp")
    target_compile_definitions(test_render_churn PRIVATE
        "DE_RCH_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_render_churn_tmp\"")
    target_link_libraries(test_render_churn PRIVATE
        "$<$<AND:$<CONFIG:Debug,Dev>,$<TARGET_EXISTS:de_cooker>>:de_cooker>")
endif()

# DENG-231: GPU meshlet culling (src/render/passes/vis/cull_meshlets.c, the engine shader
# vis_meshlet_cull), executable test_render_meshlet_cull; CTest names render_meshlet_cull.<suite>.
# <test>. Meshes of real triangles get their meshlet bounds from meshoptimizer (as the cooker's),
# so the test links it. meshlet_cull.*_{metal,vulkan,d3d12} compare the visible list with the CPU
# reference and the triangles (SKIP without the device; CI: Vulkan on lavapipe and D3D12 on the
# vendored WARP in the golden jobs); the CPU tests parse shaders/culling/meshlet_cull.slang and
# shaders/visbuffer/vis_types.slang.
de_add_test(NAME render_meshlet_cull
    SOURCES test_meshlet_cull.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_tp_meshoptimizer
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_meshlet_cull)
    target_include_directories(test_render_meshlet_cull PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_meshlet_cull PRIVATE
        "DE_VIS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_meshlet_cull PRIVATE version)
        de_d3d12_agility_deploy(test_render_meshlet_cull)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_meshlet_cull)
        endif()
    endif()
endif()

# DENG-233: the HZB and two-phase occlusion culling (src/render/passes/vis/hzb.c, occlusion.c),
# executable test_render_occlusion; CTest names render_occlusion.<suite>.<test>. test_hzb.c: the
# HZB against a CPU min/max reduction for odd and non-power-of-two sizes, the grouped and the
# one-dispatch-per-mip paths, and the conservativeness of the occlusion test against a CPU
# ray-cast reference; test_two_phase.c: an occluder wall (rejection, reveal without a pop),
# camera cuts, new instances, occlusion off. occlusion_fixture.c is their fixture: a GPU scene of
# boxes and a CPU ray-cast phase-1 depth (raster does not exist yet). The GPU tests run per
# backend (_metal, _vulkan, _d3d12; SKIP without the device; CI: lavapipe and WARP golden jobs).
set(_de_occ_libs de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support)
if(TARGET de_gpu_vulkan)
    list(APPEND _de_occ_libs de_gpu_vulkan de_tp_vulkan_headers)
endif()
if(TARGET de_gpu_metal)
    list(APPEND _de_occ_libs de_gpu_metal "-framework Metal" "-framework Foundation")
endif()
if(DE_HAS_GPU_D3D12)
    list(APPEND _de_occ_libs de_gpu_d3d12 de_tp_d3d12_agility)
endif()
de_add_test(NAME render_occlusion
    SOURCES test_hzb.c test_two_phase.c occlusion_fixture.c render_d3d12_test.c
    LIBS ${_de_occ_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_occlusion)
    target_include_directories(test_render_occlusion PRIVATE "${DE_ROOT_DIR}/src")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_occlusion PRIVATE version)
        de_d3d12_agility_deploy(test_render_occlusion)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_occlusion)
        endif()
    endif()
endif()

# DENG-234: the mesh-shader vis-buffer raster (src/render/passes/vis/raster_mesh.c; engine shader
# vis_mesh, shaders/visbuffer/vis_mesh.slang and vis_decode.slang), executable
# test_render_raster_mesh; CTest names render_raster_mesh.<suite>.<test>. Meshes of grid patches
# with every buffer the raster reads (meshoptimizer meshlet bounds, as the cooker's); the vis IDs
# and the depth are compared with a CPU rasterizer. raster_mesh.*_{metal,vulkan,d3d12} SKIP
# without the device or mesh shaders (CI: lavapipe and WARP golden jobs); the CPU tests parse the
# shaders and the embedded cook.
de_add_test(NAME render_raster_mesh
    SOURCES test_raster_mesh.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_tp_meshoptimizer
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_raster_mesh)
    target_include_directories(test_render_raster_mesh PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_raster_mesh PRIVATE
        "DE_VIS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_raster_mesh PRIVATE version)
        de_d3d12_agility_deploy(test_render_raster_mesh)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_raster_mesh)
        endif()
    endif()
endif()

# DENG-235: the compute-expansion raster path (src/render/passes/vis/raster_compute.c, the engine
# shaders vis_raster_expand and vis_raster_draw), executable test_render_raster_compute; CTest
# names render_raster_compute.<suite>.<test>. Meshes of real triangles (meshlet bounds from
# meshoptimizer, as the cooker's); the vis ID and depth images against a CPU rasterizer, both
# culling phases, the overflow clamp, and the mesh path (DENG-234) where the device has it. The GPU tests
# run per backend (_metal, _vulkan, _d3d12; SKIP without the device; CI: Vulkan on lavapipe and
# D3D12 on the vendored WARP in the golden jobs); raster_compute.layout_match parses
# shaders/visbuffer/vis_expand.slang and vis_vs_ps.slang.
de_add_test(NAME render_raster_compute
    SOURCES test_raster_compute.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_tp_meshoptimizer
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_raster_compute)
    target_include_directories(test_render_raster_compute PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_raster_compute PRIVATE
        "DE_VIS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_raster_compute PRIVATE version)
        de_d3d12_agility_deploy(test_render_raster_compute)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_raster_compute)
        endif()
    endif()
endif()

# DENG-236: the alpha-tested raster (src/render/passes/vis/raster_alpha.c; engine shader
# vis_alpha, shaders/visbuffer/vis_alpha.slang), executable test_render_raster_alpha; CTest names
# render_raster_alpha.<suite>.<test>. A cutout quad (checker alpha) against a CPU rasterizer on both
# raster paths, the alpha bucket's routing, an empty bucket, and a cutout wall in phase 1's HZB.
# raster_alpha.*_{metal,vulkan,d3d12} SKIP without the device (the mesh-path test also without mesh
# shaders; CI: lavapipe and WARP golden jobs).
de_add_test(NAME render_raster_alpha
    SOURCES test_raster_alpha.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_raster_alpha)
    target_include_directories(test_render_raster_alpha PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_raster_alpha PRIVATE
        "DE_VIS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_raster_alpha PRIVATE version)
        de_d3d12_agility_deploy(test_render_raster_alpha)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_raster_alpha)
        endif()
    endif()
endif()

# DENG-237: material classification (src/render/passes/vis/classify.c, the engine shader
# vis_classify), executable test_render_classify; CTest names render_classify.<suite>.<test>.
# Synthetic GPU-scene tables, visible lists and vis-ID images drive the classify stage alone; the
# per-model tile lists, the background list, the masks and the indirect arguments are compared
# with a CPU reference. The GPU tests run per backend (_metal, _vulkan, _d3d12; SKIP without the
# device; CI: lavapipe and WARP golden jobs); classify.layout_match parses
# shaders/visbuffer/classify.slang.
de_add_test(NAME render_classify
    SOURCES test_classify.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_classify)
    target_include_directories(test_render_classify PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_classify PRIVATE
        "DE_VIS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_classify PRIVATE version)
        de_d3d12_agility_deploy(test_render_classify)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_classify)
        endif()
    endif()
endif()

# DENG-239: vis-buffer picking (src/render/passes/vis/picking.c; engine shader vis_pick,
# shaders/visbuffer/pick.slang), executable test_render_picking; CTest names
# render_picking.<suite>.<test>. Picks on both raster paths checked against known triangles and
# against the vis buffer read back, background and outside pixels, the latency and keep windows,
# the render_scale mapping and the layer mask; the request cap, the public API and the pass list on
# the null backend. picking.*_{metal,vulkan,d3d12} SKIP without the device (the mesh-path test also
# without mesh shaders; CI: lavapipe and WARP golden jobs).
de_add_test(NAME render_picking
    SOURCES test_picking.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_picking)
    target_include_directories(test_render_picking PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_picking PRIVATE
        "DE_VIS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_picking PRIVATE version)
        de_d3d12_agility_deploy(test_render_picking)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_picking)
        endif()
    endif()
endif()

# DENG-238: the material resolve (src/render/passes/vis/resolve.c, gbuffer.h; the engine shader
# vis_resolve), executable test_render_resolve; CTest names render_resolve.<suite>.<test>. Planes of
# meshlet patches with every vertex stream and test textures go through the whole vis frame; the
# reconstructed positions and UVs against a CPU ray cast, motion vectors, the feedback, the Unlit
# dispatch and the tile counts. texture_lod_vs_hw cooks tests/unit/render/data/resolve_lod_ref.slang
# at test time (DE_RS_TEST_COOK; it SKIPs without the shader importer). The GPU tests run per
# backend (_metal, _vulkan, _d3d12; SKIP without the device; CI: lavapipe and WARP golden jobs).
de_add_test(NAME render_resolve
    SOURCES test_resolve.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_resolve)
    target_include_directories(test_render_resolve PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_resolve PRIVATE
        "DE_VIS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\""
        "DE_RS_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_render_resolve PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_render_resolve PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_render_resolve PRIVATE DE_RS_TEST_COOK=1)
    endif()
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_resolve PRIVATE version)
        de_d3d12_agility_deploy(test_render_resolve)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_resolve)
        endif()
    endif()
endif()

# DENG-298: the BSDF modules (shaders/brdf/), the material interface and the material model
# registry, executable test_render_brdf; CTest names render_brdf.<suite>.<test>. brdf.* check the C
# reference (brdf_ref.c, test-only: not part of de_render): the VNDF pdf integrates to 1, the
# albedo estimator matches a quadrature, reciprocity, Fresnel. brdf.gpu_*_<backend> cook
# tests/unit/render/data/brdf_kernels.slang at test time (DE_BRDF_TEST_COOK; the engine's shaders/
# and data/ on one root) and compare the GPU's values with the C reference (_metal, _vulkan,
# _d3d12; SKIP without the device or the shader importer). material_model.* check the feature
# flags against shaders/materials/material_interface.slang, the registry's ids and limits, and
# de_material_model_register with the data/material_model_*.slang shader assets (null backend).
de_add_test(NAME render_brdf
    SOURCES test_brdf.c brdf_ref.c brdf_ref.h render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_brdf)
    target_include_directories(test_render_brdf PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_brdf PRIVATE
        "DE_BRDF_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\""
        "DE_BRDF_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_render_brdf PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_render_brdf PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_render_brdf PRIVATE DE_BRDF_TEST_COOK=1)
    endif()
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_brdf PRIVATE version)
        de_d3d12_agility_deploy(test_render_brdf)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_brdf)
        endif()
    endif()
endif()

# DENG-299: the Standard material model's base (shaders/materials/standard/, shaders/brdf/energy.slang)
# and its energy LUT (src/render/materials/brdf_luts.c), executable test_render_standard; CTest
# names render_standard.standard.<test>. lut_reference_sane checks the CPU reference; the GPU tests
# (_metal, _vulkan, _d3d12; SKIP without the device or the shader importer) make the LUT with the
# engine shader brdf_lut_gen and cook tests/unit/render/data/standard_kernels.slang at test time
# (DE_STD_TEST_COOK; the engine's shaders/ and data/ on one root): the LUT against the CPU, the
# white and dielectric furnaces, sample / pdf consistency, the KHR F0 rules, emission.
de_add_test(NAME render_standard
    SOURCES test_standard_base.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_standard)
    target_include_directories(test_render_standard PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_standard PRIVATE
        "DE_STD_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\""
        "DE_STD_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_render_standard PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_render_standard PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_render_standard PRIVATE DE_STD_TEST_COOK=1)
    endif()
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_standard PRIVATE version)
        de_d3d12_agility_deploy(test_render_standard)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_standard)
        endif()
    endif()
endif()

# DENG-303: clustered lighting (src/render/lighting/; engine shaders lighting_clusters,
# lighting_cull, lighting_deferred), executable test_render_clustered_lights; CTest names
# render_clustered_lights.<suite>.<test>. The GPU tests run the whole vis frame and the lighting
# build over planes and lights of a fake render world and compare the lighting target, the split
# targets and the cluster lists with CPU references (Lambert materials, light_data.c's
# units and cluster arithmetic, a numerical form factor for the area lights); per backend (_metal,
# _vulkan, _d3d12; SKIP without the device; CI: lavapipe and WARP golden jobs). The CPU tests
# check the units, the LTC integral and tables, the grid, a conservative random cluster test and
# the shader constants and struct sizes (layout_match parses shaders/lighting/).
de_add_test(NAME render_clustered_lights
    SOURCES test_clustered_lights.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_clustered_lights)
    target_include_directories(test_render_clustered_lights PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_clustered_lights PRIVATE
        "DE_LIGHTS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_clustered_lights PRIVATE version)
        de_d3d12_agility_deploy(test_render_clustered_lights)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_clustered_lights)
        endif()
    endif()
endif()

# DENG-300: the Standard model's layers (shaders/materials/standard/layer_*.slang, shaders/brdf/sheen
# and thin_film.slang; the sheen rows of the LUT in src/render/materials/brdf_luts.c), executable
# test_render_standard_layers; CTest names render_standard_layers.layers.<test>. The GPU tests
# (_metal, _vulkan, _d3d12; SKIP without the device or the shader importer) cook
# tests/unit/render/data/standard_layers_kernels.slang at test time (DE_LAY_TEST_COOK; the engine's
# shaders/ and data/ on one root): furnaces, the sheen LUT against the CPU, anisotropy and
# iridescence identities, the thin film against a spectral reference, sample / pdf consistency.
# flags_compile_out cooks data/standard_layers_flags.slang to SPIR-V and compares code sizes.
de_add_test(NAME render_standard_layers
    SOURCES test_standard_layers.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_standard_layers)
    target_include_directories(test_render_standard_layers PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_standard_layers PRIVATE
        "DE_LAY_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\""
        "DE_LAY_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_render_standard_layers PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_render_standard_layers PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_render_standard_layers PRIVATE DE_LAY_TEST_COOK=1)
    endif()
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_standard_layers PRIVATE version)
        de_d3d12_agility_deploy(test_render_standard_layers)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_standard_layers)
        endif()
    endif()
endif()

# DENG-304: image-based lighting, EV100 exposure and the basic output pass (src/render/lighting/
# ibl.c, exposure.c, src/render/passes/output_basic.c; shaders/lighting/ibl.slang,
# shaders/post/output_basic.slang), executable test_render_ibl_exposure; CTest names
# render_ibl_exposure.<suite>.<test>. The GPU tests (_metal, _vulkan, _d3d12; SKIP without the
# device; CI: lavapipe and WARP golden jobs) light planes of a fake render world with a synthetic
# RGBA16F environment cube: the diffuse and metal white furnaces, rotation, the ambient fallback,
# the EV100 scale of lighting and emission, the output kernel's clamp and sRGB OETF, and a forward
# kernel calling evaluate_indirect (tests/unit/render/data/ibl_forward.slang, cooked at test time:
# DE_IBL_TEST_COOK; SKIP without the shader importer) equal to the deferred pass.
de_add_test(NAME render_ibl_exposure
    SOURCES test_ibl_exposure.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_ibl_exposure)
    target_include_directories(test_render_ibl_exposure PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_ibl_exposure PRIVATE
        "DE_IBL_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\""
        "DE_IBL_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_render_ibl_exposure PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_render_ibl_exposure PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_render_ibl_exposure PRIVATE DE_IBL_TEST_COOK=1)
    endif()
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_ibl_exposure PRIVATE version)
        de_d3d12_agility_deploy(test_render_ibl_exposure)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_ibl_exposure)
        endif()
    endif()
endif()

# DENG-301: the Standard model's transmission lobes (shaders/materials/standard/lobe_transmission.slang,
# volume.slang) and the Unlit model (shaders/materials/unlit/unlit.slang), executable
# test_render_transmission; CTest names render_transmission.transmission.<test>. The GPU tests
# (_metal, _vulkan, _d3d12; SKIP without the device or the shader importer) cook
# tests/unit/render/data/transmission_kernels.slang at test time (DE_TRN_TEST_COOK; the engine's
# shaders/ and data/ on one root): energy conservation, IOR 1 pass-through, TIR, Beer-Lambert,
# diffuse transmission, sample / pdf consistency, Unlit's exact output, the forward flags.
de_add_test(NAME render_transmission
    SOURCES test_transmission_unlit.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_transmission)
    target_include_directories(test_render_transmission PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_transmission PRIVATE
        "DE_TRN_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\""
        "DE_TRN_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_render_transmission PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_render_transmission PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_render_transmission PRIVATE DE_TRN_TEST_COOK=1)
    endif()
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_transmission PRIVATE version)
        de_d3d12_agility_deploy(test_render_transmission)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_transmission)
        endif()
    endif()
endif()

# DENG-302: material instances (src/render/materials/material_instances.c, material_params.h,
# shaders/materials/material_params.slang), executable test_render_material_instances; CTest names
# render_material_instances.materials.<test>: the parameter block layout, the feature rules, no
# pipeline for parameter changes (1000 materials a frame), dirty uploads, permutation switches,
# texture hot reload rebinding, runtime instances, mesh default materials. Null backend, the real
# asset system and ECS loop.
de_add_test(NAME render_material_instances
    SOURCES test_material_instances.c
    LIBS de_render de_asset de_gpu de_ecs de_reflect de_task de_platform de_test_support
    LABELS unit render
    TIMEOUT 300)
if(TARGET test_render_material_instances)
    target_include_directories(test_render_material_instances PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_render_material_instances_tmp")
    target_compile_definitions(test_render_material_instances PRIVATE
        "DE_RMI_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/test_render_material_instances_tmp\""
        "DE_RMI_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\""
        "DE_RMI_TEST_TEXTURE_DIR=\"${DE_ROOT_DIR}/tests/unit/import/data/texture\"")
    target_link_libraries(test_render_material_instances PRIVATE
        "$<$<AND:$<CONFIG:Debug,Dev>,$<TARGET_EXISTS:de_cooker>>:de_cooker>")
endif()

# DENG-305: the forward transparent pass (src/render/passes/forward/; engine shader
# forward_transparent), executable test_render_forward_transparent; CTest names
# render_forward_transparent.forward.<test>. The GPU tests run the whole vis frame, lighting and the
# forward pass over planes of a fake render world: routing out of the vis buffer, no work without a
# visible transparent, back-to-front compositing against a CPU blend, forward against deferred
# lighting, double-sided draws and the scene-colour copy; per backend (_metal, _vulkan, _d3d12; SKIP
# without the device; CI: lavapipe and WARP golden jobs). The CPU tests check the sort, the routing
# predicate and the shader constants (layout_match parses shaders/forward/ and meshlet_cull.slang).
de_add_test(NAME render_forward_transparent
    SOURCES test_forward_transparent.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_forward_transparent)
    target_include_directories(test_render_forward_transparent PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_forward_transparent PRIVATE
        "DE_FWD_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_forward_transparent PRIVATE version)
        de_d3d12_agility_deploy(test_render_forward_transparent)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_forward_transparent)
        endif()
    endif()
endif()

# DENG-306: screen-space separable subsurface scattering with the Burley profile
# (src/render/passes/sss/, shaders/sss/), executable test_render_sss; CTest names
# render_sss.sss.<test>. The CPU tests check the kernel tables against the numerically integrated
# profile, the shader constants and SssParams' size, and (a null-backend renderer) that sss off
# dispatches nothing. The GPU tests (_metal, _vulkan, _d3d12; SKIP without the device) run the
# vis frame, lighting and SSS over planes (lighting_fixture.h) and compare with sss off: unflagged
# pixels bitwise, a depth edge without bleeding, a constant world-space radius at two distances.
de_add_test(NAME render_sss
    SOURCES test_sss.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_sss)
    target_include_directories(test_render_sss PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_sss PRIVATE
        "DE_SSS_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_sss PRIVATE version)
        de_d3d12_agility_deploy(test_render_sss)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_sss)
        endif()
    endif()
endif()

# DENG-340: real RTT radiance capture, probe storage, GPU prefilter and bake contracts.
de_add_test(NAME render_reflection_probes
    SOURCES test_reflection_probes.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_reflection_probes)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/probe_tmp")
    target_compile_definitions(test_render_reflection_probes PRIVATE "DE_PROBE_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/probe_tmp\"")
    target_include_directories(test_render_reflection_probes PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    if(DE_HAS_COOKER)
        target_link_libraries(test_render_reflection_probes PRIVATE de_cooker)
    endif()
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_reflection_probes PRIVATE version)
        de_d3d12_agility_deploy(test_render_reflection_probes)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_reflection_probes)
        endif()
    endif()
endif()

# DENG-339: GPU closest-depth traversal and real-view SSR/history acceptance.
de_add_test(NAME render_ssr SOURCES test_ssr.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_golden_flip LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_ssr)
    target_include_directories(test_render_ssr PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    if(DE_HAS_COOKER)
        target_link_libraries(test_render_ssr PRIVATE de_cooker)
    endif()
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_ssr PRIVATE version)
        de_d3d12_agility_deploy(test_render_ssr)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_ssr)
        endif()
    endif()
endif()

# DENG-341: real irradiance-volume bake and screen-space diffuse GI acceptance.
de_add_test(NAME render_irradiance_ssgi SOURCES test_irradiance_ssgi.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_pack_lib LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_irradiance_ssgi)
    target_include_directories(test_render_irradiance_ssgi PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    # Parallel CTest can execute supported GPU backends together; packs and cache files must
    # not share a physical directory across the three reload fixture variants.
    foreach(_de_gi_backend IN ITEMS metal vulkan d3d12)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gi_tmp/${_de_gi_backend}")
    endforeach()
    target_compile_definitions(test_render_irradiance_ssgi PRIVATE "DE_GI_TEST_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/gi_tmp\"")
    if(DE_HAS_COOKER)
        target_link_libraries(test_render_irradiance_ssgi PRIVATE de_cooker)
    endif()
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_irradiance_ssgi PRIVATE version)
        de_d3d12_agility_deploy(test_render_irradiance_ssgi)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_irradiance_ssgi)
        endif()
    endif()
endif()

de_add_test(NAME render_csm
    SOURCES test_csm.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_golden_flip
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_csm)
    target_include_directories(test_render_csm PRIVATE "${DE_ROOT_DIR}/src")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_csm PRIVATE version)
        de_d3d12_agility_deploy(test_render_csm)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_csm)
        endif()
    endif()
endif()

de_add_test(NAME render_gtao
    SOURCES test_gtao.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_golden_flip
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_gtao)
    target_include_directories(test_render_gtao PRIVATE "${DE_ROOT_DIR}/src")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_gtao PRIVATE version)
        de_d3d12_agility_deploy(test_render_gtao)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_gtao)
        endif()
    endif()
endif()

# DENG-342: independent physical refraction and current-frame Gaussian-pyramid acceptance.
de_add_test(NAME render_ss_refraction
    SOURCES test_ss_refraction.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_golden_flip
    LABELS "unit;render;gpu" TIMEOUT 300)
if(TARGET test_render_ss_refraction)
    target_include_directories(test_render_ss_refraction PRIVATE "${DE_ROOT_DIR}/src")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_ss_refraction PRIVATE version)
        de_d3d12_agility_deploy(test_render_ss_refraction)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_ss_refraction)
        endif()
    endif()
endif()

# DENG-431: shared-production layered IBL, independent numeric/physical references, real
# probe/GI/SSR/AO bindings and inactive-feature identity. Per-backend actual GPU acceptance.
de_add_test(NAME render_ibl_layers
    SOURCES test_ibl_layers.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_ibl_layers)
    target_include_directories(test_render_ibl_layers PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_render_ibl_layers PRIVATE
        "DE_ILL_TEST_SHADER_DIR=\"${DE_ROOT_DIR}/shaders\""
        "DE_ILL_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
    if(DE_BUILD_TOOLS AND TARGET de_tp_slang AND TARGET de_gpu_shader)
        target_link_libraries(test_render_ibl_layers PRIVATE de_gpu_shader de_cook_shader de_cooker de_asset)
        target_include_directories(test_render_ibl_layers PRIVATE "${DE_ROOT_DIR}/tools")
        target_compile_definitions(test_render_ibl_layers PRIVATE DE_ILL_TEST_COOK=1)
    endif()
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_ibl_layers PRIVATE version)
        de_d3d12_agility_deploy(test_render_ibl_layers)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_ibl_layers)
        endif()
    endif()
endif()

# DENG-336: fixed-pool atlas allocation and local-light shadow cache acceptance.
de_add_test(NAME render_shadow_atlas
    SOURCES test_shadow_atlas.c render_d3d12_test.c
    LIBS ${_de_vis_libs}
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_shadow_atlas)
    target_include_directories(test_render_shadow_atlas PRIVATE "${DE_ROOT_DIR}/src")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_shadow_atlas PRIVATE version)
        de_d3d12_agility_deploy(test_render_shadow_atlas)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_shadow_atlas)
        endif()
    endif()
endif()

# DENG-337: physical PCSS and opaque direct-light contact acceptance.
de_add_test(NAME render_pcss_contact
    SOURCES test_pcss_contact.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_tp_stb de_golden_flip
    LABELS unit gpu render
    TIMEOUT 300)
if(TARGET test_render_pcss_contact)
    target_compile_definitions(test_render_pcss_contact PRIVATE
        "DE_PCSS_GAP_REFERENCE=\"${DE_ROOT_DIR}/tests/golden/refs/metal/base/contact_gap.png\"")
    target_include_directories(test_render_pcss_contact PRIVATE "${DE_ROOT_DIR}/src")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_pcss_contact PRIVATE version)
        de_d3d12_agility_deploy(test_render_pcss_contact)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_pcss_contact)
        endif()
    endif()
endif()

# DENG-44: production renderer scene AS and shared alpha opacity.
de_add_test(NAME render_scene_as SOURCES test_scene_as.c render_d3d12_test.c
    LIBS ${_de_vis_libs} LABELS unit gpu render TIMEOUT 180)
if(TARGET test_render_scene_as)
    target_include_directories(test_render_scene_as PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/scene_as_tmp")
    target_compile_definitions(test_render_scene_as PRIVATE
        "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/scene_as_tmp\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_scene_as PRIVATE version)
        de_d3d12_agility_deploy(test_render_scene_as)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_scene_as)
        endif()
    endif()
endif()

# DENG-45: production analytic RT shadows, physical sampling and native readback.
de_add_test(NAME render_rt_shadows SOURCES test_rt_shadows.c render_d3d12_test.c
    LIBS ${_de_vis_libs} LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_rt_shadows)
    target_include_directories(test_render_rt_shadows PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rt_shadows_tmp")
    target_compile_definitions(test_render_rt_shadows PRIVATE
        "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/rt_shadows_tmp\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_rt_shadows PRIVATE version)
        de_d3d12_agility_deploy(test_render_rt_shadows)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_rt_shadows)
        endif()
    endif()
endif()

# DENG-46: actual source aliasing, reservoir reuse and many-light renderer acceptance.
de_add_test(NAME render_restir_di SOURCES test_restir_di.c render_d3d12_test.c
    LIBS ${_de_vis_libs} LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_restir_di)
    target_include_directories(test_render_restir_di PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/restir_di_tmp")
    target_compile_definitions(test_render_restir_di PRIVATE
        "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/restir_di_tmp\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_restir_di PRIVATE version)
        de_d3d12_agility_deploy(test_render_restir_di)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_restir_di)
        endif()
    endif()
endif()

# DENG-47: actual renderer RTAO and fallback acceptance.
de_add_test(NAME render_rtao SOURCES test_rtao.c render_d3d12_test.c
    LIBS ${_de_vis_libs} LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_rtao)
    target_include_directories(test_render_rtao PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rtao_tmp")
    target_compile_definitions(test_render_rtao PRIVATE
        "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/rtao_tmp\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_rtao PRIVATE version)
        de_d3d12_agility_deploy(test_render_rtao)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_rtao)
        endif()
    endif()
endif()

# DENG-48 native closest-hit material shading.
de_add_test(NAME render_rt_reflections SOURCES test_rt_reflections.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_golden_flip LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_rt_reflections)
    target_include_directories(test_render_rt_reflections PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rt_reflections_tmp")
    target_compile_definitions(test_render_rt_reflections PRIVATE
        "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/rt_reflections_tmp\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_rt_reflections PRIVATE version)
        de_d3d12_agility_deploy(test_render_rt_reflections)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_rt_reflections)
        endif()
    endif()
endif()

# DENG-49 live DDGI source.
de_add_test(NAME render_rt_gi_probes SOURCES test_rt_gi_probes.c render_d3d12_test.c
    LIBS ${_de_vis_libs} LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_rt_gi_probes)
    target_include_directories(test_render_rt_gi_probes PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rt_gi_probes_tmp")
    target_compile_definitions(test_render_rt_gi_probes PRIVATE
        "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/rt_gi_probes_tmp\"")
    if(DE_HAS_GPU_D3D12)
        target_link_libraries(test_render_rt_gi_probes PRIVATE version)
        de_d3d12_agility_deploy(test_render_rt_gi_probes)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_rt_gi_probes)
        endif()
    endif()
endif()

# DENG-50: actual sorted-forward capture and alpha-aware native batch refraction.
de_add_test(NAME render_rt_refraction SOURCES test_rt_refraction.c render_d3d12_test.c
    LIBS ${_de_vis_libs} LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_rt_refraction)
    target_include_directories(test_render_rt_refraction PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rt_refraction_tmp")
    target_compile_definitions(test_render_rt_refraction PRIVATE "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/rt_refraction_tmp\"")
    if(WIN32)
        target_link_libraries(test_render_rt_refraction PRIVATE version)
        de_d3d12_agility_deploy(test_render_rt_refraction)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_rt_refraction)
        endif()
    endif()
endif()

# Original S16 combined production scene and all-active allocation exit proof.
de_add_test(NAME render_s16_rt_integration SOURCES test_s16_rt_integration.c render_d3d12_test.c
    LIBS ${_de_vis_libs} LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_s16_rt_integration)
    target_include_directories(test_render_s16_rt_integration PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/s16_integration_tmp")
    target_compile_definitions(test_render_s16_rt_integration PRIVATE
        "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/s16_integration_tmp\"")
    if(WIN32)
        target_compile_definitions(test_render_s16_rt_integration PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(test_render_s16_rt_integration PRIVATE version)
        de_d3d12_agility_deploy(test_render_s16_rt_integration)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_s16_rt_integration)
        endif()
    endif()
endif()

# S17 progressive path tracer using the real renderer/scene and RT pipelines.
de_add_test(NAME render_pathtracer SOURCES test_pathtracer.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_golden_flip LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_pathtracer)
    target_include_directories(test_render_pathtracer PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pathtracer_tmp")
    target_compile_definitions(test_render_pathtracer PRIVATE "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/pathtracer_tmp\"" "DE_PT_REFERENCE=\"${CMAKE_CURRENT_SOURCE_DIR}/data/pt_cornell_4096.f32\"")
    if(WIN32)
        target_compile_definitions(test_render_pathtracer PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(test_render_pathtracer PRIVATE version)
        de_d3d12_agility_deploy(test_render_pathtracer)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_pathtracer)
        endif()
    endif()
endif()

# S17 participating media and random-walk SSS, real native integrator readback.
de_add_test(NAME render_pt_media SOURCES test_pt_media.c render_d3d12_test.c
    LIBS ${_de_vis_libs} LABELS unit gpu render TIMEOUT 300)
if(TARGET test_render_pt_media)
    target_include_directories(test_render_pt_media PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pt_media_tmp")
    target_compile_definitions(test_render_pt_media PRIVATE "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/pt_media_tmp\"")
    if(WIN32)
        target_compile_definitions(test_render_pt_media PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(test_render_pt_media PRIVATE version)
        de_d3d12_agility_deploy(test_render_pt_media)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_pt_media)
        endif()
    endif()
endif()

# Shared S17 deterministic graph denoiser blocks.
de_add_test(NAME render_denoise_framework SOURCES test_denoise_framework.c
    LIBS ${_de_vis_libs} LABELS unit gpu render TIMEOUT 120)
if(TARGET test_render_denoise_framework)
    target_include_directories(test_render_denoise_framework PRIVATE "${DE_ROOT_DIR}/src")
endif()

# S17 production shadow/AO policies and actual fixed-spp references.
de_add_test(NAME render_denoise_shadow_ao SOURCES test_denoise_shadow_ao.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_golden_flip LABELS unit gpu render TIMEOUT 180)
if(TARGET test_render_denoise_shadow_ao)
    target_include_directories(test_render_denoise_shadow_ao PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/denoise_shadow_ao_tmp")
    target_compile_definitions(test_render_denoise_shadow_ao PRIVATE "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/denoise_shadow_ao_tmp\"")
    if(WIN32)
        target_compile_definitions(test_render_denoise_shadow_ao PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(test_render_denoise_shadow_ao PRIVATE version)
        de_d3d12_agility_deploy(test_render_denoise_shadow_ao)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_denoise_shadow_ao)
        endif()
    endif()
endif()

# S17 production diffuse/specular policies, independent fixed-spp references.
de_add_test(NAME render_denoise_gi_spec SOURCES test_denoise_gi_spec.c render_d3d12_test.c
    LIBS ${_de_vis_libs} de_golden_flip LABELS unit gpu render TIMEOUT 180)
if(TARGET test_render_denoise_gi_spec)
    target_include_directories(test_render_denoise_gi_spec PRIVATE "${DE_ROOT_DIR}/src")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/denoise_gi_spec_tmp")
    target_compile_definitions(test_render_denoise_gi_spec PRIVATE "DE_SCENE_AS_TMP=\"${CMAKE_CURRENT_BINARY_DIR}/denoise_gi_spec_tmp\"")
    if(WIN32)
        target_compile_definitions(test_render_denoise_gi_spec PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(test_render_denoise_gi_spec PRIVATE version)
        de_d3d12_agility_deploy(test_render_denoise_gi_spec)
        if(DE_D3D12_WARP_DLL)
            de_d3d12_warp_deploy(test_render_denoise_gi_spec)
        endif()
    endif()
endif()
