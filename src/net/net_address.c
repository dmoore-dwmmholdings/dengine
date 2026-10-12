/* net_address.c: de_net_address parsing, formatting, comparison and hashing (DENG-51).
 *
 * Own parser and formatter instead of inet_pton/inet_ntop: the same text on every OS (Windows
 * and glibc differ on leading zeros and on the mapped form), no Winsock initialisation, and the
 * zone index stays numeric. Pure functions; no allocation. */
#include "net/net_internal.h"

#include <stdio.h>
#include <string.h>

/* ---------- Constructors ---------- */

de_net_address de_net_address_any(de_net_family family, u16 port) {
    de_net_address a;
    memset(&a, 0, sizeof a);
    if (family == DE_NET_FAMILY_IPV4 || family == DE_NET_FAMILY_IPV6) {
        a.family = (u16)family;
        a.port = port;
    }
    return a;
}

de_net_address de_net_address_loopback(de_net_family family, u16 port) {
    de_net_address a = de_net_address_any(family, port);
    if (family == DE_NET_FAMILY_IPV4) {
        a.addr[0] = 127;
        a.addr[3] = 1;
    } else if (family == DE_NET_FAMILY_IPV6) {
        a.addr[15] = 1;
    }
    return a;
}

de_net_address de_net_address_ipv4(u8 a, u8 b, u8 c, u8 d, u16 port) {
    de_net_address r = de_net_address_any(DE_NET_FAMILY_IPV4, port);
    r.addr[0] = a;
    r.addr[1] = b;
    r.addr[2] = c;
    r.addr[3] = d;
    return r;
}

/* ---------- Comparison and hashing ---------- */

static u32 addr_len(u16 family) {
    return family == DE_NET_FAMILY_IPV4 ? 4u : family == DE_NET_FAMILY_IPV6 ? 16u : 0u;
}

bool de_net_address_equal(const de_net_address *a, const de_net_address *b) {
    if (a == NULL || b == NULL)
        return a == b;
    if (a->family != b->family || a->port != b->port || a->scope_id != b->scope_id)
        return false;
    return memcmp(a->addr, b->addr, addr_len(a->family)) == 0;
}

u64 de_net_address_hash(const de_net_address *a) {
    /* A fixed little-endian encoding of exactly the compared fields, so the hash is the same on
     * every platform and ignores bytes an IPv4 address does not use. */
    u8 buf[24];
    memset(buf, 0, sizeof buf);
    if (a != NULL) {
        buf[0] = (u8)(a->family & 0xFFu);
        buf[1] = (u8)(a->family >> 8);
        buf[2] = (u8)(a->port & 0xFFu);
        buf[3] = (u8)(a->port >> 8);
        memcpy(buf + 4, a->addr, addr_len(a->family));
        for (u32 i = 0; i < 4; ++i)
            buf[20 + i] = (u8)(a->scope_id >> (8u * i));
    }
    return de_hash64(buf, sizeof buf, 0x6E65742E61646472ull /* "net.addr" */);
}

bool de__net_address_is_unspecified(const de_net_address *a) {
    u32 n = addr_len(a->family);
    if (n == 0)
        return false;
    for (u32 i = 0; i < n; ++i)
        if (a->addr[i] != 0)
            return false;
    return true;
}

static bool is_v4_mapped(const u8 b[16]) {
    for (u32 i = 0; i < 10; ++i)
        if (b[i] != 0)
            return false;
    return b[10] == 0xFF && b[11] == 0xFF;
}

void de__net_address_unmap(de_net_address *a) {
    if (a->family != DE_NET_FAMILY_IPV6 || !is_v4_mapped(a->addr))
        return;
    u8 v4[4];
    memcpy(v4, a->addr + 12, 4);
    memset(a->addr, 0, sizeof a->addr);
    memcpy(a->addr, v4, 4);
    a->family = DE_NET_FAMILY_IPV4;
    a->scope_id = 0;
}

/* ---------- Parsing ---------- */

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Dotted quad, exactly s[0 .. len). Octets 0-255, no leading zeros. */
static bool parse_ipv4(const char *s, size_t len, u8 out[4]) {
    size_t i = 0;
    for (u32 part = 0; part < 4; ++part) {
        if (part > 0) {
            if (i >= len || s[i] != '.')
                return false;
            ++i;
        }
        size_t start = i;
        u32 v = 0;
        while (i < len && is_digit(s[i]) && i - start < 3) {
            v = v * 10u + (u32)(s[i] - '0');
            ++i;
        }
        size_t n = i - start;
        if (n == 0 || v > 255u || (n > 1 && s[start] == '0'))
            return false;
        out[part] = (u8)v;
    }
    return i == len;
}

/* Decimal digits, exactly s[0 .. len), value <= max. */
static bool parse_decimal(const char *s, size_t len, u64 max, u64 *out) {
    if (len == 0 || len > 10)
        return false;
    u64 v = 0;
    for (size_t i = 0; i < len; ++i) {
        if (!is_digit(s[i]))
            return false;
        v = v * 10u + (u64)(s[i] - '0');
    }
    if (v > max)
        return false;
    *out = v;
    return true;
}

/* RFC 4291 §2.2 text form, exactly s[0 .. len), with an optional `%zone` (decimal). */
static bool parse_ipv6(const char *s, size_t len, u8 out[16], u32 *scope) {
    *scope = 0;
    const char *pct = memchr(s, '%', len);
    if (pct != NULL) {
        size_t alen = (size_t)(pct - s);
        u64 z;
        if (!parse_decimal(pct + 1, len - alen - 1, 0xFFFFFFFFull, &z))
            return false;
        *scope = (u32)z;
        len = alen;
    }

    u16 groups[8];
    u32 ng = 0;
    int gap = -1; /* group index where "::" stands */
    size_t i = 0;
    if (len >= 2 && s[0] == ':' && s[1] == ':') {
        gap = 0;
        i = 2;
    } else if (len >= 1 && s[0] == ':') {
        return false;
    }
    while (i < len) {
        size_t end = i;
        bool dotted = false;
        while (end < len && s[end] != ':') {
            if (s[end] == '.')
                dotted = true;
            ++end;
        }
        if (dotted) {
            /* A trailing dotted IPv4 fills the last two groups. */
            u8 v4[4];
            if (end != len || ng > 6 || !parse_ipv4(s + i, end - i, v4))
                return false;
            groups[ng++] = (u16)((v4[0] << 8) | v4[1]);
            groups[ng++] = (u16)((v4[2] << 8) | v4[3]);
            i = end;
            break;
        }
        size_t n = end - i;
        if (n == 0 || n > 4 || ng == 8)
            return false;
        u32 v = 0;
        for (size_t k = i; k < end; ++k) {
            int h = hex_value(s[k]);
            if (h < 0)
                return false;
            v = (v << 4) | (u32)h;
        }
        groups[ng++] = (u16)v;
        i = end;
        if (i == len)
            break;
        ++i; /* the ':' */
        if (i < len && s[i] == ':') {
            if (gap >= 0)
                return false;
            gap = (int)ng;
            ++i;
        } else if (i == len) {
            return false; /* trailing single ':' */
        }
    }
    if (gap < 0) {
        if (ng != 8)
            return false;
    } else if (ng > 7) {
        return false; /* "::" stands for at least one zero group */
    }

    u16 full[8];
    memset(full, 0, sizeof full);
    if (gap < 0) {
        memcpy(full, groups, sizeof full);
    } else {
        u32 head = (u32)gap, tail = ng - head;
        memcpy(full, groups, head * sizeof(u16));
        memcpy(full + 8 - tail, groups + head, tail * sizeof(u16));
    }
    for (u32 g = 0; g < 8; ++g) {
        out[2 * g] = (u8)(full[g] >> 8);
        out[2 * g + 1] = (u8)(full[g] & 0xFFu);
    }
    return true;
}

static bool parse_port(const char *s, size_t len, u16 *port) {
    u64 v;
    if (len > 5 || !parse_decimal(s, len, 65535u, &v))
        return false;
    *port = (u16)v;
    return true;
}

static bool looks_like_name(const char *s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')
            return true;
    }
    return false;
}

de_result de_net_address_parse(const char *text, de_net_address *out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_net_address_parse: out is NULL");
    memset(out, 0, sizeof *out);
    if (text == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_net_address_parse: text is NULL");

    size_t len = strlen(text);
    de_net_address a;
    memset(&a, 0, sizeof a);
    if (len > 0 && text[0] == '[') {
        const char *close = memchr(text, ']', len);
        if (close == NULL)
            return DE_FAIL(DE_ERR_PARSE, "net address '%s': '[' without ']'", text);
        size_t inner = (size_t)(close - text) - 1;
        size_t rest = len - inner - 2;
        if (rest == 0)
            return DE_FAIL(DE_ERR_PARSE, "net address '%s': missing ':port'", text);
        if (close[1] != ':')
            return DE_FAIL(DE_ERR_PARSE, "net address '%s': expected ':port' after ']'", text);
        if (!parse_port(close + 2, rest - 1, &a.port))
            return DE_FAIL(DE_ERR_PARSE, "net address '%s': port must be 0 to 65535", text);
        if (!parse_ipv6(text + 1, inner, a.addr, &a.scope_id)) {
            if (looks_like_name(text + 1, inner) && memchr(text + 1, ':', inner) == NULL)
                return DE_FAIL(DE_ERR_PARSE,
                               "net address '%s': not a numeric address; resolve host names "
                               "with de_net_resolve",
                               text);
            return DE_FAIL(DE_ERR_PARSE, "net address '%s': bad IPv6 address", text);
        }
        a.family = DE_NET_FAMILY_IPV6;
        *out = a;
        return DE_OK;
    }

    const char *colon = NULL;
    for (size_t i = 0; i < len; ++i)
        if (text[i] == ':')
            colon = text + i;
    if (colon == NULL)
        return DE_FAIL(DE_ERR_PARSE, "net address '%s': missing ':port'", text);
    size_t host_len = (size_t)(colon - text);
    if (memchr(text, ':', host_len) != NULL)
        return DE_FAIL(DE_ERR_PARSE, "net address '%s': write IPv6 as [address]:port", text);
    if (!parse_port(colon + 1, len - host_len - 1, &a.port))
        return DE_FAIL(DE_ERR_PARSE, "net address '%s': port must be 0 to 65535", text);
    if (!parse_ipv4(text, host_len, a.addr)) {
        if (looks_like_name(text, host_len))
            return DE_FAIL(DE_ERR_PARSE,
                           "net address '%s': not a numeric address; resolve host names with "
                           "de_net_resolve",
                           text);
        return DE_FAIL(DE_ERR_PARSE, "net address '%s': bad IPv4 address", text);
    }
    a.family = DE_NET_FAMILY_IPV4;
    *out = a;
    return DE_OK;
}

de_result de__net_split_host_port(const char *text, char *host, u32 host_cap, u16 *port) {
    size_t len = strlen(text);
    const char *h;
    size_t hlen;
    const char *p;
    if (len > 0 && text[0] == '[') {
        const char *close = memchr(text, ']', len);
        if (close == NULL || close[1] != ':')
            return DE_FAIL(DE_ERR_PARSE, "net address '%s': expected [host]:port", text);
        h = text + 1;
        hlen = (size_t)(close - h);
        p = close + 2;
    } else {
        const char *colon = strrchr(text, ':');
        if (colon == NULL)
            return DE_FAIL(DE_ERR_PARSE, "net address '%s': missing ':port'", text);
        h = text;
        hlen = (size_t)(colon - text);
        if (memchr(h, ':', hlen) != NULL)
            return DE_FAIL(DE_ERR_PARSE, "net address '%s': write IPv6 as [address]:port", text);
        p = colon + 1;
    }
    if (!parse_port(p, strlen(p), port))
        return DE_FAIL(DE_ERR_PARSE, "net address '%s': port must be 0 to 65535", text);
    if (hlen == 0)
        return DE_FAIL(DE_ERR_PARSE, "net address '%s': empty host", text);
    if (hlen + 1 > host_cap)
        return DE_FAIL(DE_ERR_PARSE, "net address '%s': host longer than %u bytes", text,
                       host_cap - 1u);
    memcpy(host, h, hlen);
    host[hlen] = '\0';
    return DE_OK;
}

/* ---------- Formatting ---------- */

/* Appends to tmp at *n (tmp is large enough for any address). */
static void put(char *tmp, u32 *n, const char *fmt, unsigned v) {
    int w = snprintf(tmp + *n, DE_NET_ADDRESS_STRING_MAX - *n, fmt, v);
    if (w > 0)
        *n += (u32)w;
}

u32 de_net_address_to_string(const de_net_address *a, char *buf, u32 buf_size) {
    char tmp[DE_NET_ADDRESS_STRING_MAX];
    u32 n = 0;
    if (a == NULL || (a->family != DE_NET_FAMILY_IPV4 && a->family != DE_NET_FAMILY_IPV6)) {
        memcpy(tmp, "(none)", 7);
        n = 6;
    } else if (a->family == DE_NET_FAMILY_IPV4) {
        for (u32 i = 0; i < 4; ++i)
            put(tmp, &n, i == 0 ? "%u" : ".%u", a->addr[i]);
        put(tmp, &n, ":%u", a->port);
    } else {
        tmp[n++] = '[';
        u16 g[8];
        for (u32 i = 0; i < 8; ++i)
            g[i] = (u16)((a->addr[2 * i] << 8) | a->addr[2 * i + 1]);
        if (is_v4_mapped(a->addr)) {
            /* RFC 5952 §5: mapped addresses keep the dotted IPv4 tail. */
            memcpy(tmp + n, "::ffff:", 7);
            n += 7;
            for (u32 i = 0; i < 4; ++i)
                put(tmp, &n, i == 0 ? "%u" : ".%u", a->addr[12 + i]);
        } else {
            /* RFC 5952 §4.2: compress the longest run of >= 2 zero groups, the first on a tie. */
            int best = -1, best_len = 0;
            for (int i = 0; i < 8;) {
                if (g[i] != 0) {
                    ++i;
                    continue;
                }
                int j = i;
                while (j < 8 && g[j] == 0)
                    ++j;
                if (j - i > best_len && j - i >= 2) {
                    best = i;
                    best_len = j - i;
                }
                i = j;
            }
            for (int i = 0; i < 8;) {
                if (i == best) {
                    tmp[n++] = ':';
                    tmp[n++] = ':';
                    i += best_len;
                    continue;
                }
                bool after_gap = best >= 0 && i == best + best_len;
                put(tmp, &n, (i == 0 || after_gap) ? "%x" : ":%x", g[i]);
                ++i;
            }
        }
        if (a->scope_id != 0)
            put(tmp, &n, "%%%u", a->scope_id);
        put(tmp, &n, "]:%u", a->port);
    }
    if (buf != NULL && buf_size > 0) {
        u32 c = n < buf_size - 1u ? n : buf_size - 1u;
        memcpy(buf, tmp, c);
        buf[c] = '\0';
    }
    return n;
}
