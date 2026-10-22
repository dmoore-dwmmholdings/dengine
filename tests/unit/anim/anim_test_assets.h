#ifndef DE_ANIM_TEST_ASSETS_H
#define DE_ANIM_TEST_ASSETS_H
#include "anim/anim_internal.h"
#include "render/formats/de_cooked_le.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct anim_blob {
    u8 *data;
    u32 bytes;
} anim_blob;
static void put_u16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
static u32 aligned(u32 value) {
    return (value + 7) & ~7u;
}
static anim_blob skeleton_blob(u32 joints) {
    u32 parents = 112, names = aligned(parents + joints * 2), rest = aligned(names + joints * 8),
        inverse = aligned(rest + joints * 40), remap = aligned(inverse + joints * 48),
        size = aligned(remap + joints * 2);
    anim_blob blob = {calloc(1, size), size};
    u8 *p = blob.data;
    memcpy(p, "DSKL", 4);
    de_cooked_put_u32(p + 4, 1);
    de_cooked_put_u32(p + 12, joints);
    de_cooked_put_u32(p + 16, joints);
    de_cooked_put_u32(p + 20, 0);
    de_cooked_put_u32(p + 24, parents);
    de_cooked_put_u32(p + 28, names);
    de_cooked_put_u32(p + 32, rest);
    de_cooked_put_u32(p + 36, inverse);
    de_cooked_put_u32(p + 40, remap);
    de_cooked_put_u32(p + 52, size);
    de_affine identity = {{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}};
    for (u32 k = 0; k < 12; ++k)
        de_cooked_put_f32(p + 56 + k * 4, identity.m[k]);
    for (u32 j = 0; j < joints; ++j) {
        put_u16(p + parents + j * 2, j ? (u16)(j - 1) : DE_COOKED_SKEL_NO_PARENT);
        char name[24];
        snprintf(name, sizeof name, "joint%u", j);
        de_cooked_put_u64(p + names + j * 8, de_hash64_str(name));
        de_cooked_put_f32(p + rest + j * 40 + 4, j ? 1.f : 0.f);
        de_cooked_put_f32(p + rest + j * 40 + 24, 1);
        for (u32 c = 0; c < 3; ++c)
            de_cooked_put_f32(p + rest + j * 40 + 28 + c * 4, 1);
        de_affine bind = identity;
        bind.m[10] = -(f32)j;
        for (u32 k = 0; k < 12; ++k)
            de_cooked_put_f32(p + inverse + j * 48 + k * 4, bind.m[k]);
        put_u16(p + remap + j * 2, (u16)j);
    }
    return blob;
}
static anim_blob clip_blob(u32 joints) {
    /* Quantized translation, smallest-three quaternion, uniform scale. */
    u32 tracks = 80, times = 128, data = 136, size = 224;
    anim_blob blob = {calloc(1, size), size};
    u8 *p = blob.data;
    memcpy(p, "DCLP", 4);
    de_cooked_put_u32(p + 4, 1);
    de_cooked_put_u32(p + 12, joints);
    de_cooked_put_u32(p + 16, 3);
    de_cooked_put_u32(p + 20, 2);
    de_cooked_put_u32(p + 24, DE_COOKED_NONE);
    de_cooked_put_f32(p + 28, 1);
    de_cooked_put_f32(p + 32, 1);
    de_cooked_put_u32(p + 36, tracks);
    de_cooked_put_u32(p + 40, times);
    de_cooked_put_u32(p + 44, data);
    de_cooked_put_u32(p + 48, size - data);
    de_cooked_put_u32(p + 52, size);
    de_cooked_put_f32(p + times + 4, 1);
    u32 offsets[] = {0, 40, 56};
    for (u32 t = 0; t < 3; ++t) {
        u8 *track = p + tracks + t * 16;
        track[2] = (u8)t;
        track[3] = (u8)(t == 0   ? DE_COOKED_CLIP_ENC_QUANT16
                        : t == 1 ? DE_COOKED_CLIP_ENC_SMALLEST3
                                 : DE_COOKED_CLIP_ENC_RAW);
        track[4] = (u8)(t == 0 ? 3 : t == 1 ? 4 : 1);
        if (t == 2)
            track[5] = DE_COOKED_CLIP_TRACK_UNIFORM;
        de_cooked_put_u32(track + 8, 2);
        de_cooked_put_u32(track + 12, offsets[t]);
    }
    u8 *translation = p + data;
    de_cooked_put_f32(translation + 12, 2);
    put_u16(translation + 26, 1);
    put_u16(translation + 34, 65535);
    u8 *rotation = p + data + 40;
    put_u16(rotation + 2, 1);
    const f64 quats[2][4] = {{0, 0, 0, 1}, {0, 0, .7071067811865476, .7071067811865476}};
    for (u32 key = 0; key < 2; ++key) {
#if DE_ANIM_HAS_COOKER
        u16 words[3];
        f32 decoded[4];
        de_model_clip_s3_encode(quats[key], words, decoded);
        for (u32 w = 0; w < 3; ++w)
            put_u16(rotation + 4 + key * 6 + w * 2, words[w]);
#else
        u32 q = 16384;
        u64 bits = ((u64)3 << 45) | ((u64)q << 30) | ((u64)q << 15) | (key ? 32767 : q);
        for (u32 b = 0; b < 6; ++b)
            rotation[4 + key * 6 + b] = (u8)(bits >> (b * 8));
        (void)quats;
#endif
    }
    u8 *scale = p + data + 56;
    put_u16(scale + 2, 1);
    de_cooked_put_f32(scale + 4, 1);
    de_cooked_put_f32(scale + 8, 2);
    return blob;
}
#endif
