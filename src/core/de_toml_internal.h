/*
 * de_toml_internal.h: the engine's one way into the vendored tomlc17 parser (internal, not
 * installed). Include as "core/de_toml_internal.h"; the including module links de_tp_tomlc17.
 *
 * tomlc17 takes its allocator through process-global hooks (toml_set_option) without a user
 * pointer. src/core/cvar_toml.c installs them once, and every block they hand out records the
 * de_allocator it came from (Trama page "Vendoring", section "tomlc17 allocator hooks"). A module
 * that parses TOML calls these two functions and never calls toml_parse, toml_free or
 * toml_set_option itself; another toml_set_option would replace the hooks for the whole process.
 *
 * Thread-safe: parses on different threads may run at once (the allocator choice is per thread).
 */
#ifndef DE_CORE_TOML_INTERNAL_H
#define DE_CORE_TOML_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <tomlc17.h>

/**
 * toml_parse(src, len) with every block taken from `a` (NULL = de_heap_allocator()). `src` must
 * hold `len` bytes (it need not be NUL-terminated). The result, also a failed one, must be
 * released with de__toml_free(). UTF-8 is checked.
 */
toml_result_t de__toml_parse(const de_allocator *a, const char *src, int len);

/** Opt-in per-parse duplicate assignment handler; ordinary parsing stays strict. */
toml_result_t de__toml_parse_with_duplicates(const de_allocator *a, const char *src, int len,
                                             toml_duplicate_fn handler, void *user);

/** Releases a de__toml_parse() result. Each block goes back to the allocator it came from. */
void de__toml_free(toml_result_t res);

#endif /* DE_CORE_TOML_INTERNAL_H */
