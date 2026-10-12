/*
 * cooker_link_probe.c: an executable that links the aggregate `dengine` library the way a game
 * does (DENG-209). It is built, never run by the tests: cooker_link_check (check_cooker_link.cmake)
 * reads its symbols. Creating an asset system installs the load pipeline, whose submit path holds
 * the cook stage, so in Debug and Dev the linker must pull the cooker in, and in Release and
 * Shipping there must be nothing to pull.
 */
#include <dengine/de_asset.h>
#include <dengine/de_base.h>

int main(void) {
    de_asset_system *s = NULL;
    if (de_asset_system_create(&(de_asset_system_desc){0}, &s) != DE_OK)
        return 1;
    de_asset_system_destroy(s);
    return 0;
}
