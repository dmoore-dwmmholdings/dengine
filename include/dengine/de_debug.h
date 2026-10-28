/* de_debug.h — profiling (Tracy), cvars, console, stats overlay, crash handling. SPEC.md §5.8, §18.
 *
 * Sections and their owners: profiling (S06 DENG-94), console variables (S02 DENG-154), console
 * commands and stats overlay (S06 DENG-96), crash handling (S03 DENG-251). Each issue edits only
 * its own section. */
#ifndef DE_DEBUG_H
#define DE_DEBUG_H
#include "de_base.h"
#include "de_math.h"
DE_EXTERN_C_BEGIN

/* ---------- Profiling (S06 DENG-94) ----------
 *
 * SPEC §18.1. CPU zones, frame marks, plots, memory events, lock events, thread names and Warn+
 * log messages, sent to the Tracy profiler (the vendored client, third_party/tracy/). Profiling is
 * compiled in when DE_PROFILE is defined: Dev builds, and Release builds unless the CMake option
 * DE_ENABLE_PROFILE is off. Shipping never defines it. Without DE_PROFILE every DE_PROFILE_* macro
 * is ((void)0), its arguments are not evaluated, and no profiler code or Tracy symbol is linked.
 * The CMake option DE_TRACY (default ON) picks the backend of DE_PROFILE builds: with it off they
 * keep the calls but have no backend (de_prof_init returns DE_ERR_UNSUPPORTED).
 *
 * Nothing is sent before de_prof_init(): until then, and after de_prof_shutdown(), every
 * de_prof_* call returns at once. de_prof_init() starts the Tracy client in on-demand mode: it
 * listens for the profiler (TCP port 8086, or the next free one) and records nothing until one
 * connects, so a Dev build that nobody profiles pays one atomic load and a branch per call.
 *
 * Lifetime rules. Call de_prof_init() first, before any engine thread is created (threads name
 * themselves in the profiler when they start, de_mutex records its lock context when it is
 * initialized, and memory events start with the hooks). Call de_prof_shutdown() last, after every
 * thread that profiles, logs or allocates through a reporting allocator has stopped (so after
 * de_log_shutdown). A zone must begin and end within one init/shutdown span.
 *
 * Names (zones, frames, plots, lock and memory pools) are kept by pointer: pass string literals
 * or strings that live as long as the process.
 */

/** An open zone (de_prof_zone_begin_). Holds the backend's zone context. */
typedef struct de_prof_zone {
    u64 opaque[2];
} de_prof_zone;

/** A source location: zone, lock or mark site. The backend keeps the pointer, so it must be
 * static (DE_PROFILE_ZONE_BEGIN makes it so). `color` is 0xRRGGBB, 0 = default. */
typedef struct de_prof_srcloc {
    const char *name, *function, *file;
    u32 line, color;
} de_prof_srcloc;

/**
 * A profiled lock's context, in the lock's own storage (de_mutex holds one in DE_PROFILE
 * builds). All zeros means "not profiled". Only meaningful between de_prof_init and
 * de_prof_shutdown: a context from an earlier span is ignored.
 */
typedef struct de_prof_lock {
    void *ctx;
    u32 epoch;
    u32 reserved_;
} de_prof_lock;

/**
 * Starts the profiler backend, installs the memory hooks (de_mem_set_hooks, chained to the hooks
 * installed before) and adds a log sink that sends Warn, Error and Fatal records as profiler
 * messages. Allowed before de_log_init(). Calling it again while it runs does nothing.
 *
 * @return DE_OK; DE_ERR_UNSUPPORTED when the build has no backend (no DE_PROFILE, or DE_TRACY
 *         off).
 */
DE_API de_result de_prof_init(void);

/** Removes the log sink and the memory hooks and stops the backend. See the lifetime rules
 * above. Does nothing if the profiler is not running. */
DE_API void de_prof_shutdown(void);

/** True between de_prof_init() and de_prof_shutdown(). */
DE_API bool de_prof_running(void);

/** True while a profiler is connected and receiving (on-demand mode records only then). */
DE_API bool de_prof_connected(void);

/* The functions below are what the DE_PROFILE_* macros and the engine's instrumented code call.
 * They are defined in DE_PROFILE builds only; call them under #ifdef DE_PROFILE. */
DE_API de_prof_zone de_prof_zone_begin_(const de_prof_srcloc *loc);
DE_API void de_prof_zone_end_(de_prof_zone z);
/** Frame mark. NULL is the main frame; another name is a secondary frame set. */
DE_API void de_prof_frame_mark_(const char *name);
DE_API void de_prof_plot_(const char *name, f64 value);
/** Memory events in the pool `pool` (NULL: the unnamed pool). The engine allocators report
 * through the memory hooks with the memory tag name as the pool; these are for other memory. */
DE_API void de_prof_alloc_(const void *ptr, size_t size, const char *pool);
DE_API void de_prof_free_(const void *ptr, const char *pool);
/** Names the calling thread in the profiler. de_thread_set_name calls it, so every engine
 * thread started after de_prof_init is named. The name is copied. */
DE_API void de_prof_thread_name_(const char *name);

/* Profiled locks (de_mutex uses these in DE_PROFILE builds). Order of calls around a lock:
 * `if (de_prof_lock_before_(l)) { lock; de_prof_lock_after_(l); } else { lock; }`, then
 * de_prof_lock_after_unlock_ after each unlock; for a try-lock, de_prof_lock_after_try_ after it.
 * Every function ignores a lock with no context of the current span. */
/** Gives `l` a lock context if the profiler runs (else zeroes it). `loc` must be static. */
DE_API void de_prof_lock_init_(de_prof_lock *l, const de_prof_srcloc *loc);
/** Releases the lock context of `l` and zeroes it. */
DE_API void de_prof_lock_destroy_(de_prof_lock *l);
/** Before a blocking lock. Returns true when de_prof_lock_after_ must follow the lock. */
DE_API bool de_prof_lock_before_(de_prof_lock *l);
DE_API void de_prof_lock_after_(de_prof_lock *l);
DE_API void de_prof_lock_after_try_(de_prof_lock *l, bool acquired);
DE_API void de_prof_lock_after_unlock_(de_prof_lock *l);

#ifdef DE_PROFILE
/** Opens zone `var` named `name` (a string literal); close it with DE_PROFILE_ZONE_END(var) in the
 * same scope, on every path. Zones nest per thread and must close in reverse order. */
#define DE_PROFILE_ZONE_BEGIN(var, name)                                                           \
    static const de_prof_srcloc var##_loc = {(name), __func__, __FILE__, (u32)__LINE__, 0};        \
    de_prof_zone var = de_prof_zone_begin_(&var##_loc)
/** Like DE_PROFILE_ZONE_BEGIN with a 0xRRGGBB colour. */
#define DE_PROFILE_ZONE_BEGIN_C(var, name, color)                                                  \
    static const de_prof_srcloc var##_loc = {(name), __func__, __FILE__, (u32)__LINE__,            \
                                             (u32)(color)};                                        \
    de_prof_zone var = de_prof_zone_begin_(&var##_loc)
#define DE_PROFILE_ZONE_END(var)          de_prof_zone_end_(var)
#define DE_PROFILE_FRAME(name)            de_prof_frame_mark_(name)
#define DE_PROFILE_PLOT(name, v)          de_prof_plot_((name), (f64)(v))
#define DE_PROFILE_ALLOC(ptr, size, pool) de_prof_alloc_((ptr), (size_t)(size), (pool))
#define DE_PROFILE_FREE(ptr, pool)        de_prof_free_((ptr), (pool))
#else
#define DE_PROFILE_ZONE_BEGIN(var, name)          ((void)0)
#define DE_PROFILE_ZONE_BEGIN_C(var, name, color) ((void)0)
#define DE_PROFILE_ZONE_END(var)                  ((void)0)
#define DE_PROFILE_FRAME(name)                    ((void)0)
#define DE_PROFILE_PLOT(name, v)                  ((void)0)
#define DE_PROFILE_ALLOC(ptr, size, pool)         ((void)0)
#define DE_PROFILE_FREE(ptr, pool)                ((void)0)
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
 * `vpath` is a virtual path (`user:/config.toml`, see de_file_read_all in de_platform.h).
 *
 * @return DE_OK; DE_ERR_NOT_FOUND (no such file), DE_ERR_INVALID_ARGUMENT (not a virtual path)
 *         or DE_ERR_IO (the file cannot be read); DE_ERR_PARSE (not valid TOML, with the
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
 * `vpath` is a virtual path on a writable mount. The file is replaced atomically
 * (de_file_write_all_atomic): a crash leaves the old or the new file, never a part.
 *
 * @return DE_OK, DE_ERR_IO, DE_ERR_NOT_FOUND (no parent directory), DE_ERR_INVALID_ARGUMENT
 *         (NULL or bad path, no writable mount) or DE_ERR_OUT_OF_MEMORY.
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

/* ---------- Console commands (S06 DENG-96) ----------
 *
 * SPEC §18.6. A process-wide registry of named commands that any module registers; the debug
 * console (S30) and `exec` files run them through de_command_exec. A line is split into words at
 * spaces and tabs. A word in double quotes keeps its spaces (`echo "a b" c` gives three words:
 * echo, a b, c); `\"` is a literal quote and `\\` a literal backslash, inside or outside quotes;
 * any other backslash is kept as it is (so `C:\dir` needs no escaping).
 *
 * The first word names a command or a cvar. Command names are matched without regard to ASCII
 * case. When no command has the name, a cvar with that name (lowercased) is used:
 * `r.vsync 0` sets it with de_cvar_set (several value words are joined with single spaces), and
 * `r.vsync` alone logs its value.
 *
 * Built-in commands (always registered): `help [prefix]` lists commands, `cvars [prefix]` lists
 * cvars with their values, `echo <words>` logs the words, `exec <vpath>` runs a file (one line
 * per command, lines starting with `#` are comments). Output goes to the log, category `console`,
 * at Info level; the console UI (S30) shows the log.
 *
 * Registration is thread-safe. de_command_exec is for the main thread (commands touch engine
 * state); a command function runs on the thread that called de_command_exec, with no lock held,
 * so it may register, unregister or execute commands itself.
 */

/** Longest command name, in bytes without the NUL. */
#define DE_COMMAND_NAME_MAX 63
/** Most words in one command line (more fail with DE_ERR_INVALID_ARGUMENT). */
#define DE_COMMAND_MAX_ARGS 64
/** Longest command line, in bytes without the NUL (longer fails with DE_ERR_INVALID_ARGUMENT). */
#define DE_COMMAND_LINE_MAX 4095

/**
 * A command. `argv[0]` is the name as typed, `argv[1..argc-1]` the arguments with quotes and
 * escapes removed; the strings are valid during the call only. Return DE_OK, or an error set
 * with DE_FAIL (bad arguments: DE_ERR_INVALID_ARGUMENT); de_command_exec returns it.
 */
typedef de_result (*de_command_fn)(int argc, const char *const *argv, void *user);

/**
 * Registers command `name` (1 to DE_COMMAND_NAME_MAX bytes, no spaces, tabs, quotes or control
 * characters). `help` is one line for `help` (may be NULL); name and help are copied.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL fn, bad name); DE_ERR_ALREADY_EXISTS (a command
 *         with this name, in any case, exists: unregister it first); DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_command_register(const char *name, const char *help, de_command_fn fn,
                                     void *user);

/**
 * Removes command `name` (any case). A game library unregisters its commands in `on_unload`, so
 * a reload can register them again with the new code. Built-in commands can be removed too.
 * @return DE_OK or DE_ERR_NOT_FOUND.
 */
DE_API de_result de_command_unregister(const char *name);

/**
 * Runs one command line (see the section comment). An empty line, or one whose first word starts
 * with `#`, does nothing and returns DE_OK. Main thread.
 * @return DE_OK; the command's result; DE_ERR_NOT_FOUND (no command or cvar with that name);
 *         de_cvar_set's errors for a cvar; DE_ERR_INVALID_ARGUMENT (NULL line, a line longer
 *         than DE_COMMAND_LINE_MAX, more than DE_COMMAND_MAX_ARGS words, an unclosed quote).
 */
DE_API de_result de_command_exec(const char *line);

/* ---------- Stats overlay (S06 DENG-96) ---------- */
typedef void (*de_stats_panel_fn)(void *user); /* draws with de_dbg_* */
DE_API void de_stats_register_panel(const char *name, de_stats_panel_fn fn, void *user);
DE_API void de_stats_overlay_toggle(void);

/* ---------- Crash handling (S03 DENG-251) ----------
 *
 * SPEC §18.5. de_crash_install() catches CPU crashes and DE_PANIC and writes a text report
 * `<dump_dir>/crash-<unixtime>-<pid>.txt` (reason, fault address, build ID, thread name,
 * annotations, backtrace), a minidump `<stem>.dmp` on Windows, and a pending marker
 * `<stem>.pending` that the next launch finds (S06 DENG-96 scans it and calls the upload
 * callback). After the report, the process dies with the original signal (default action) or the
 * Windows exception goes on to WER, so the OS crash reporter still sees the crash.
 *
 *   - POSIX (macOS, Linux): sigaction for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT and SIGTRAP,
 *     on a preallocated alternate signal stack (at least 64 KiB), so a stack overflow is
 *     reported too.
 *   - Windows: SetUnhandledExceptionFilter, a vectored handler for stack overflow, and the CRT
 *     SIGABRT signal; a thread created at install writes the minidump (MiniDumpWriteDump) and
 *     the report, because the crashing thread may have no stack left.
 *   - DE_PANIC: de_crash_install takes the panic hook slot (de_panic_set_hook), so a panic writes
 *     a report with the panic message before de_panic_ aborts. The abort that follows does not
 *     write a second report.
 *
 * One crash is reported per process: when several threads crash at once, the first writes the
 * report and the others wait for the process to die. The crash state is process-wide.
 */

/** A crash report as the next-launch upload flow passes it to the game (S06 DENG-96). */
typedef struct de_crash_report {
    const char *dump_path;       /**< minidump (Windows) or the text report (POSIX) */
    const char *log_path;        /**< the text report, or a log tail written beside it */
    const char *build_id;        /**< de_build_id() of the build that crashed */
    const char *gpu_breadcrumbs; /**< last render-graph passes, if a GPU fault (S30 DENG-333) */
    bool gpu_fault;
} de_crash_report;

/** Game-provided upload callback. Called on a normal thread at the next launch (S06 DENG-96),
 * never from a crash handler. */
typedef void (*de_crash_upload_fn)(const de_crash_report *report, void *user);

/** What caused a crash report. */
typedef enum de_crash_kind {
    DE_CRASH_SIGNAL,    /**< POSIX signal, or the CRT SIGABRT on Windows; `code` = signal number */
    DE_CRASH_EXCEPTION, /**< Windows structured exception; `code` = exception code */
    DE_CRASH_PANIC,     /**< DE_PANIC; `code` = 0, `panic_message` is set */
} de_crash_kind;

/**
 * What a chained crash handler (de_crash_add_handler) receives. Every string lives in
 * preallocated storage that stays valid for the whole handler chain; none may be kept after the
 * handler returns.
 */
typedef struct de_crash_context {
    de_crash_kind kind;
    u32 code;                  /**< signal number, exception code, or 0 for a panic */
    const char *reason;        /**< "SIGSEGV", "EXCEPTION_ACCESS_VIOLATION", "panic", ... */
    u64 fault_address;         /**< si_addr / the faulting data address; 0 for a panic */
    const char *panic_message; /**< the formatted DE_PANIC message, or NULL */
    const char *thread_name;   /**< the crashing thread's name, "" if it has none */
    const char *build_id;      /**< de_build_id() */
    const char *report_path;   /**< full native path of the text report: `<report_stem>.txt` */
    const char *report_stem;   /**< the same path without ".txt": `<dump_dir>/crash-<t>-<pid>` */
    const char *dump_path;     /**< `<report_stem>.dmp` on Windows when written, else NULL */
    intptr_t report_fd;        /**< the open report: POSIX file descriptor, or Win32 HANDLE */
} de_crash_context;

/**
 * A chained crash handler. Handlers run after the core report and the pending marker are
 * written, in registration order: on the crashing thread on POSIX (inside the signal handler, on
 * the alternate stack, or in the panic hook), on the dump thread on Windows. It must be
 * async-signal-safe: no malloc, no locks, no stdio, no logging. It may append to the report with
 * de_crash_write and create sibling files named `<report_stem>.<something>` with open()/write()
 * (CreateFileW on Windows).
 */
typedef void (*de_crash_handler_fn)(const de_crash_context *ctx, void *user);

/** Most chained handlers (de_crash_add_handler). */
#define DE_CRASH_MAX_HANDLERS 16
/** Annotation table size: entries, and bytes per key and value including the NUL. */
#define DE_CRASH_MAX_ANNOTATIONS      64
#define DE_CRASH_ANNOTATION_KEY_MAX   64
#define DE_CRASH_ANNOTATION_VALUE_MAX 256

/**
 * Installs the crash handlers (once per process) and sets where reports go.
 *
 * `dump_dir` is a native UTF-8 directory path (not a VFS path); NULL or "" is the current
 * directory. It is created if missing, then copied, so the crash path does no allocation and no
 * VFS work. `upload` and `user` are stored for the next-launch upload flow (S06 DENG-96);
 * `upload` is never called from a crash handler. Also sets up the calling thread's alternate
 * signal stack (de_crash_thread_init) and takes the panic hook slot (de_panic_set_hook).
 *
 * Calling it again only updates the directory and the upload callback; the handlers stay as
 * they are. Not thread-safe against a concurrent install or crash.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (path longer than about 900 bytes); DE_ERR_IO (the
 *         directory cannot be created, or a handler cannot be installed). On failure the
 *         previous state is kept.
 */
DE_API de_result de_crash_install(const char *dump_dir, de_crash_upload_fn upload, void *user);

/**
 * Sets a key/value pair that every crash report lists (level name, GPU, game state...).
 *
 * The table is preallocated: DE_CRASH_MAX_ANNOTATIONS entries of DE_CRASH_ANNOTATION_KEY_MAX-byte
 * keys and DE_CRASH_ANNOTATION_VALUE_MAX-byte values (each with its NUL); longer strings are cut
 * at a UTF-8 boundary. Setting an existing key overwrites its value; an empty or NULL value
 * removes the key and frees its entry. When the table is full a new key is dropped and counted
 * (the report shows the count). Thread-safe and allocation-free: each entry is a seqlock, so the
 * crash handler never waits on a writer. Works before de_crash_install. NULL or "" key: ignored.
 */
DE_API void de_crash_set_annotation(const char *key, const char *value);

/**
 * Adds a handler to the crash chain (see de_crash_handler_fn). Handlers run in the order they
 * were added. Thread-safe; works before de_crash_install. There is no removal.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL fn); DE_ERR_BUSY (DE_CRASH_MAX_HANDLERS already).
 */
DE_API de_result de_crash_add_handler(de_crash_handler_fn fn, void *user);

/**
 * Appends `n` bytes of `s` to the report of `ctx`. Async-signal-safe, no allocation; for use in
 * chained handlers. A NULL ctx or s, or an n of 0, does nothing.
 */
DE_API void de_crash_write(const de_crash_context *ctx, const char *s, size_t n);

/**
 * Prepares the calling thread for crash reporting: on POSIX it gives the thread its own alternate
 * signal stack of at least 64 KiB (unless it already has one), so a stack overflow on it is
 * reported; on Windows it reserves stack for the stack-overflow handler (SetThreadStackGuarantee).
 * Call it at the start of every long-lived thread; de_crash_install does it for its own thread.
 * Calling it twice is harmless. The stack is freed when the thread exits.
 */
DE_API void de_crash_thread_init(void);

/** The build ID: the git commit (`git rev-parse --short HEAD` at configure time), or "unknown"
 * outside a git checkout. Written into every crash report. Never NULL. */
DE_API const char *de_build_id(void);

/* ---------- Debug draw (DENG-572) ----------
 *
 * World-space lines and shapes that live for a time and belong to a named category. The editor
 * service draws them into its views (DENG-632) and lists the categories for the IDE, which turns
 * them on and off, recolours them and pauses them (paused shapes stay past their lifetime).
 * Debug and Dev builds; in Shipping every call is a no-op. Any thread.
 *
 * A shape drawn with `seconds` 0 lasts one frame. `color` alpha 0 takes the category's colour.
 */
typedef struct de_draw_category {
    u32 id; /**< 0 = invalid */
} de_draw_category;

/** The category `name`, registered on first use with `rgb` (0xRRGGBB). `game` marks a category
 * game code defines. Up to 64 categories; an invalid id after that. */
DE_API de_draw_category de_draw_category_register(const char *name, u32 rgb, bool game);
DE_API void de_draw_line(de_draw_category c, de_dvec3 a, de_dvec3 b, de_vec4 color, f32 seconds);
DE_API void de_draw_arrow(de_draw_category c, de_dvec3 from, de_dvec3 to, de_vec4 color, f32 seconds);
DE_API void de_draw_box(de_draw_category c, de_dvec3 min, de_dvec3 max, de_vec4 color, f32 seconds);
DE_API void de_draw_sphere(de_draw_category c, de_dvec3 center, f32 radius, de_vec4 color,
                           f32 seconds);
/** A text label at `at` (the editor draws a marker there; the text is counted). */
DE_API void de_draw_text(de_draw_category c, de_dvec3 at, const char *text, de_vec4 color,
                         f32 seconds);

/** One live line, as the editor reads them. */
typedef struct de_draw_line_item {
    de_dvec3 a, b;
    de_vec4 color;
} de_draw_line_item;
/** One category, as the editor reads them. */
typedef struct de_draw_category_info {
    const char *name;
    u32 rgb;
    bool on, game;
    u32 shapes; /**< drawn in the last frame */
} de_draw_category_info;

DE_API void de_draw_set_enabled(const char *category, bool on);
DE_API void de_draw_set_color(const char *category, u32 rgb);
DE_API void de_draw_set_paused(bool paused);
DE_API bool de_draw_paused(void);
/** Number of categories (registration order); `out` (nullable) gets up to `cap` of them. */
DE_API u32 de_draw_categories(de_draw_category_info *out, u32 cap);
/** Ages every shape by `dt` seconds (unless paused) and drops the expired ones; returns the
 * live lines of enabled categories, up to `cap`, in `out`. Main thread, once per frame. */
DE_API u32 de_draw_update(f32 dt, de_draw_line_item *out, u32 cap);

DE_EXTERN_C_END
#endif
