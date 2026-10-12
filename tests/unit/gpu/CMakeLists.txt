# tests/unit/gpu: unit tests of module `gpu` (de_gpu, SPEC §12.1). Later S09 tasks append their
# test sources or executables below.
#
# DENG-281: the front-end and the null backend. CTest names: gpu.<test> (suite gpu),
# gpu.handle_pool.<test>, gpu.gpu_validation.<test>. The tests include internal headers
# ("gpu/gpu_backend.h") for the null backend's manual fence control and the pool/bindless/deferred
# counters. Label `tsan`: gpu.threads_create_destroy_submit runs 8 threads on one device.
# The gpu module does not exist in DE_HEADLESS builds (cmake/DeOptions.cmake), so neither do these.
if(NOT DE_HAS_GPU)
    return()
endif()

de_add_test(NAME gpu
    SOURCES test_gpu_null.c test_handle_pool.c test_gpu_validation.c
    LIBS de_gpu
    LABELS unit tsan
    TIMEOUT 120)
if(TARGET test_gpu)
    target_include_directories(test_gpu PRIVATE "${DE_ROOT_DIR}/src")
endif()
