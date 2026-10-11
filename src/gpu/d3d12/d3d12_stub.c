/* Placeholder: keeps de_gpu_d3d12 a non-empty static library until src/gpu/d3d12/
 * has real code.
 * The extern anchor keeps the static function referenced (-Wunused-function, MSVC C4505) and
 * gives the archive a symbol (Apple ranlib warns about members with no symbols). */
static int de__gpu_d3d12_stub(void) {
    return 0;
}

int de__gpu_d3d12_stub_anchor(void);
int de__gpu_d3d12_stub_anchor(void) {
    return de__gpu_d3d12_stub();
}
