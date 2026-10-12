/* Unit tests: without DE_PROFILE every DE_PROFILE_* macro is ((void)0) and does not evaluate its
 * arguments (DENG-94). This file undefines DE_PROFILE before any engine header, so it checks the
 * Shipping form of the macros in every build type. It includes nothing that depends on the
 * define for its layout (de_mutex does). */
#undef DE_PROFILE

#include <dengine/de_debug.h>

#include "de_test.h"

static int g_evaluated;

static int side_effect(void) {
    return ++g_evaluated;
}

TEST(prof_off, macros_compile_out) {
    g_evaluated = 0;
    DE_PROFILE_ZONE_BEGIN(zone, "prof_off.zone");
    DE_PROFILE_ZONE_BEGIN_C(colored, "prof_off.colored", side_effect());
    DE_PROFILE_PLOT("prof_off.plot", side_effect());
    DE_PROFILE_FRAME((side_effect(), "prof_off.frame"));
    DE_PROFILE_ALLOC(&g_evaluated, side_effect(), "prof_off.pool");
    DE_PROFILE_FREE(&g_evaluated, (side_effect(), "prof_off.pool"));
    DE_PROFILE_ZONE_END(colored);
    DE_PROFILE_ZONE_END(zone);
    /* Usable as expressions, like the real calls (all are void). */
    if (g_evaluated == 0)
        DE_PROFILE_FRAME(NULL);
    else
        DE_PROFILE_PLOT("prof_off.never", 0);
    EXPECT_EQ(g_evaluated, 0);
    EXPECT_EQ(side_effect(), 1); /* the counter itself works */
}
