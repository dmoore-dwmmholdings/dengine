/* DENG-805: guard for DE_MPACK_DEBUG (cmake/DeOptions.cmake).
 *
 * mpack enables MPACK_DEBUG (and with it read/write tracking, which breaks on a reader or writer
 * destroyed with a map or array still open) only when DEBUG or _DEBUG is defined. MSVC Debug
 * defines _DEBUG and Clang/GCC do not, so an unclosed map in the editor service failed 16 editor_
 * tests on Windows Debug and passed on macOS. This test fails if a build that should track does
 * not, so that the class of bug is caught on every host.
 *
 * DE_TEST_MPACK_TRACKING_EXPECTED is set by tests/unit/host/CMakeLists.txt from DE_MPACK_DEBUG and
 * the config; it is not defined when DE_MPACK_DEBUG=OFF (then nothing is asserted). */
#include "de_editor_protocol.h"
#include "de_test.h"

TEST(editor_mpack_config, tracking_matches_de_mpack_debug) {
#if !defined(DE_TEST_MPACK_TRACKING_EXPECTED)
    SKIP("DE_MPACK_DEBUG=OFF: mpack's own DEBUG/_DEBUG detection decides");
#elif DE_TEST_MPACK_TRACKING_EXPECTED
    EXPECT_EQ(MPACK_DEBUG, 1);
    EXPECT_EQ(MPACK_READ_TRACKING, 1);
    EXPECT_EQ(MPACK_WRITE_TRACKING, 1);
#else
    /* Not forced here. MSVC Debug still tracks through _DEBUG; nothing else should. */
#    if !defined(_DEBUG) && !defined(DEBUG)
    EXPECT_EQ(MPACK_DEBUG, 0);
#    endif
#endif
}

/* The balanced pattern every editor reader uses: a map read to its end and closed before destroy
 * returns mpack_ok with tracking on (and off). */
TEST(editor_mpack_config, a_closed_map_destroys_cleanly) {
    static const char map[] = "\x82\xa1"
                              "a"
                              "\x01\xa1"
                              "b"
                              "\x81\xa1"
                              "c"
                              "\x02";
    mpack_reader_t r;
    mpack_reader_init_data(&r, map, sizeof map - 1);
    const uint32_t n = mpack_expect_map(&r);
    ASSERT_EQ(n, 2u);
    for (uint32_t i = 0; i < n; ++i) {
        mpack_discard(&r); /* key */
        mpack_discard(&r); /* value, nested map included */
    }
    mpack_done_map(&r);
    EXPECT_EQ(mpack_reader_destroy(&r), mpack_ok);
}
