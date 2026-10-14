/* Smoke test for de_tp_stb: decode a 1x1 PNG with stb_image, check that stb_vorbis links and
 * rejects data that is not Ogg Vorbis, and that a PNG from stb_image_write (DENG-327) decodes to
 * the same pixels. */
#include <de_stb_vorbis.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <string.h>

#include "de_test.h"

/* 1x1 RGBA8 PNG, pixel (0x12, 0x34, 0x56, 0x78). */
static const unsigned char k_png[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
    0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xDA, 0x63, 0x10, 0x32, 0x09, 0xAB, 0x00, 0x00, 0x02, 0x0D, 0x01, 0x15, 0x76, 0x20,
    0xB7, 0xD6, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

TEST(de_tp_smoke_stb, decode_1x1_png) {
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(k_png, (int)sizeof k_png, &w, &h, &comp, 4);
    ASSERT_TRUE(px != NULL);
    EXPECT_EQ(w, 1);
    EXPECT_EQ(h, 1);
    EXPECT_EQ(comp, 4);
    EXPECT_EQ(px[0], 0x12);
    EXPECT_EQ(px[1], 0x34);
    EXPECT_EQ(px[2], 0x56);
    EXPECT_EQ(px[3], 0x78);
    stbi_image_free(px);
}

TEST(de_tp_smoke_stb, vorbis_rejects_non_ogg) {
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(k_png, (int)sizeof k_png, &err, NULL);
    if (v)
        stb_vorbis_close(v);
    EXPECT_TRUE(v == NULL);
    EXPECT_NE(err, (int)VORBIS__no_error);
}

/* stb_image_write (DENG-327): a 3x2 RGBA8 PNG written to memory decodes to the same bytes. */
typedef struct png_sink {
    unsigned char data[1024];
    int size;
    int overflow;
} png_sink;

static void png_write(void *ctx, void *data, int size) {
    png_sink *s = (png_sink *)ctx;
    if (size < 0 || s->size + size > (int)sizeof s->data) {
        s->overflow = 1;
        return;
    }
    memcpy(s->data + s->size, data, (size_t)size);
    s->size += size;
}

TEST(de_tp_smoke_stb, write_png_roundtrip) {
    unsigned char px[3 * 2 * 4];
    for (int i = 0; i < (int)sizeof px; ++i)
        px[i] = (unsigned char)(i * 11 + 3);
    static png_sink sink;
    memset(&sink, 0, sizeof sink);
    ASSERT_NE(stbi_write_png_to_func(png_write, &sink, 3, 2, 4, px, 3 * 4), 0);
    ASSERT_EQ(sink.overflow, 0);
    int w = 0, h = 0, comp = 0;
    unsigned char *back = stbi_load_from_memory(sink.data, sink.size, &w, &h, &comp, 4);
    ASSERT_TRUE(back != NULL);
    EXPECT_EQ(w, 3);
    EXPECT_EQ(h, 2);
    EXPECT_MEMEQ(back, px, sizeof px);
    stbi_image_free(back);
}
