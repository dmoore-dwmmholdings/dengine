/* DENG-751 diagnostic branch only: no product optimization or timing qualification. */
#ifndef DE_DENG603_TRACE_INTERNAL_H
#define DE_DENG603_TRACE_INTERNAL_H
#include <dengine/de_base.h>
#define DE_DENG603_PHASES 3u
typedef struct de_deng603_trace {
    bool enabled;
    u32 style_count, layout_count;
    u64 style_ns[DE_DENG603_PHASES], layout_ns[DE_DENG603_PHASES];
} de_deng603_trace;
extern de_deng603_trace de__deng603_trace;
#endif
