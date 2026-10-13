/* Smoke test for de_tp_basisu: the transcoder links, starts with KTX2 and zstd support, and
 * refuses files that are not KTX2 or are cut short. Transcoding real files is tested by the
 * texture importer (tests/unit/import/, import.texture.*). */
#include <stddef.h>
#include <string.h>

#include "de_test.h"

int de_smoke_basisu_start(void);
int de_smoke_basisu_ktx2_init(const void *data, size_t size);

TEST(de_tp_smoke_basisu, start) {
    EXPECT_EQ(de_smoke_basisu_start(), 1);
}

TEST(de_tp_smoke_basisu, rejects_bad_files) {
    ASSERT_EQ(de_smoke_basisu_start(), 1);
    static const unsigned char junk[96] = "definitely not a KTX2 file";
    EXPECT_EQ(de_smoke_basisu_ktx2_init(junk, sizeof junk), 0);
    /* The KTX2 identifier alone, without the rest of the header. */
    static const unsigned char ident[12] = {0xAB, 'K', 'T', 'X', ' ', '2',
                                            '0',  0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    EXPECT_EQ(de_smoke_basisu_ktx2_init(ident, sizeof ident), 0);
}
