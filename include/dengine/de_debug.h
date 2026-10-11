/* de_debug.h — profiling (Tracy), cvars, console, stats overlay, crash handling. SPEC.md §5.8, §18.
 *
 * Sections and their owners: profiling (S06 DENG-94), console variables (S02 DENG-154), console
 * commands and stats overlay (S06 DENG-96), crash handling (S03 DENG-251). Each issue edits only
 * its own section. */
#ifndef DE_DEBUG_H
#define DE_DEBUG_H
#include "de_base.h"
DE_EXTERN_C_BEGIN

/* ---------- Profiling (S06 DENG-94): compiled out unless DE_PROFILE is defined (Dev/Release) */
typedef struct de_prof_zone {
    u64 opaque[2];
} de_prof_zone;
typedef struct de_prof_srcloc {
    const char *name, *function, *file;
    u32 line, color;
} de_prof_srcloc;
DE_API de_prof_zone de_prof_zone_begin_(const de_prof_srcloc *loc);
DE_API void de_prof_zone_end_(de_prof_zone z);
DE_API void de_prof_frame_mark_(const char *name);
DE_API void de_prof_plot_(const char *name, f64 value);
DE_API void de_prof_alloc_(const void *ptr, size_t size, const char *pool);
DE_API void de_prof_free_(const void *ptr, const char *pool);

#ifdef DE_PROFILE
#define DE_PROFILE_ZONE_BEGIN(var, name)                                                           \
    static const de_prof_srcloc var##_loc = {(name), __func__, __FILE__, (u32)__LINE__, 0};        \
    de_prof_zone var = de_prof_zone_begin_(&var##_loc)
#define DE_PROFILE_ZONE_END(var) de_prof_zone_end_(var)
#define DE_PROFILE_FRAME(name)   de_prof_frame_mark_(name)
#define DE_PROFILE_PLOT(name, v) de_prof_plot_((name), (f64)(v))
#else
#define DE_PROFILE_ZONE_BEGIN(var, name) ((void)0)
#define DE_PROFILE_ZONE_END(var)         ((void)0)
#define DE_PROFILE_FRAME(name)           ((void)0)
#define DE_PROFILE_PLOT(name, v)         ((void)0)
#endif

/* ---------- Console variables (S02 DENG-154) ----------
 *
 * A cvar is a typed, named value that any module registers (SPEC §5.8). It can be set from the
 * console (de_cvar_set), the command line (de_cvar_apply_args) and config.toml (de_cvar_load), and
 * de_cvar_save writes the DE_CVAR_SAVED ones back.
 *
 * Names are lowercase and dotted: 1 to DE_CVAR_NAME_MAX_PARTS segments of [a-z0-9_], separated
 * by single dots ("r.shadows.quality", "log.level.render"), at most DE_CVAR_NAME_MAX bytes. In
 * config.toml the dots become nested tables: `r.shadows.quality = 2` is `[r.shadows]` +
 * `quality = 2`. So a name cannot also be a table of other cvars: with "r.shadows.quality"
 * registered, "r.shadows" cannot be (and the other way round).
 *
 * The registry is process-wide and thread-safe. It is created on first use. Registration, set,
 * load, save and find take its mutex; the typed getters (de_cvar_bool, _int, _float, _string)
 * never lock: each reads one atomically published value, so they are cheap enough for per-frame
 * code. Cvars are never unregistered.
 *
 * Values that a source sets for a name nobody has registered yet (command line, config.toml) are
 * kept as pending text and applied when that name is registered, so the order of module startup
 * and argument parsing does not matter.
 */

/** Longest cvar name, in bytes without the NUL. */
#define DE_CVAR_NAME_MAX 127

/** Most dot-separated segments in a cvar name (the TOML parser's limit for one dotted key). */
#define DE_CVAR_NAME_MAX_PARTS 10

/** Most cvars a process can register. */
#define DE_CVAR_MAX_COUNT 65535u

DE_HANDLE(de_cvar);

/** Value type of a cvar. */
typedef enum de_cvar_type {
    DE_CVAR_BOOL,   /**< text: 1/0, true/false, on/off, yes/no (any case) */
    DE_CVAR_INT,    /**< i64; text: decimal, 0x hex or 0b binary (de_str_to_i64) */
    DE_CVAR_FLOAT,  /**< f64; text: de_str_to_f64 syntax; NaN is refused */
    DE_CVAR_STRING, /**< UTF-8 text, copied */
    DE_CVAR_ENUM,   /**< one of `enum_values`; text: the exact name, or its index */
    DE_CVAR_TYPE_COUNT_
} de_cvar_type;

/** Flags of a cvar (a bit set in de_cvar_desc.flags). */
typedef enum de_cvar_flags {
    /** de_cvar_set refuses it (DE_ERR_UNSUPPORTED). The command line and config.toml may set it
     * until de_cvar_lock_startup(). */
    DE_CVAR_READONLY = 1u << 0,
    /** Not registered in Shipping builds: de_cvar_register returns an invalid handle (no error)
     * and de_cvar_find does not find it. */
    DE_CVAR_DEV_ONLY = 1u << 1,
    /** Written by de_cvar_save. Only these are. */
    DE_CVAR_SAVED = 1u << 2,
    /** Server → client replication (S32). The registry only stores the flag; see
     * de_cvar_foreach_flag. */
    DE_CVAR_REPLICATED = 1u << 3,
    /** Every source refuses to set it (DE_ERR_UNSUPPORTED) unless the built-in bool cvar
     * `cheats` is true. Debug and Dev builds always allow it. */
    DE_CVAR_CHEAT = 1u << 4,
} de_cvar_flags;

/** Called after a successful change of the value, on the thread that set it, without any
 * registry lock held (so it may get or set cvars). Not called when a set leaves the value as it
 * was, nor for the pending value applied during de_cvar_register. */
typedef void (*de_cvar_change_fn)(de_cvar cv, void *user);

/**
 * What to register. `(de_cvar_desc){.name = "x"}` is a valid bool cvar that defaults to false.
 *
 * Defaults: bool `default_number != 0`; int `default_number` (rounded towards zero, saturated);
 * float `default_number`; string `default_string` (NULL is ""); enum `default_string` (a name in
 * `enum_values`) or, when it is NULL, the index `default_number`.
 *
 * `min < max` makes int and float values clamp to [min, max] (the default too); otherwise they
 * are not clamped, so a zeroed desc means "no range". Other types ignore min and max.
 *
 * The registry copies `name`, `help` and the enum names; the desc need not outlive the call.
 */
typedef struct de_cvar_desc {
    const char *name; /**< "r.shadows.quality"; see the naming rules above */
    const char *help; /**< one line for the console and config.toml; may be NULL */
    de_cvar_type type;
    u32 flags; /**< de_cvar_flags */
    f64 default_number, min, max;
    const char *default_string;
    const char *const *enum_values; /**< DE_CVAR_ENUM: `enum_count` non-empty names */
    u32 enum_count;
    de_cvar_change_fn on_change; /**< may be NULL */
    void *user;                  /**< passed to on_change */
} de_cvar_desc;

/**
 * Registers a cvar and returns its handle. If a value for this name is pending (command line or
 * config.toml before registration), it is applied now as a startup value; if it is refused (it
 * does not parse, or a flag forbids it), the default stays, the error context says why and a
 * warning goes to the log (category "core").
 *
 * Registering a name again with the same type returns the existing handle and replaces its
 * `on_change` and `user` (a reloaded game library re-registers its cvars); the value, flags,
 * range and help stay. Another type is DE_ERR_ALREADY_EXISTS.
 *
 * @return the handle, or an invalid one (`id == 0`) with the error context set:
 *         DE_ERR_INVALID_ARGUMENT (NULL desc, bad name, bad type, bad enum values or default),
 *         DE_ERR_ALREADY_EXISTS (name taken by another type, or a table/value conflict),
 *         DE_ERR_OUT_OF_MEMORY. A DE_CVAR_DEV_ONLY cvar in a Shipping build gives an invalid
 *         handle without an error.
 */
DE_API de_cvar de_cvar_register(const de_cvar_desc *desc);

/** The cvar called `name`, or an invalid handle. */
DE_API de_cvar de_cvar_find(const char *name);

/*
 * Typed getters. They never lock and never fail: an invalid or stale handle gives false, 0, 0.0
 * or NULL. Other types convert: bool and enum read as 0/1 and the enum index; a float read as an
 * int is rounded towards zero and saturated; de_cvar_string of an enum is the value's name, and
 * NULL for bool, int and float. A string from de_cvar_string stays valid for the life of the
 * process (old values are kept, not freed), so it may be used while another thread sets the cvar.
 */
DE_API bool de_cvar_bool(de_cvar cv);
DE_API i64 de_cvar_int(de_cvar cv);
DE_API f64 de_cvar_float(de_cvar cv);
DE_API const char *de_cvar_string(de_cvar cv);

/**
 * Sets the value from text, as the console does: parses it for the cvar's type (leading and
 * trailing whitespace is ignored except for strings), clamps numbers, checks the flags, then
 * calls `on_change` if the value changed.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (invalid handle, NULL value, NaN); DE_ERR_PARSE (text
 *         that is not a value of the type, or not one of the enum names; the message names the
 *         cvar); DE_ERR_UNSUPPORTED (DE_CVAR_READONLY, or DE_CVAR_CHEAT without `cheats`);
 *         DE_ERR_OUT_OF_MEMORY. On failure the value is unchanged.
 */
DE_API de_result de_cvar_set(de_cvar cv, const char *value);

/**
 * Applies `+name value` pairs from the command line. argv[0] (the program) is skipped, and so is
 * every argument that is not a `+name` with a value after it. Values are startup values: they may
 * set DE_CVAR_READONLY cvars until de_cvar_lock_startup(). Names nobody registered yet are kept
 * pending and applied at registration.
 *
 * @return DE_OK, or the first failure (bad name, `+name` without a value, a value that does not
 *         parse, a flag that refuses it). The other pairs are still applied.
 */
DE_API de_result de_cvar_apply_args(int argc, char **argv);

/**
 * Loads config.toml: every value becomes a startup set of the cvar named by its dotted key
 * path. Keys nobody registered yet are kept pending.
 *
 * Until the S03 virtual file system exists (DENG-247), `vpath` is a native path opened with
 * stdio.
 *
 * @return DE_OK; DE_ERR_IO (the file cannot be read); DE_ERR_PARSE (not valid TOML, with the
 *         line in the message; nothing is applied); or the first per-key failure (a bad name, an
 *         array or date value, a value that does not parse, a flag that refuses it), after the
 *         other keys were applied.
 */
DE_API de_result de_cvar_load(const char *vpath);

/** Like de_cvar_load, from `len` bytes of TOML text in memory (tests, fuzzing, embedded
 * defaults). `text` need not be NUL-terminated. Documents longer than INT_MAX bytes fail. */
DE_API de_result de_cvar_load_from_memory(const char *text, size_t len);

/**
 * Writes every DE_CVAR_SAVED cvar to `vpath` as TOML, in registration order: top-level names
 * first, then one `[table]` per dotted prefix, in the order its first cvar was registered. Each
 * value is preceded by its help text as a comment. The output depends only on the registered
 * cvars and their values, so saving twice gives the same bytes, and loading the file and saving
 * again does too. Pending values and cvars without DE_CVAR_SAVED are not written.
 *
 * Until S03 (DENG-247), `vpath` is a native path written with stdio.
 *
 * @return DE_OK, DE_ERR_IO, DE_ERR_INVALID_ARGUMENT (NULL path) or DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_cvar_save(const char *vpath);

/**
 * Ends startup: from now on the command line and config.toml cannot set DE_CVAR_READONLY cvars
 * either (DE_ERR_UNSUPPORTED). The app host calls it after it has applied the command line and
 * the config (S06). It cannot be undone.
 */
DE_API void de_cvar_lock_startup(void);

/** Visitor for de_cvar_foreach_flag. */
typedef void (*de_cvar_visit_fn)(de_cvar cv, void *user);

/**
 * Calls `fn` for every registered cvar whose flags contain all bits of `flags` (0: every cvar),
 * in registration order. No lock is held during the calls, so `fn` may get, set or register
 * cvars; cvars registered during the walk are not visited. S32 uses it with DE_CVAR_REPLICATED.
 */
DE_API void de_cvar_foreach_flag(u32 flags, de_cvar_visit_fn fn, void *user);

/** The cvar's name, help text ("" when none), type and flags. NULL / "" / 0 for an invalid
 * handle. The strings stay valid for the life of the process. Never lock. */
DE_API const char *de_cvar_name(de_cvar cv);
DE_API const char *de_cvar_help(de_cvar cv);
DE_API de_cvar_type de_cvar_get_type(de_cvar cv);
DE_API u32 de_cvar_get_flags(de_cvar cv);

/* ---------- Console commands (S06 DENG-96) ---------- */
typedef void (*de_command_fn)(int argc, const char *const *argv, void *user);
DE_API void de_command_register(const char *name, const char *help, de_command_fn fn, void *user);
DE_API de_result de_command_exec(const char *line);

/* ---------- Stats overlay (S06 DENG-96) ---------- */
typedef void (*de_stats_panel_fn)(void *user); /* draws with de_dbg_* */
DE_API void de_stats_register_panel(const char *name, de_stats_panel_fn fn, void *user);
DE_API void de_stats_overlay_toggle(void);

/* ---------- Crash handling (S03 DENG-251) ---------- */
typedef struct de_crash_report {
    const char *dump_path; /* minidump or equivalent */
    const char *log_path;
    const char *build_id;
    const char *gpu_breadcrumbs; /* last render-graph passes, if a GPU fault */
    bool gpu_fault;
} de_crash_report;
typedef void (*de_crash_upload_fn)(const de_crash_report *report, void *user); /* game-provided */

DE_API void de_crash_install(const char *dump_dir, de_crash_upload_fn upload, void *user);
DE_API void de_crash_set_annotation(const char *key, const char *value); /* included in reports */

DE_EXTERN_C_END
#endif
