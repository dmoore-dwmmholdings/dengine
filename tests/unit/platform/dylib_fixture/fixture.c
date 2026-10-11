/* fixture.c: the shared library that test_dylib.c loads (DENG-249). Plain C, no engine code:
 * it stands in for a game library. */
#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

/** Exported data: the test reads it through de_dylib_symbol(). */
FIXTURE_EXPORT int fixture_value = 42;

/** The function the acceptance criteria name. */
FIXTURE_EXPORT int fixture_add(int a, int b);
FIXTURE_EXPORT int fixture_add(int a, int b) {
    return a + b;
}

/* Per-image state: two loaded copies of this library count separately. */
static int fixture_count;

/** Increments this image's counter and returns the new value. */
FIXTURE_EXPORT int fixture_counter(void);
FIXTURE_EXPORT int fixture_counter(void) {
    return ++fixture_count;
}
