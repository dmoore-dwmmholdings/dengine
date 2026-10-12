/* smoke_tracy.c: <tracy/TracyC.h> compiles as C11 under the engine's warning flags, and the
 * client links. The profiler is not started here (that opens a socket); test_prof_tracy.c in
 * tests/unit/core runs it through de_prof_init. */
#include <tracy/TracyC.h>

#include "de_test.h"

TEST(tracy, header_compiles_and_client_links) {
    /* TRACY_MANUAL_LIFETIME: nothing runs until ___tracy_startup_profiler. */
    EXPECT_EQ(___tracy_profiler_started(), 0);
    static const struct ___tracy_source_location_data loc = {"smoke", __func__, __FILE__, 1u, 0u};
    EXPECT_TRUE(loc.line == 1u);
    EXPECT_TRUE(sizeof(TracyCZoneCtx) <= 16u);
}
