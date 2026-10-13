/*
 * host_packs.h: where a packed game finds its packs (SPEC §11.5, §11.7; DENG-213). Internal to
 * the host; include as "host/host_packs.h".
 *
 * In PACKED mode (Shipping) engine create mounts every `*.dpak` file directly in `game:/` (the
 * executable's directory, or de_host_desc.game_dir), so `de-cook cook --out <game dir>` output
 * runs as is. See the Trama page "Decision: Packed builds mount every .dpak in game:/ at engine
 * create; later file names override earlier ones".
 */
#ifndef DE_HOST_PACKS_H
#define DE_HOST_PACKS_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

/** The virtual directory a PACKED-mode engine mounts its packs from. */
#define DE__HOST_PACK_DIR "game:/"

/**
 * Mounts every file of the virtual directory `dir` whose name ends in `.dpak` (exact case),
 * at priority 0, in byte order of the file names (strcmp): among equal priorities the later
 * mount wins, so `game.dpak` is overridden by `game_patch1.dpak`, which `zz_mod.dpak` overrides
 * in turn. Subdirectories are not searched. `*count` (optional) gets the number mounted.
 * Temporaries come from `a` (NULL = de_heap_allocator()). Main thread.
 * @return DE_OK (also for a directory without packs); DE_ERR_NOT_FOUND (no such directory) and
 *         the errors of de_dir_list; the first de_asset_pack_mount failure (the packs mounted
 *         before it stay mounted; the error context names the pack); DE_ERR_OUT_OF_MEMORY.
 */
de_result de__host_mount_packs(de_asset_system *s, const char *dir, const de_allocator *a,
                               u32 *count);

DE_EXTERN_C_END

#endif /* DE_HOST_PACKS_H */
