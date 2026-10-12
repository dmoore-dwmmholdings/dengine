/* crash_common.c: the OS-independent part of the crash handler (SPEC §18.5, DENG-251).
 *
 * Owns the crash API state: the annotation table, the handler chain, the build ID, the dump
 * directory and upload callback, the one-report-per-process state, and the report writer that
 * crash_posix.c and crash_win32.c call. Everything the report writer touches is static and
 * preallocated, so a crash does no allocation (see crash_internal.h for the file layout and the
 * pending marker format).
 *
 * S06 DENG-96 appends the next-launch marker scan and the upload call to this file. */
#include "platform/crash_internal.h"

#include <dengine/de_platform.h>

#include "platform/de_atomic.h"
#include "platform/vfs_internal.h" /* DENG-96: native file calls of the next-launch scan */

#include <signal.h>
#include <string.h>

#ifndef DE_BUILD_ID
#define DE_BUILD_ID "unknown"
#endif

/* ---------- Formatting ---------- */

size_t de__crash_fmt_u64(char *buf, u64 v, unsigned base) {
    static const char digits[] = "0123456789abcdef";
    char tmp[24];
    size_t n = 0;
    if (base != 16)
        base = 10;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0);
    for (size_t i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

bool de__crash_str_append(char *dst, size_t cap, const char *src) {
    size_t len = 0;
    while (len < cap && dst[len] != '\0')
        len++;
    if (len >= cap)
        return false;
    while (src != NULL && *src != '\0') {
        if (len + 1 >= cap) {
            dst[len] = '\0';
            return false;
        }
        dst[len++] = *src++;
    }
    dst[len] = '\0';
    return true;
}

size_t de__crash_str_copy_utf8(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (cap == 0)
        return 0;
    if (src != NULL) {
        while (n + 1 < cap && src[n] != '\0')
            n++;
        /* When cutting, do not split a UTF-8 sequence: back up over continuation bytes. */
        if (src[n] != '\0')
            while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u)
                n--;
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
    return n;
}

static void de__crash_puts(intptr_t fd, const char *s) {
    if (s != NULL)
        de__crash_os_write(fd, s, strlen(s));
}

static void de__crash_put_u64(intptr_t fd, u64 v, unsigned base) {
    char buf[24];
    size_t n = de__crash_fmt_u64(buf, v, base);
    if (base == 16)
        de__crash_puts(fd, "0x");
    de__crash_os_write(fd, buf, n);
}

/* Writes `s` on one line: line breaks and other control bytes become spaces. */
static void de__crash_put_line_text(intptr_t fd, const char *s) {
    char chunk[128];
    size_t n = 0;
    for (; s != NULL && *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        chunk[n++] = (c < 0x20u || c == 0x7Fu) ? ' ' : (char)c;
        if (n == sizeof chunk) {
            de__crash_os_write(fd, chunk, n);
            n = 0;
        }
    }
    if (n > 0)
        de__crash_os_write(fd, chunk, n);
}

const char *de__crash_signal_name(int sig) {
    /* POSIX numbers differ between systems for some of these, so compare against the macros. */
    switch (sig) {
#ifdef SIGSEGV
    case SIGSEGV:
        return "SIGSEGV";
#endif
#ifdef SIGBUS
    case SIGBUS:
        return "SIGBUS";
#endif
#ifdef SIGILL
    case SIGILL:
        return "SIGILL";
#endif
#ifdef SIGFPE
    case SIGFPE:
        return "SIGFPE";
#endif
#ifdef SIGABRT
    case SIGABRT:
        return "SIGABRT";
#endif
#ifdef SIGTRAP
    case SIGTRAP:
        return "SIGTRAP";
#endif
    default:
        return "signal";
    }
}

/* ---------- Annotation table ----------
 *
 * DE_CRASH_MAX_ANNOTATIONS slots, each a seqlock over atomic 64-bit words (so a reader racing a
 * writer is not a data race, and TSan sees none). `seq` is even when the slot is stable and odd
 * while a writer is inside. Writers of one slot exclude each other by moving `seq` from even to
 * odd with a CAS; the crash handler never takes that lock, it only retries a bounded number of
 * times and then reports what it read.
 *
 * Updating an existing key takes only that slot's lock. Taking a free slot for a new key also
 * holds `g_annot_claim`, so two threads cannot add the same key twice. A key with an empty
 * value frees its slot. */

#define DE__KEY_WORDS (DE_CRASH_ANNOTATION_KEY_MAX / 8)
#define DE__VAL_WORDS (DE_CRASH_ANNOTATION_VALUE_MAX / 8)
_Static_assert(DE_CRASH_ANNOTATION_KEY_MAX % 8 == 0, "key size must be whole words");
_Static_assert(DE_CRASH_ANNOTATION_VALUE_MAX % 8 == 0, "value size must be whole words");
_Static_assert(DE_ATOMIC_LOCK_FREE_U32 &&DE_ATOMIC_LOCK_FREE_U64,
               "the crash handler needs lock-free atomics");

typedef struct de__annot_slot {
    de_atomic_u32 seq;
    de_atomic_u64 key[DE__KEY_WORDS];
    de_atomic_u64 val[DE__VAL_WORDS];
} de__annot_slot;

static de__annot_slot g_annot[DE_CRASH_MAX_ANNOTATIONS];
static de_atomic_u32 g_annot_claim;
static de_atomic_u64 g_annot_dropped;

/* Readers in the crash path give up after this many torn reads. */
#define DE__SEQLOCK_CRASH_TRIES 64

static void de__words_store(de_atomic_u64 *w, size_t nw, const char *s) {
    char buf[DE_CRASH_ANNOTATION_VALUE_MAX];
    size_t bytes = nw * 8;
    memset(buf, 0, bytes);
    if (s != NULL)
        memcpy(buf, s, strlen(s)); /* callers pass a string that fits (with its NUL) */
    for (size_t i = 0; i < nw; i++) {
        u64 x;
        memcpy(&x, buf + i * 8, 8);
        de_atomic_store_u64(&w[i], x, DE_ATOMIC_RELAXED);
    }
}

static void de__words_load(const de_atomic_u64 *w, size_t nw, char *out) {
    for (size_t i = 0; i < nw; i++) {
        u64 x = de_atomic_load_u64(&w[i], DE_ATOMIC_RELAXED);
        memcpy(out + i * 8, &x, 8);
    }
    out[nw * 8 - 1] = '\0'; /* a torn read may lack its NUL */
}

static u32 de__slot_lock(de__annot_slot *s) {
    u32 seq = de_atomic_load_u32(&s->seq, DE_ATOMIC_RELAXED);
    for (;;) {
        if ((seq & 1u) != 0) {
            de_cpu_pause();
            seq = de_atomic_load_u32(&s->seq, DE_ATOMIC_RELAXED);
            continue;
        }
        if (de_atomic_cas_weak_u32(&s->seq, &seq, seq + 1u, DE_ATOMIC_ACQUIRE, DE_ATOMIC_RELAXED))
            break;
    }
    /* Orders the odd `seq` before the data stores, for readers that pair it with their acquire
     * fence (the standard seqlock argument). */
    de_atomic_thread_fence(DE_ATOMIC_RELEASE);
    return seq + 1u;
}

static void de__slot_unlock(de__annot_slot *s, u32 odd) {
    de_atomic_store_u32(&s->seq, odd + 1u, DE_ATOMIC_RELEASE);
}

/* Seqlock read of a slot. `key`/`val` may be NULL. With max_tries == 0 it retries until it
 * gets a consistent copy (normal threads: writers hold a slot only briefly); otherwise it stops
 * after max_tries and keeps the last, possibly torn, copy. Returns true when consistent. */
static bool de__slot_read(const de__annot_slot *s, char *key, char *val, u32 max_tries) {
    for (u32 tries = 0;; tries++) {
        bool last = max_tries != 0 && tries + 1 >= max_tries;
        u32 s1 = de_atomic_load_u32(&s->seq, DE_ATOMIC_ACQUIRE);
        if ((s1 & 1u) != 0 && !last) {
            de_cpu_pause();
            continue;
        }
        if (key != NULL)
            de__words_load(s->key, DE__KEY_WORDS, key);
        if (val != NULL)
            de__words_load(s->val, DE__VAL_WORDS, val);
        de_atomic_thread_fence(DE_ATOMIC_ACQUIRE);
        u32 s2 = de_atomic_load_u32(&s->seq, DE_ATOMIC_RELAXED);
        if (s1 == s2 && (s1 & 1u) == 0)
            return true;
        if (last)
            return false;
    }
}

/* Under the slot lock: the slot's key (only lock holders change it). */
static void de__slot_key_locked(const de__annot_slot *s, char *key) {
    de__words_load(s->key, DE__KEY_WORDS, key);
}

/* Under the slot lock: writes the value, or frees the slot when `val` is empty. */
static void de__slot_write_locked(de__annot_slot *s, const char *key, const char *val) {
    if (val[0] == '\0') {
        de__words_store(s->key, DE__KEY_WORDS, NULL);
        de__words_store(s->val, DE__VAL_WORDS, NULL);
    } else {
        if (key != NULL)
            de__words_store(s->key, DE__KEY_WORDS, key);
        de__words_store(s->val, DE__VAL_WORDS, val);
    }
}

/* Fast path: updates `key` if a slot holds it. Returns false when no slot does. */
static bool de__annot_update_existing(const char *key, const char *val) {
    char k[DE_CRASH_ANNOTATION_KEY_MAX];
retry:
    for (u32 i = 0; i < DE_CRASH_MAX_ANNOTATIONS; i++) {
        de__annot_slot *s = &g_annot[i];
        (void)de__slot_read(s, k, NULL, 0);
        if (strcmp(k, key) != 0)
            continue;
        u32 odd = de__slot_lock(s);
        de__slot_key_locked(s, k);
        if (strcmp(k, key) != 0) {
            /* Freed (and maybe reused) between the read and the lock: look again. */
            de__slot_unlock(s, odd);
            goto retry;
        }
        de__slot_write_locked(s, NULL, val);
        de__slot_unlock(s, odd);
        return true;
    }
    return false;
}

void de_crash_set_annotation(const char *key, const char *value) {
    char k[DE_CRASH_ANNOTATION_KEY_MAX];
    char v[DE_CRASH_ANNOTATION_VALUE_MAX];
    if (de__crash_str_copy_utf8(k, sizeof k, key) == 0)
        return;
    de__crash_str_copy_utf8(v, sizeof v, value);

    if (de__annot_update_existing(k, v) || v[0] == '\0')
        return;

    /* New key: claim a free slot. Under the claim lock no other thread adds a key, so a second
     * look for the key and the choice of a free slot cannot race another add. */
    u32 expected = 0;
    while (!de_atomic_cas_weak_u32(&g_annot_claim, &expected, 1u, DE_ATOMIC_ACQUIRE,
                                   DE_ATOMIC_RELAXED)) {
        expected = 0;
        de_cpu_pause();
    }
    i32 free_slot = -1;
    bool done = false;
    char cur[DE_CRASH_ANNOTATION_KEY_MAX];
    for (u32 i = 0; i < DE_CRASH_MAX_ANNOTATIONS && !done; i++) {
        de__annot_slot *s = &g_annot[i];
        u32 odd = de__slot_lock(s);
        de__slot_key_locked(s, cur);
        if (strcmp(cur, k) == 0) {
            de__slot_write_locked(s, NULL, v); /* added by another thread meanwhile */
            done = true;
        } else if (cur[0] == '\0' && free_slot < 0) {
            free_slot = (i32)i;
        }
        de__slot_unlock(s, odd);
    }
    if (!done) {
        if (free_slot >= 0) {
            /* Only claim-lock holders fill a free slot, so it is still free. */
            de__annot_slot *s = &g_annot[free_slot];
            u32 odd = de__slot_lock(s);
            de__slot_write_locked(s, k, v);
            de__slot_unlock(s, odd);
        } else {
            de_atomic_fetch_add_u64(&g_annot_dropped, 1, DE_ATOMIC_RELAXED);
        }
    }
    de_atomic_store_u32(&g_annot_claim, 0, DE_ATOMIC_RELEASE);
}

bool de__crash_annotation_get(const char *key, char *out, size_t cap) {
    char want[DE_CRASH_ANNOTATION_KEY_MAX];
    char k[DE_CRASH_ANNOTATION_KEY_MAX];
    char v[DE_CRASH_ANNOTATION_VALUE_MAX];
    if (de__crash_str_copy_utf8(want, sizeof want, key) == 0)
        return false;
    for (u32 i = 0; i < DE_CRASH_MAX_ANNOTATIONS; i++) {
        (void)de__slot_read(&g_annot[i], k, v, 0);
        if (strcmp(k, want) == 0) {
            if (out != NULL && cap > 0)
                de__crash_str_copy_utf8(out, cap, v);
            return true;
        }
    }
    return false;
}

u32 de__crash_annotation_count(void) {
    char k[DE_CRASH_ANNOTATION_KEY_MAX];
    u32 n = 0;
    for (u32 i = 0; i < DE_CRASH_MAX_ANNOTATIONS; i++) {
        (void)de__slot_read(&g_annot[i], k, NULL, 0);
        n += k[0] != '\0';
    }
    return n;
}

u64 de__crash_annotation_dropped(void) {
    return de_atomic_load_u64(&g_annot_dropped, DE_ATOMIC_RELAXED);
}

/* ---------- Handler chain ---------- */

/* A handler is published by its `ready` flag (release), after fn and user are stored. Slots are
 * handed out by `g_handler_next` in call order, which is the run order. */
static de_crash_handler_fn g_handler_fn[DE_CRASH_MAX_HANDLERS];
static void *g_handler_user[DE_CRASH_MAX_HANDLERS];
static de_atomic_u32 g_handler_ready[DE_CRASH_MAX_HANDLERS];
static de_atomic_u32 g_handler_next;

de_result de_crash_add_handler(de_crash_handler_fn fn, void *user) {
    if (fn == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_crash_add_handler: NULL handler");
    u32 i = de_atomic_load_u32(&g_handler_next, DE_ATOMIC_RELAXED);
    do {
        if (i >= DE_CRASH_MAX_HANDLERS)
            return DE_FAIL(DE_ERR_BUSY, "de_crash_add_handler: all %d handler slots are taken",
                           DE_CRASH_MAX_HANDLERS);
    } while (
        !de_atomic_cas_weak_u32(&g_handler_next, &i, i + 1u, DE_ATOMIC_RELAXED, DE_ATOMIC_RELAXED));
    g_handler_fn[i] = fn;
    g_handler_user[i] = user;
    de_atomic_store_u32(&g_handler_ready[i], 1u, DE_ATOMIC_RELEASE);
    return DE_OK;
}

static void de__crash_run_chain(const de_crash_context *ctx) {
    u32 n = de_atomic_load_u32(&g_handler_next, DE_ATOMIC_ACQUIRE);
    if (n > DE_CRASH_MAX_HANDLERS)
        n = DE_CRASH_MAX_HANDLERS;
    for (u32 i = 0; i < n; i++)
        if (de_atomic_load_u32(&g_handler_ready[i], DE_ATOMIC_ACQUIRE) != 0)
            g_handler_fn[i](ctx, g_handler_user[i]);
}

void de_crash_write(const de_crash_context *ctx, const char *s, size_t n) {
    if (ctx == NULL || s == NULL || n == 0 || ctx->report_fd < 0)
        return;
    de__crash_os_write(ctx->report_fd, s, n);
}

/* ---------- Build ID ---------- */

const char *de_build_id(void) {
    return DE_BUILD_ID;
}

/* ---------- Install state ---------- */

static char g_dir[DE__CRASH_DIR_MAX + 1];
static de_crash_upload_fn g_upload;
static void *g_upload_user;
static bool g_os_installed;

const char *de__crash_dump_dir(void) {
    return g_dir;
}

de_crash_upload_fn de__crash_upload(void **user) {
    if (user != NULL)
        *user = g_upload_user;
    return g_upload;
}

static void de__crash_panic_hook(const de_panic_info *info, void *user) {
    (void)user;
    /* A panic runs on a normal thread (not in a signal handler), so it may log. de_panic_ has
     * already flushed the log once (DENG-150); flush again so this line gets out too. */
    const char *path = de__crash_os_panic(info);
    if (path != NULL) {
        DE_ERROR(DE_LOG_CAT_PLATFORM, "crash report written: %s", path);
        de_log_flush();
    }
}

de_result de_crash_install(const char *dump_dir, de_crash_upload_fn upload, void *user) {
    const char *dir = (dump_dir == NULL || dump_dir[0] == '\0') ? "." : dump_dir;
    size_t len = strlen(dir);
    if (len > DE__CRASH_DIR_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_crash_install: dump directory is %zu bytes (at most %d)", len,
                       DE__CRASH_DIR_MAX);
    de_result r = de__crash_os_mkdirs(dir);
    if (r != DE_OK)
        return r;

    if (!g_os_installed) {
        r = de__crash_os_install();
        if (r != DE_OK)
            return r;
        g_os_installed = true;
        de_panic_set_hook(de__crash_panic_hook, NULL);
    }

    memcpy(g_dir, dir, len + 1);
    /* Drop one trailing separator so "<dir>/crash-..." has exactly one. */
    while (len > 1 && (g_dir[len - 1] == '/' || g_dir[len - 1] == de__crash_os_separator()))
        g_dir[--len] = '\0';
    g_upload = upload;
    g_upload_user = user;
    de_crash_thread_init();
    /* DENG-96: hand the reports of earlier crashes to the game, here on the calling thread. */
    if (upload != NULL)
        (void)de__crash_upload_pending();
    return DE_OK;
}

/* ---------- One report per process ---------- */

enum { DE__CRASH_IDLE = 0, DE__CRASH_REPORTING = 1, DE__CRASH_FINISHED = 2 };
static de_atomic_u32 g_state;
static de_atomic_u64 g_owner;

de__crash_entry de__crash_enter(u64 tid) {
    u32 expected = DE__CRASH_IDLE;
    if (de_atomic_cas_u32(&g_state, &expected, DE__CRASH_REPORTING, DE_ATOMIC_ACQ_REL,
                          DE_ATOMIC_ACQUIRE)) {
        de_atomic_store_u64(&g_owner, tid, DE_ATOMIC_RELEASE);
        return DE__CRASH_ENTER_OWNER;
    }
    /* Only the owner itself can match: it stored its id before doing anything else. */
    if (de_atomic_load_u64(&g_owner, DE_ATOMIC_ACQUIRE) == tid)
        return DE__CRASH_ENTER_SAME;
    return DE__CRASH_ENTER_OTHER;
}

void de__crash_leave(void) {
    de_atomic_store_u32(&g_state, DE__CRASH_FINISHED, DE_ATOMIC_RELEASE);
}

bool de__crash_finished(void) {
    return de_atomic_load_u32(&g_state, DE_ATOMIC_ACQUIRE) == DE__CRASH_FINISHED;
}

/* ---------- Report writer ---------- */

/* Static, not on the (alternate) stack: only the crash owner uses them. */
static char g_stem[DE__CRASH_PATH_CAP];
static char g_report_path[DE__CRASH_PATH_CAP];
static char g_dump_path[DE__CRASH_PATH_CAP];
static char g_marker_path[DE__CRASH_PATH_CAP];
static char g_rep_key[DE_CRASH_ANNOTATION_KEY_MAX];
static char g_rep_val[DE_CRASH_ANNOTATION_VALUE_MAX];

static void de__crash_make_stem(u64 t, u64 pid, u32 attempt) {
    char num[24];
    char sep[2] = {de__crash_os_separator(), '\0'};
    g_stem[0] = '\0';
    de__crash_str_append(g_stem, sizeof g_stem, g_dir);
    de__crash_str_append(g_stem, sizeof g_stem, sep);
    de__crash_str_append(g_stem, sizeof g_stem, "crash-");
    de__crash_fmt_u64(num, t, 10);
    de__crash_str_append(g_stem, sizeof g_stem, num);
    de__crash_str_append(g_stem, sizeof g_stem, "-");
    de__crash_fmt_u64(num, pid, 10);
    de__crash_str_append(g_stem, sizeof g_stem, num);
    if (attempt > 0) {
        de__crash_str_append(g_stem, sizeof g_stem, "-");
        de__crash_fmt_u64(num, attempt, 10);
        de__crash_str_append(g_stem, sizeof g_stem, num);
    }
}

static void de__crash_path_with(char *out, const char *ext) {
    out[0] = '\0';
    de__crash_str_append(out, DE__CRASH_PATH_CAP, g_stem);
    de__crash_str_append(out, DE__CRASH_PATH_CAP, ext);
}

static void de__crash_write_annotations(intptr_t fd) {
    u32 n = 0;
    de__crash_puts(fd, "annotations:\n");
    for (u32 i = 0; i < DE_CRASH_MAX_ANNOTATIONS; i++) {
        bool ok = de__slot_read(&g_annot[i], g_rep_key, g_rep_val, DE__SEQLOCK_CRASH_TRIES);
        if (g_rep_key[0] == '\0')
            continue;
        de__crash_puts(fd, "  ");
        de__crash_put_line_text(fd, g_rep_key);
        de__crash_puts(fd, " = ");
        de__crash_put_line_text(fd, g_rep_val);
        de__crash_puts(fd, ok ? "\n" : "  (torn: being written during the crash)\n");
        n++;
    }
    if (n == 0)
        de__crash_puts(fd, "  (none)\n");
    u64 dropped = de__crash_annotation_dropped();
    if (dropped > 0) {
        de__crash_puts(fd, "annotations_dropped: ");
        de__crash_put_u64(fd, dropped, 10);
        de__crash_puts(fd, " (table full)\n");
    }
}

static void de__crash_write_marker(const de__crash_event *ev) {
    de__crash_path_with(g_marker_path, ".pending");
    bool exists = false;
    intptr_t fd = de__crash_os_create(g_marker_path, &exists);
    if (fd < 0)
        return;
    de__crash_puts(fd, "report=");
    de__crash_puts(fd, g_report_path);
    de__crash_puts(fd, "\n");
    if (g_dump_path[0] != '\0') {
        de__crash_puts(fd, "dump=");
        de__crash_puts(fd, g_dump_path);
        de__crash_puts(fd, "\n");
    }
    de__crash_puts(fd, "build_id=");
    de__crash_puts(fd, de_build_id());
    de__crash_puts(fd, "\nreason=");
    de__crash_puts(fd, ev->reason);
    de__crash_puts(fd, "\n");
    de__crash_os_close(fd);
}

const char *de__crash_write_report(const de__crash_event *ev, const de__crash_os_report *os) {
    if (g_dir[0] == '\0')
        return NULL;
    u64 t = de__crash_os_unix_time();
    u64 pid = de__crash_os_pid();

    intptr_t fd = -1;
    for (u32 attempt = 0; attempt < 10 && fd < 0; attempt++) {
        bool exists = false;
        de__crash_make_stem(t, pid, attempt);
        de__crash_path_with(g_report_path, ".txt");
        fd = de__crash_os_create(g_report_path, &exists);
        if (fd < 0 && !exists)
            return NULL;
    }
    if (fd < 0)
        return NULL;

    g_dump_path[0] = '\0';
    if (os != NULL && os->write_dump != NULL) {
        de__crash_path_with(g_dump_path, ".dmp");
        if (!os->write_dump(g_dump_path, os->user))
            g_dump_path[0] = '\0';
    }

    de__crash_puts(fd, "DEngine crash report\nreason: ");
    de__crash_puts(fd, ev->reason);
    if (ev->kind != DE_CRASH_PANIC) {
        de__crash_puts(fd, ev->kind == DE_CRASH_EXCEPTION ? " (exception " : " (signal ");
        de__crash_put_u64(fd, ev->code, ev->kind == DE_CRASH_EXCEPTION ? 16 : 10);
        de__crash_puts(fd, ")");
    }
    de__crash_puts(fd, "\nfault_address: ");
    de__crash_put_u64(fd, ev->fault_address, 16);
    if (ev->kind == DE_CRASH_PANIC) {
        de__crash_puts(fd, "\npanic: ");
        de__crash_put_line_text(fd, ev->panic_message);
        de__crash_puts(fd, "\npanic_location: ");
        de__crash_puts(fd, ev->panic_file != NULL ? ev->panic_file : "?");
        de__crash_puts(fd, ":");
        de__crash_put_u64(fd, ev->panic_line, 10);
        de__crash_puts(fd, " (");
        de__crash_puts(fd, ev->panic_function != NULL ? ev->panic_function : "?");
        de__crash_puts(fd, ")");
    }
    de__crash_puts(fd, "\nbuild_id: ");
    de__crash_puts(fd, de_build_id());
    de__crash_puts(fd, "\npid: ");
    de__crash_put_u64(fd, pid, 10);
    de__crash_puts(fd, "\ntime: ");
    de__crash_put_u64(fd, t, 10);
    de__crash_puts(fd, " (unix)\nthread: ");
    de__crash_put_line_text(fd, ev->thread_name[0] != '\0' ? ev->thread_name : "(unnamed)");
    de__crash_puts(fd, " (id ");
    de__crash_put_u64(fd, ev->thread_id, 10);
    de__crash_puts(fd, ")\n");
    if (os != NULL && os->write_dump != NULL) {
        de__crash_puts(fd, "minidump: ");
        de__crash_puts(fd, g_dump_path[0] != '\0' ? g_dump_path : "(failed)");
        de__crash_puts(fd, "\n");
    }
    de__crash_write_annotations(fd);
    if (os != NULL && os->write_backtrace != NULL)
        os->write_backtrace(fd, os->user);
    de__crash_puts(fd, "end of core report\n");

    /* The marker goes out before the handlers run: if one of them crashes, the report it was
     * appending to is still found at the next launch. */
    de__crash_write_marker(ev);

    de_crash_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.kind = ev->kind;
    ctx.code = ev->code;
    ctx.reason = ev->reason;
    ctx.fault_address = ev->fault_address;
    ctx.panic_message = ev->kind == DE_CRASH_PANIC ? ev->panic_message : NULL;
    ctx.thread_name = ev->thread_name;
    ctx.build_id = de_build_id();
    ctx.report_path = g_report_path;
    ctx.report_stem = g_stem;
    ctx.dump_path = g_dump_path[0] != '\0' ? g_dump_path : NULL;
    ctx.report_fd = fd;
    de__crash_run_chain(&ctx);

    de__crash_os_close(fd);
    return g_report_path;
}

/* ---------- Tests ---------- */

void de__crash_reset_for_tests(void) {
    for (u32 i = 0; i < DE_CRASH_MAX_ANNOTATIONS; i++) {
        de__annot_slot *s = &g_annot[i];
        u32 odd = de__slot_lock(s);
        de__words_store(s->key, DE__KEY_WORDS, NULL);
        de__words_store(s->val, DE__VAL_WORDS, NULL);
        de__slot_unlock(s, odd);
    }
    de_atomic_store_u64(&g_annot_dropped, 0, DE_ATOMIC_RELAXED);
    for (u32 i = 0; i < DE_CRASH_MAX_HANDLERS; i++)
        de_atomic_store_u32(&g_handler_ready[i], 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_handler_next, 0, DE_ATOMIC_RELEASE);
}

/* ---------- Next-launch upload (S06 DENG-96) ----------
 *
 * de_crash_install(dir, upload, user) with an upload callback scans `dir` for the pending
 * markers that earlier crashes left (`crash-*.pending`, format in crash_internal.h), and for each
 * one: renames the marker to `<stem>.sent` (or deletes it when the rename fails), then calls
 * `upload` once with the report's files. The marker is retired before the call, so a report is
 * never handed over twice, even when the upload callback itself crashes or the process is killed
 * during it. A marker that can be neither renamed nor deleted is skipped with a warning (it
 * would otherwise be uploaded at every launch). This runs on the calling thread, never from a
 * crash, and uses only platform file calls (no services/, no VFS: the dump directory is native).
 */

/* Marker names gathered per directory listing; a full batch triggers another listing. */
#define DE__CRASH_SCAN_BATCH 32
/* Bytes of a marker file name ("crash-<20>-<20>-<n>.pending" is at most 59). */
#define DE__CRASH_SCAN_NAME_CAP 96
/* Listings per scan: bounds the work when markers keep appearing or cannot be retired. */
#define DE__CRASH_SCAN_ROUNDS 16
/* Bytes read from a marker (three paths and two short values). */
#define DE__CRASH_MARKER_MAX 4096

typedef struct de__crash_scan {
    char names[DE__CRASH_SCAN_BATCH][DE__CRASH_SCAN_NAME_CAP];
    u32 count;
    bool full;
} de__crash_scan;

static bool de__crash_has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

static bool de__crash_scan_fn(const char *name, bool is_dir, void *user) {
    de__crash_scan *sc = (de__crash_scan *)user;
    if (is_dir || strncmp(name, "crash-", 6) != 0 || !de__crash_has_suffix(name, ".pending") ||
        strlen(name) >= DE__CRASH_SCAN_NAME_CAP)
        return true;
    if (sc->count == DE__CRASH_SCAN_BATCH) {
        sc->full = true;
        return false;
    }
    memcpy(sc->names[sc->count++], name, strlen(name) + 1);
    return true;
}

/* Copies the value of `key=` from the marker text into `out` (cap bytes); "" when absent. */
static void de__crash_marker_value(const char *text, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    out[0] = '\0';
    for (const char *line = text; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        if (len > klen && memcmp(line, key, klen) == 0 && line[klen] == '=') {
            size_t vlen = len - klen - 1;
            if (vlen > 0 && line[klen + vlen] == '\r')
                vlen--;
            if (vlen >= cap)
                vlen = cap - 1;
            memcpy(out, line + klen + 1, vlen);
            out[vlen] = '\0';
            return;
        }
        line = end != NULL ? end + 1 : NULL;
    }
}

/* Reads at most DE__CRASH_MARKER_MAX - 1 bytes of `path` into `out`, NUL-terminated. */
static bool de__crash_read_marker(const char *path, char *out) {
    de_file *f = NULL;
    if (de__file_open_native(path, DE_FILE_READ, de_heap_allocator(), &f) != DE_OK) {
        de_error_clear();
        return false;
    }
    u64 total = 0;
    for (;;) {
        u64 got = 0;
        if (de__file_pread(f, out + total, total, DE__CRASH_MARKER_MAX - 1 - total, &got) !=
            DE_OK) {
            de_error_clear();
            break;
        }
        total += got;
        if (got == 0 || total == DE__CRASH_MARKER_MAX - 1)
            break;
    }
    de_file_close(f);
    out[total] = '\0';
    return total > 0;
}

/* Retires one marker and hands its report to `upload`. Returns true when `upload` was called. */
static bool de__crash_upload_one(const char *dir, const char *name, de_crash_upload_fn upload,
                                 void *user) {
    static char marker[DE__CRASH_MARKER_MAX];
    char sep[2] = {de__crash_os_separator(), '\0'};
    char marker_path[DE__CRASH_PATH_CAP], sent_path[DE__CRASH_PATH_CAP];
    char report[DE__CRASH_PATH_CAP], dump[DE__CRASH_PATH_CAP], tail[DE__CRASH_PATH_CAP];
    char build_id[128];

    marker_path[0] = '\0';
    if (!de__crash_str_append(marker_path, sizeof marker_path, dir) ||
        !de__crash_str_append(marker_path, sizeof marker_path, sep) ||
        !de__crash_str_append(marker_path, sizeof marker_path, name))
        return false;
    size_t stem_len = strlen(marker_path) - (sizeof ".pending" - 1);
    memcpy(sent_path, marker_path, stem_len);
    sent_path[stem_len] = '\0';
    de__crash_str_append(sent_path, sizeof sent_path, ".sent");

    bool have_text = de__crash_read_marker(marker_path, marker);
    if (!have_text)
        marker[0] = '\0';

    /* Retire the marker first: "never twice" wins over "at least once". */
    if (de__native_rename_replace(marker_path, sent_path) != DE_OK &&
        de__native_remove(marker_path) != DE_OK) {
        de_error_clear();
        DE_WARN(DE_LOG_CAT_PLATFORM,
                "crash report marker %s can be neither renamed nor deleted; not uploading it",
                marker_path);
        return false;
    }
    de_error_clear();

    de__crash_marker_value(marker, "report", report, sizeof report);
    if (report[0] == '\0') { /* an empty or unreadable marker: the report is <stem>.txt */
        memcpy(report, marker_path, stem_len);
        report[stem_len] = '\0';
        de__crash_str_append(report, sizeof report, ".txt");
    }
    de__crash_marker_value(marker, "dump", dump, sizeof dump);
    de__crash_marker_value(marker, "build_id", build_id, sizeof build_id);

    /* The log tail sits beside the report: <report stem>.log_tail.txt. */
    tail[0] = '\0';
    if (de__crash_has_suffix(report, ".txt")) {
        size_t rs = strlen(report) - 4;
        memcpy(tail, report, rs);
        tail[rs] = '\0';
        de__crash_str_append(tail, sizeof tail, ".log_tail.txt");
        bool is_dir = false;
        if (de__native_stat(tail, &is_dir) != DE_OK || is_dir)
            tail[0] = '\0';
    }

    de_crash_report rep;
    memset(&rep, 0, sizeof rep);
    rep.dump_path = dump[0] != '\0' ? dump : report;
    rep.log_path = tail[0] != '\0' ? tail : report;
    rep.build_id = build_id[0] != '\0' ? build_id : "unknown";
    rep.gpu_breadcrumbs = NULL;
    rep.gpu_fault = false;
    DE_INFO(DE_LOG_CAT_PLATFORM, "crash report %s (build %s) handed to the upload callback", report,
            rep.build_id);
    upload(&rep, user);
    return true;
}

u32 de__crash_upload_pending(void) {
    void *user = NULL;
    de_crash_upload_fn upload = de__crash_upload(&user);
    if (upload == NULL || g_dir[0] == '\0')
        return 0;
    u32 uploaded = 0;
    de__crash_scan sc;
    for (u32 round = 0; round < DE__CRASH_SCAN_ROUNDS; ++round) {
        sc.count = 0;
        sc.full = false;
        if (de__native_dir_list(g_dir, de__crash_scan_fn, &sc) != DE_OK) {
            de_error_clear();
            DE_WARN(DE_LOG_CAT_PLATFORM, "crash dump directory %s cannot be listed", g_dir);
            break;
        }
        bool retired_any = false;
        for (u32 i = 0; i < sc.count; ++i) {
            if (de__crash_upload_one(g_dir, sc.names[i], upload, user)) {
                ++uploaded;
                retired_any = true;
            }
        }
        if (!sc.full || !retired_any)
            break;
    }
    return uploaded;
}
