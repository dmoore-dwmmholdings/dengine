#include "de_test.h"
#include "text/loc/loc_internal.h"
#include "plural_samples_gen.h"
#include <stdio.h>
TEST(loc_plural, authoritative_cldr_samples_en_fr_ru_pl_ar_ja) {
    for (u32 i = 0; i < sizeof cldr_samples / sizeof cldr_samples[0]; ++i) {
        de__plural actual = de__loc_plural(cldr_samples[i].locale, cldr_samples[i].number);
        if (actual != cldr_samples[i].category) {
            printf("plural %s %s expected %s actual %s", cldr_samples[i].locale,
                   cldr_samples[i].number, de__loc_plural_name(cldr_samples[i].category),
                   de__loc_plural_name(actual));
            EXPECT_EQ(actual, cldr_samples[i].category);
        }
    }
}
TEST(loc_plural, regional_parent_and_visible_fraction_operands) {
    EXPECT_EQ(de__loc_plural("en-US", "1"), DE__PLURAL_ONE);
    EXPECT_EQ(de__loc_plural("en", "1.0"), DE__PLURAL_OTHER);
    EXPECT_EQ(de__loc_plural("ru", "21"), DE__PLURAL_ONE);
    EXPECT_EQ(de__loc_plural("ru", "22"), DE__PLURAL_FEW);
    EXPECT_EQ(de__loc_plural("ru", "25"), DE__PLURAL_MANY);
    EXPECT_EQ(de__loc_plural("ja", "1"), DE__PLURAL_OTHER);
    EXPECT_EQ(de__loc_plural("ar", "0"), DE__PLURAL_ZERO);
    EXPECT_EQ(de__loc_plural("ar", "2"), DE__PLURAL_TWO);
}
