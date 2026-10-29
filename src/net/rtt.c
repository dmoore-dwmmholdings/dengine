#include "net/packet.h"

void de__chan_rtt_sample(de__chan_rtt *r, u64 sample) {
    if (!r->initialized) {
        r->smoothed_ns = sample;
        r->variance_ns = sample / 2;
        r->initialized = true;
        return;
    }
    const u64 delta = sample > r->smoothed_ns ? sample - r->smoothed_ns : r->smoothed_ns - sample;
    r->variance_ns = r->variance_ns - r->variance_ns / 4 + delta / 4;
    r->smoothed_ns = r->smoothed_ns - r->smoothed_ns / 8 + sample / 8;
}
u64 de__chan_resend_ns(const de__chan_rtt *r) {
    u64 result = r->smoothed_ns;
    const u64 quarter = result / 4;
    result = quarter > UINT64_MAX - result ? UINT64_MAX : result + quarter;
    result = r->variance_ns > UINT64_MAX - result ? UINT64_MAX : result + r->variance_ns;
    return result < 30000000ull ? 30000000ull : result;
}
