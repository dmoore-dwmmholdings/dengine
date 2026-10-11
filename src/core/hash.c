/* hash.c: 64-bit hashing over XXH3-64 (SPEC §5.6, §11.1).
 *
 * The values end up in cooked data (asset IDs, de_name), so the algorithm and the seed are part of
 * the pack format. de_tp_xxhash is the only xxHash in the engine (zstd's copy is renamed). */
#include <dengine/de_base.h>

#include <string.h>

#include <xxhash.h>

u64 de_hash64(const void *data, size_t len, u64 seed) {
    /* XXH3 reads no bytes for len == 0, but keep NULL out of the library anyway. */
    if (len == 0)
        data = "";
    return (u64)XXH3_64bits_withSeed(data, len, (XXH64_hash_t)seed);
}

u64 de_hash64_str(const char *str) {
    if (str == NULL)
        return de_hash64("", 0, 0);
    return de_hash64(str, strlen(str), 0);
}
