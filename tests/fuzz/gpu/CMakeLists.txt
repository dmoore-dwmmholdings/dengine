# tests/fuzz/gpu: fuzz targets of module `gpu`. DENG-324: fuzz_d3d12_dxil, the D3D12 backend's
# portable readers (src/gpu/d3d12/): the DXIL container and bitcode reader
# (de__d3d12_dxil_scan) and the pipeline library file check. The seed corpus holds the hand-built
# container of the unit test d3d12.dxil_scan and a library file. Without the D3D12 backend the
# target compiles the portable parts of d3d12_pipeline.c and d3d12_pipeline_library.c itself.
if(NOT DE_HAS_GPU)
    return()
endif()
if(DE_HAS_GPU_D3D12)
    set(_de_dxil_fuzz_sources fuzz_d3d12_dxil.c)
    set(_de_dxil_fuzz_libs de_gpu_d3d12 de_tp_d3d12_agility de_gpu)
else()
    set(_de_dxil_fuzz_sources fuzz_d3d12_dxil.c
        "${DE_ROOT_DIR}/src/gpu/d3d12/d3d12_pipeline.c"
        "${DE_ROOT_DIR}/src/gpu/d3d12/d3d12_pipeline_library.c")
    set(_de_dxil_fuzz_libs de_gpu)
endif()
de_add_fuzz_target(NAME d3d12_dxil
                   SOURCES ${_de_dxil_fuzz_sources}
                   LIBS ${_de_dxil_fuzz_libs}
                   CORPUS corpus/d3d12_dxil)
if(TARGET fuzz_d3d12_dxil)
    target_include_directories(fuzz_d3d12_dxil PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-409: fuzz_shader_view, the cooked `shader` asset check (de__shader_view_init in
# src/gpu/shader/shader_type.c) and the view accessors. The seed corpus holds blobs cooked by
# `de-cook cook` (SPIR-V and metallib, SPIR-V only) and small hand-made blobs at the format's edges.
if(TARGET de_gpu_shader)
    de_add_fuzz_target(NAME shader_view
                       SOURCES fuzz_shader_view.c
                       LIBS de_gpu_shader de_asset de_gpu de_core
                       CORPUS corpus/shader_view)
    if(TARGET fuzz_shader_view)
        target_include_directories(fuzz_shader_view PRIVATE "${DE_ROOT_DIR}/src")
    endif()
endif()
