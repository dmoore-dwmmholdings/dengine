/* base.c: result names and the engine version (SPEC §5.2). */
#include <dengine/de_base.h>

/* Designated initialisers keep each name next to its value; the size check catches a value
 * appended to de_result without a name here. A value skipped in the middle would leave a NULL,
 * which test base.result_str_all_values catches. */
static const char *const de__result_names[] = {
    [DE_OK] = "DE_OK",
    [DE_ERR_INVALID_ARGUMENT] = "DE_ERR_INVALID_ARGUMENT",
    [DE_ERR_OUT_OF_MEMORY] = "DE_ERR_OUT_OF_MEMORY",
    [DE_ERR_IO] = "DE_ERR_IO",
    [DE_ERR_NOT_FOUND] = "DE_ERR_NOT_FOUND",
    [DE_ERR_ALREADY_EXISTS] = "DE_ERR_ALREADY_EXISTS",
    [DE_ERR_UNSUPPORTED] = "DE_ERR_UNSUPPORTED",
    [DE_ERR_PARSE] = "DE_ERR_PARSE",
    [DE_ERR_VERSION_MISMATCH] = "DE_ERR_VERSION_MISMATCH",
    [DE_ERR_DEVICE_LOST] = "DE_ERR_DEVICE_LOST",
    [DE_ERR_TIMEOUT] = "DE_ERR_TIMEOUT",
    [DE_ERR_BUSY] = "DE_ERR_BUSY",
    [DE_ERR_CANCELLED] = "DE_ERR_CANCELLED",
    [DE_ERR_NETWORK] = "DE_ERR_NETWORK",
    [DE_ERR_INTERNAL] = "DE_ERR_INTERNAL",
};
_Static_assert(sizeof de__result_names / sizeof de__result_names[0] == DE_RESULT_COUNT_,
               "de__result_names must name every de_result value");

const char *de_result_str(de_result r) {
    /* Compare as unsigned: an out-of-range value may arrive through the ABI as any integer. */
    unsigned idx = (unsigned)r;
    if (idx >= (unsigned)DE_RESULT_COUNT_ || de__result_names[idx] == NULL)
        return "DE_ERR_UNKNOWN";
    return de__result_names[idx];
}

_Static_assert(DE_VERSION_MAJOR < 1024 && DE_VERSION_MINOR < 1024 && DE_VERSION_PATCH < 4096,
               "version fields exceed their bit widths (10.10.12)");

u32 de_version(void) {
    return DE_MAKE_VERSION(DE_VERSION_MAJOR, DE_VERSION_MINOR, DE_VERSION_PATCH);
}
