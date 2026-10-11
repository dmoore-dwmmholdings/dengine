/* Placeholder: keeps de_water a non-empty static library until src/world/water/
 * has real code.
 * The extern anchor keeps the static function referenced (-Wunused-function, MSVC C4505) and
 * gives the archive a symbol (Apple ranlib warns about members with no symbols). */
static int de__water_stub(void) {
    return 0;
}

int de__water_stub_anchor(void);
int de__water_stub_anchor(void) {
    return de__water_stub();
}
