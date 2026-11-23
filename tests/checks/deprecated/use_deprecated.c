/* Fixture for check_deprecated_macro.cmake (DENG-767): calls a function marked DE_DEPRECATED. */
#ifdef DE_TEST_FALLBACK
/* Pose as an unknown compiler. System headers go in first, so they still see GNUC. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#undef __GNUC__
#undef __clang__
#endif
#include "dengine/de_base.h"

DE_DEPRECATED("de_fixture_old is deprecated: use de_fixture_new") int de_fixture_old(void);

int de_fixture_old(void) { return 1; }

int de_fixture_call(void) { return de_fixture_old(); }
