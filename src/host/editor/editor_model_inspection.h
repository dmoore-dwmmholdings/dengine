#ifndef DE_EDITOR_MODEL_INSPECTION_H
#define DE_EDITOR_MODEL_INSPECTION_H
#include <de_editor_protocol.h>
#include "render/loaders/render_assets.h"

/* Main-thread snapshot adapter. Strings and float spans borrow loaded records only until
 * the caller serializes this response; all variable-size result records use its arena. */
typedef struct de__ed_model_names {
    const char *(*handle)(void *user, u32 handle);
    const char *(*asset)(void *user, de_asset_id id);
    void *user;
} de__ed_model_names;
de_result de__ed_model_inspect(const de_render_model *model, const de_render_mesh *mesh,
                               u32 selected, const de__ed_model_names *names, de_ep_arena *arena,
                               de_ep_model_inspection *out);
#endif
