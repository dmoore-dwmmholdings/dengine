# Unit tests of the .dpak pack writer (tools/pack/lib, DENG-211). CTest names dpak_writer.<test>.
# The writer is a tool library, so the tests exist only when tools are built. tests/ is added
# before tools/, so test for the option, not for the target.
if(NOT DE_BUILD_TOOLS)
    return()
endif()

de_add_test(NAME dpak_writer SOURCES test_dpak_writer.c LIBS de_pack_lib de_platform de_tp_zstd)
if(TARGET test_dpak_writer)
    # dpak_format.h ("asset/dpak/dpak_format.h") and the VFS test hook (platform/vfs_internal.h).
    target_include_directories(test_dpak_writer PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_dpak_writer PRIVATE
        DE_PACK_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_dpak_writer_tmp")
endif()
