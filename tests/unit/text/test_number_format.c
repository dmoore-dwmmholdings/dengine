#include "de_test.h"
#include "text/loc/loc_internal.h"
TEST(loc_number, cldr_decimal_grouping_en_de_fr_and_indian_secondary_group) {
    char out[256];
    de__loc_number("en", 1234567.891, NULL, out, sizeof out);
    EXPECT_STREQ(out, "1,234,567.891");
    de__loc_number("de", 1234567.891, NULL, out, sizeof out);
    EXPECT_STREQ(out, "1.234.567,891");
    de__loc_number("fr", 1234567.891, NULL, out, sizeof out);
    EXPECT_STREQ(out, "1 234 567,891");
    de__loc_number("hi", 1234567.891, NULL, out, sizeof out);
    EXPECT_STREQ(out, "12,34,567.891");
}
TEST(loc_number, all_specified_locales_percent_and_basic_utc_dates) {
    const char *locales[] = {"en", "de", "fr", "es", "it",      "pt",     "pt-BR", "nl",
                             "sv", "pl", "cs", "ru", "uk",      "tr",     "ar",    "he",
                             "hi", "th", "ja", "ko", "zh-Hans", "zh-Hant"};
    for (u32 i = 0; i < sizeof locales / sizeof locales[0]; ++i) {
        char out[256];
        const de__loc_number_data *d = de__loc_number_data_get(locales[i]);
        EXPECT_STREQ(d->locale, locales[i]);
        EXPECT_TRUE(de__loc_number(locales[i], .5, "percent", out, sizeof out) > 0);
        EXPECT_TRUE(de__loc_date(locales[i], 0, "medium", out, sizeof out) > 0);
    }
    char out[256];
    de__loc_date("en", 0, "short", out, sizeof out);
    EXPECT_STREQ(out, "1/1/70");
    de__loc_date("en", 0, "medium", out, sizeof out);
    EXPECT_STREQ(out, "Jan 1, 1970");
    de_loc_arg arg = {"date", DE_LOC_ARG_INT, NULL, 0, 0};
    u32 n = 0;
    ASSERT_EQ(de__loc_message("{date, date, medium}", 20, "en", &arg, 1, out, sizeof out, &n),
              DE_OK);
    EXPECT_STREQ(out, "Jan 1, 1970");
}
