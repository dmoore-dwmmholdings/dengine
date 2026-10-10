/* de_debug.h — profiling (Tracy), cvars, console, stats overlay, crash handling. SPEC.md §5.8, §18. */
#ifndef DE_DEBUG_H
#define DE_DEBUG_H
#include "de_base.h"
DE_EXTERN_C_BEGIN

/* ---------- Profiling: compiled out unless DE_PROFILE is defined (Dev/Release) ---------- */
typedef struct de_prof_zone { u64 opaque[2]; } de_prof_zone;
typedef struct de_prof_srcloc { const char *name, *function, *file; u32 line, color; } de_prof_srcloc;
DE_API de_prof_zone de_prof_zone_begin_(const de_prof_srcloc *loc);
DE_API void         de_prof_zone_end_(de_prof_zone z);
DE_API void         de_prof_frame_mark_(const char *name);
DE_API void         de_prof_plot_(const char *name, f64 value);
DE_API void         de_prof_alloc_(const void *ptr, size_t size, const char *pool);
DE_API void         de_prof_free_(const void *ptr, const char *pool);

#ifdef DE_PROFILE
#  define DE_PROFILE_ZONE_BEGIN(var, name) \
     static const de_prof_srcloc var##_loc = { (name), __func__, __FILE__, (u32)__LINE__, 0 }; \
     de_prof_zone var = de_prof_zone_begin_(&var##_loc)
#  define DE_PROFILE_ZONE_END(var)  de_prof_zone_end_(var)
#  define DE_PROFILE_FRAME(name)    de_prof_frame_mark_(name)
#  define DE_PROFILE_PLOT(name, v)  de_prof_plot_((name), (f64)(v))
#else
#  define DE_PROFILE_ZONE_BEGIN(var, name) ((void)0)
#  define DE_PROFILE_ZONE_END(var)         ((void)0)
#  define DE_PROFILE_FRAME(name)           ((void)0)
#  define DE_PROFILE_PLOT(name, v)         ((void)0)
#endif

/* ---------- Console variables ---------- */
DE_HANDLE(de_cvar);
typedef enum de_cvar_type  { DE_CVAR_BOOL, DE_CVAR_INT, DE_CVAR_FLOAT, DE_CVAR_STRING, DE_CVAR_ENUM } de_cvar_type;
typedef enum de_cvar_flags { DE_CVAR_READONLY = 1u << 0, DE_CVAR_DEV_ONLY = 1u << 1, DE_CVAR_SAVED = 1u << 2,
                             DE_CVAR_REPLICATED = 1u << 3, DE_CVAR_CHEAT = 1u << 4 } de_cvar_flags;

typedef struct de_cvar_desc {
    const char  *name;            /* "r.shadows.quality" */
    const char  *help;
    de_cvar_type type;
    u32          flags;
    f64          default_number, min, max;
    const char  *default_string;
    const char *const *enum_values; u32 enum_count;
    void (*on_change)(de_cvar cv, void *user); void *user;
} de_cvar_desc;

DE_API de_cvar     de_cvar_register(const de_cvar_desc *desc);
DE_API de_cvar     de_cvar_find(const char *name);
DE_API bool        de_cvar_bool(de_cvar cv);
DE_API i64         de_cvar_int(de_cvar cv);
DE_API f64         de_cvar_float(de_cvar cv);
DE_API const char *de_cvar_string(de_cvar cv);
DE_API de_result   de_cvar_set(de_cvar cv, const char *value);    /* parses per type, enforces flags */
DE_API de_result   de_cvar_load(const char *vpath);               /* config.toml */
DE_API de_result   de_cvar_save(const char *vpath);               /* DE_CVAR_SAVED only */
DE_API void        de_cvar_apply_args(int argc, char **argv);     /* +name value */

/* ---------- Console commands ---------- */
typedef void (*de_command_fn)(int argc, const char *const *argv, void *user);
DE_API void      de_command_register(const char *name, const char *help, de_command_fn fn, void *user);
DE_API de_result de_command_exec(const char *line);

/* ---------- Stats overlay ---------- */
typedef void (*de_stats_panel_fn)(void *user);   /* draws with de_dbg_* */
DE_API void de_stats_register_panel(const char *name, de_stats_panel_fn fn, void *user);
DE_API void de_stats_overlay_toggle(void);

/* ---------- Crash handling ---------- */
typedef struct de_crash_report {
    const char *dump_path;          /* minidump or equivalent */
    const char *log_path;
    const char *build_id;
    const char *gpu_breadcrumbs;    /* last render-graph passes, if a GPU fault */
    bool        gpu_fault;
} de_crash_report;
typedef void (*de_crash_upload_fn)(const de_crash_report *report, void *user);   /* game-provided */

DE_API void de_crash_install(const char *dump_dir, de_crash_upload_fn upload, void *user);
DE_API void de_crash_set_annotation(const char *key, const char *value);   /* included in reports */

DE_EXTERN_C_END
#endif
