#ifndef DE_TEXT_ATLAS_PACKER_H
#define DE_TEXT_ATLAS_PACKER_H
#include <dengine/de_base.h>
#define DE_ATLAS_SKYLINE_MAX 4098
typedef struct de_atlas_node {
    u32 x, y, width;
} de_atlas_node;
typedef struct de_atlas_packer {
    u32 width, height, padding, count;
    u64 used;
    de_atlas_node nodes[DE_ATLAS_SKYLINE_MAX];
} de_atlas_packer;
void de_atlas_packer_init(de_atlas_packer *p, u32 width, u32 height, u32 padding);
bool de_atlas_packer_insert(de_atlas_packer *p, u32 width, u32 height, u32 *x, u32 *y);
#endif
