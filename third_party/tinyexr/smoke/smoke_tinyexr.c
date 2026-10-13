/* Smoke test for de_tp_tinyexr: <tinyexr.h> compiles as C11 with the engine's strict flags (its
 * declaration part is C), the implementation links, and the parser accepts an EXR version header
 * and refuses garbage and a truncated file. Decoding real files is tested by the texture importer
 * (tests/unit/import/, import.texture.*). */
#include <string.h>

#include <tinyexr.h>

#include "de_test.h"

/* The 8-byte EXR start: magic 76 2f 31 01, version 2, flags 0 (single-part scanline). */
static const unsigned char k_version[8] = {0x76, 0x2f, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};

TEST(de_tp_smoke_tinyexr, version_header) {
    EXRVersion v;
    memset(&v, 0, sizeof v);
    ASSERT_EQ(ParseEXRVersionFromMemory(&v, k_version, sizeof k_version), TINYEXR_SUCCESS);
    EXPECT_EQ(v.version, 2);
    EXPECT_EQ(v.tiled, 0);
    EXPECT_EQ(v.multipart, 0);
}

TEST(de_tp_smoke_tinyexr, rejects_garbage) {
    static const unsigned char junk[16] = "not an exr file";
    EXPECT_TRUE(IsEXRFromMemory(junk, sizeof junk) != TINYEXR_SUCCESS);
    float *rgba = NULL;
    int w = 0, h = 0;
    const char *err = NULL;
    EXPECT_TRUE(LoadEXRFromMemory(&rgba, &w, &h, k_version, sizeof k_version, &err) !=
                TINYEXR_SUCCESS);
    EXPECT_TRUE(rgba == NULL);
    if (err != NULL)
        FreeEXRErrorMessage(err);
}
