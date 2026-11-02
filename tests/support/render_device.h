#ifndef DE_TEST_RENDER_DEVICE_H
#define DE_TEST_RENDER_DEVICE_H

#include "de_test.h"
#include <dengine/de_gpu.h>

/* Only device availability is classified here. Renderer initialization on a Base-capable
 * device must still satisfy the caller's original success assertion. */
static inline void de_test_require_base_device(de_result result, const de_gpu_device *device) {
    if (result == DE_ERR_UNSUPPORTED)
        SKIP("renderer fixture GPU runtime unavailable");
    ASSERT_EQ(result, DE_OK);
    ASSERT_TRUE(device != NULL);
    de_gpu_tier_report tier;
    de_gpu_evaluate_tier(de_gpu_get_caps(device), &tier);
    if (!tier.base_ok)
        SKIP("renderer fixture requires Base-tier device capabilities");
}

#endif
