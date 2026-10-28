/* command_internal.h: internals of the console command registry (SPEC §18.6, DENG-96). Not a
 * public header; include as "core/command_internal.h". The public API is the console commands
 * section of include/dengine/de_debug.h.
 *
 * Users: src/services/crash (the cvar snapshot refreshes after every top-level
 * de_command_exec, through the exec hook below) and tests/unit/core/test_command.c. */
#ifndef DE_CORE_COMMAND_INTERNAL_H
#define DE_CORE_COMMAND_INTERNAL_H

#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** Most nested `exec` files (an `exec` inside an executed file, ...). Deeper fails. */
#define DE__COMMAND_EXEC_DEPTH_MAX 8

/**
 * Called on the executing thread after every top-level de_command_exec (not for the lines of an
 * `exec` file, only once for the whole file), whatever the result, with no lock held. Core cannot
 * call up into services, so services/crash installs this to refresh its cvar snapshot.
 */
typedef void (*de__command_exec_hook_fn)(void *user);

/** Sets the one exec hook (NULL removes it). Thread-safe; the hook in place when an exec ends is
 * the one called. */
void de__command_set_exec_hook(de__command_exec_hook_fn fn, void *user);

/**
 * Splits `line` into words, in place (see the de_debug.h console section for the rules):
 * quotes and escapes are removed by moving bytes within `line`, each word is NUL-terminated, and
 * `argv[0..*argc-1]` point into `line`. At most `max_args` words.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (an unclosed quote, or more than `max_args` words; the
 *         message says which).
 */
de_result de__command_tokenize(char *line, int max_args, char **argv, int *argc);

/** The name of command `index` (registration order) into `out`; false past the end (the editor
 * service's console completion, DENG-630). */
bool de__command_name_at(u32 index, char *out, size_t cap);

/** Number of registered commands (built-ins included). */
u32 de__command_count(void);

/** Tests only: removes every command and the exec hook; the built-ins are registered again on
 * the next use. No other thread may use the registry during the call. */
void de__command_reset_for_tests(void);

DE_EXTERN_C_END
#endif /* DE_CORE_COMMAND_INTERNAL_H */
