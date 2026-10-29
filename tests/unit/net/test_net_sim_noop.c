#include <dengine/de_net.h>
#if defined(DE_DEV) || defined(DE_DEBUG)
#error "The non-development simulator test must not compile the decorator"
#endif
#if EXPECT_SHIPPING && !defined(DE_SHIPPING)
#error "Shipping test is missing DE_SHIPPING"
#endif
#if !EXPECT_SHIPPING && defined(DE_SHIPPING)
#error "Release test must not use DE_SHIPPING"
#endif
int main(void) {
    /* Neither pointer may be read: non-development simulation is a genuine no-op. */
    de_net_set_simulation((de_net_session *)(uintptr_t)1, (const de_net_sim *)(uintptr_t)1);
    return 0;
}
