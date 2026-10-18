#include "golden.h"
/* Original S14 loader and assets remain canonical, including this gallery subset. */
void de__golden_pathtracer_assets(de_golden_ctx *, bool cornell);
DE_GOLDEN_SCENE(pathtracer_cornell) {
    de__golden_pathtracer_assets(ctx, true);
}
DE_GOLDEN_SCENE(pathtracer_gallery_subset) {
    de__golden_pathtracer_assets(ctx, false);
}
