# tests/unit/gpu/metal: the Metal backend (DENG-287), executable test_gpu_metal, CTest names
# gpu_metal.<test>, labels unit;gpu;metal. Runs on the host GPU with Metal API validation on
# (test_mtl_device.c sets MTL_DEBUG_LAYER=1 before the first device). The tests are C and include
# src/gpu/metal/mtl_internal.h; mtl_test_helpers.m is the Objective-C side (independent Metal
# queries, a compute kernel that reads the bindless table). Tests skip without a Metal device, and
# the GPU read skips below the Base tier.
if(NOT TARGET de_gpu_metal)
    return()
endif()

de_add_test(NAME gpu_metal
    SOURCES test_mtl_device.c test_mtl_resources.c mtl_test_helpers.m
    LIBS de_gpu de_gpu_metal "-framework Metal" "-framework Foundation"
    LABELS unit gpu
    TIMEOUT 300)
if(TARGET test_gpu_metal)
    set_source_files_properties(mtl_test_helpers.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(test_gpu_metal PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_gpu_metal PRIVATE "DE_TEST_ROOT_DIR=\"${DE_ROOT_DIR}\"")
endif()
