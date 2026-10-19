#include "de_test.h"
#include "text/loc/loc_internal.h"
#include <string.h>
TEST(loc_pseudo, accents_expansion_preserve_placeholders_and_markup) {
    const char *source = "Hello {name} <b>world</b> [color=red] text";
    char out[256];
    u32 n = de__loc_pseudo_text(source, strlen(source), out, sizeof out);
    EXPECT_TRUE(strncmp(out, "[!! ", 4) == 0);
    EXPECT_TRUE(strstr(out, "{name}") != NULL);
    EXPECT_TRUE(strstr(out, "<b>") != NULL);
    EXPECT_TRUE(strstr(out, "[color=red]") != NULL);
    EXPECT_TRUE(strstr(out, "Ħëľľö") != NULL);
    EXPECT_TRUE(strstr(out, "~~~~") != NULL);
    EXPECT_TRUE(n > strlen(source));
    char tiny[7];
    de__loc_pseudo_text("é", 2, tiny, sizeof tiny);
    EXPECT_TRUE(tiny[sizeof tiny - 1] == 0);
}
