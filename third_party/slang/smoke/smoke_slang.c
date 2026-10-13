/* Smoke test for the prebuilt Slang compiler (DENG-289): the library extracted for this host
 * opens with de_dylib_open, exports the functions the shader importer resolves, and its build tag
 * is the pinned version (third_party/vendor.toml). slang.h is C++, so this C test names the two
 * functions it calls with their C signatures. */
#include <dengine/de_base.h>
#include <dengine/de_platform.h>

#include "de_test.h"

#include <string.h>

typedef const char *(*build_tag_fn)(void);

TEST(de_tp_smoke_slang, library_opens) {
    de_dylib *lib = NULL;
    ASSERT_EQ(de_dylib_open(DE_SLANG_LIBRARY, &lib), DE_OK);
    ASSERT_TRUE(lib != NULL);
    EXPECT_TRUE(de_dylib_function(lib, "slang_createGlobalSession2") != NULL);
    EXPECT_TRUE(de_dylib_function(lib, "spReflectionEntryPoint_getComputeThreadGroupSize") != NULL);
    EXPECT_TRUE(de_dylib_function(lib, "spReflectionTypeLayout_GetSize") != NULL);
    build_tag_fn tag = (build_tag_fn)de_dylib_function(lib, "spGetBuildTagString");
    ASSERT_TRUE(tag != NULL);
    const char *v = tag();
    ASSERT_TRUE(v != NULL);
    EXPECT_TRUE(strstr(v, "2026.19") != NULL);
    de_dylib_close(lib);
}
