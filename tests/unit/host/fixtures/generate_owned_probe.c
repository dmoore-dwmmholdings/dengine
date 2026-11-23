/* Standalone test-data generator, linked to de_pack_lib and its normal dependencies.
 * Run with tests/unit/host/data as argv[1]; no renderer or game is started. */
#include "dpak_writer.h"
#include <dengine/de_platform.h>
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    if (de_fs_init(&(de_fs_desc){.game_dir = argv[1]}) != DE_OK)
        return 1;
    void *bytes = NULL;
    u64 size = 0;
    de_result result = de_file_read_all("game:/owned_probe.bin", NULL, &bytes, &size);
    dpak_writer *writer = NULL;
    if (result == DE_OK)
        result =
            dpak_writer_create(&(dpak_writer_desc){.alignment = 8, .include_names = true}, &writer);
    if (result == DE_OK)
        result = dpak_writer_add(writer, de_hash64_str("probe"), "host_owned_probe", bytes, size,
                                 NULL, 0, "probe");
    if (result == DE_OK)
        result = de_fs_mount("generated", argv[1], DE_MOUNT_WRITABLE, 0);
    if (result == DE_OK)
        result = dpak_writer_finish(writer, "generated:/owned_probe.dpak");
    dpak_writer_destroy(writer);
    if (bytes)
        DE_FREE(de_heap_allocator(), bytes, (size_t)size + 1u);
    de_fs_shutdown();
    if (result != DE_OK)
        fprintf(stderr, "owned probe pack generation failed: %d\n", result);
    return result == DE_OK ? 0 : 1;
}
