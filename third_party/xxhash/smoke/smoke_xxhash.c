/* Smoke test for de_tp_xxhash: known vectors through the linked library. */
#include <string.h>

#include <xxhash.h>

#include "de_test.h"

/* Reference values from the xxHash test suite (tests/sanity_test_vectors.h). */
TEST(de_tp_smoke_xxhash, empty_input_vectors) {
    EXPECT_EQ(XXH3_64bits("", 0), 0x2D06800538D394C2ULL);
    EXPECT_EQ(XXH64("", 0, 0), 0xEF46DB3751D8E999ULL);
    EXPECT_EQ(XXH32("", 0, 0), 0x02CC5D05U);
}

TEST(de_tp_smoke_xxhash, streaming_matches_one_shot) {
    static const char msg[] = "DEngine vendored xxHash smoke test";
    const size_t n = strlen(msg);
    XXH3_state_t *st = XXH3_createState();
    ASSERT_TRUE(st != NULL);
    ASSERT_EQ(XXH3_64bits_reset(st), XXH_OK);
    ASSERT_EQ(XXH3_64bits_update(st, msg, 10), XXH_OK);
    ASSERT_EQ(XXH3_64bits_update(st, msg + 10, n - 10), XXH_OK);
    const XXH64_hash_t streamed = XXH3_64bits_digest(st);
    XXH3_freeState(st);
    EXPECT_EQ(streamed, XXH3_64bits(msg, n));
}
