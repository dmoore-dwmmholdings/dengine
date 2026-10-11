/* Placeholder: keeps de_sky a non-empty static library until src/world/sky/
 * has real code.
 * The extern anchor keeps the static function referenced (-Wunused-function, MSVC C4505) and
 * gives the archive a symbol (Apple ranlib warns about members with no symbols). */
static int de__sky_stub(void) {
    return 0;
}

int de__sky_stub_anchor(void);
int de__sky_stub_anchor(void) {
    return de__sky_stub();
}
