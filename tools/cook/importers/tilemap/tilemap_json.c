/*
 * tilemap_json.c: the strict JSON reader of the tilemap importer (DENG-80). RFC 8259 only: no
 * comments, no trailing commas, no NaN/Infinity, no leading zeros, no unescaped control
 * characters; \u escapes (with surrogate pairs) become UTF-8. Recursion is bounded by
 * TMJ_MAX_DEPTH, and every byte comes from the import arena. cgltf's tokenizer is file-static,
 * so the importer has its own (DENG-80 note).
 */
#include "cook/importers/tilemap/tilemap_ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct jp {
    tm_arena *ar;
    const char *s;
    size_t n, i;
    char *err;
    size_t err_cap;
    bool failed;
} jp;

static void jfail(jp *p, const char *msg) {
    if (p->failed)
        return;
    p->failed = true;
    u32 line = 1, col = 1;
    for (size_t k = 0; k < p->i && k < p->n; ++k) {
        if (p->s[k] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
    }
    if (p->err != NULL && p->err_cap > 0)
        snprintf(p->err, p->err_cap, "%u:%u: %s", line, col, msg);
}

static void ws(jp *p) {
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        ++p->i;
    }
}

static bool lit(jp *p, const char *word) {
    size_t m = strlen(word);
    if (p->n - p->i < m || memcmp(p->s + p->i, word, m) != 0)
        return false;
    p->i += m;
    return true;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool hex4(jp *p, u32 *out) {
    if (p->n - p->i < 4) {
        jfail(p, "truncated \\u escape");
        return false;
    }
    u32 v = 0;
    for (u32 k = 0; k < 4; ++k) {
        int h = hexv(p->s[p->i + k]);
        if (h < 0) {
            jfail(p, "bad \\u escape");
            return false;
        }
        v = (v << 4) | (u32)h;
    }
    p->i += 4;
    *out = v;
    return true;
}

static size_t utf8(u32 cp, char *o) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* A string at p->s[p->i] == '"'. The decoded form is never longer than the source. */
static bool str(jp *p, const char **out, u32 *len) {
    ++p->i; /* opening quote */
    size_t start = p->i;
    size_t end = start;
    bool escapes = false;
    while (end < p->n && p->s[end] != '"') {
        unsigned char c = (unsigned char)p->s[end];
        if (c < 0x20) {
            p->i = end;
            jfail(p, "control character in string");
            return false;
        }
        if (c == '\\') {
            escapes = true;
            ++end;
            if (end >= p->n)
                break;
        }
        ++end;
    }
    if (end >= p->n) {
        p->i = end;
        jfail(p, "unterminated string");
        return false;
    }
    if ((u64)(end - start) > 0x7fffffffu) {
        jfail(p, "string too long");
        return false;
    }
    char *o = (char *)tm_alloc(p->ar, end - start + 1, 1);
    if (o == NULL) {
        jfail(p, "out of memory");
        return false;
    }
    if (!escapes) {
        memcpy(o, p->s + start, end - start);
        *out = o;
        *len = (u32)(end - start);
        p->i = end + 1;
        return true;
    }
    size_t w = 0;
    while (p->i < end) {
        char c = p->s[p->i++];
        if (c != '\\') {
            o[w++] = c;
            continue;
        }
        char e = p->s[p->i++];
        switch (e) {
        case '"':
            o[w++] = '"';
            break;
        case '\\':
            o[w++] = '\\';
            break;
        case '/':
            o[w++] = '/';
            break;
        case 'b':
            o[w++] = '\b';
            break;
        case 'f':
            o[w++] = '\f';
            break;
        case 'n':
            o[w++] = '\n';
            break;
        case 'r':
            o[w++] = '\r';
            break;
        case 't':
            o[w++] = '\t';
            break;
        case 'u': {
            u32 cp;
            if (!hex4(p, &cp))
                return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                u32 lo;
                if (p->i + 2 > end || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u') {
                    jfail(p, "unpaired surrogate");
                    return false;
                }
                p->i += 2;
                if (!hex4(p, &lo))
                    return false;
                if (lo < 0xDC00 || lo > 0xDFFF) {
                    jfail(p, "unpaired surrogate");
                    return false;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                jfail(p, "unpaired surrogate");
                return false;
            }
            /* \u0000 would cut the C string short: refused. */
            if (cp == 0) {
                jfail(p, "\\u0000 in string");
                return false;
            }
            w += utf8(cp, o + w);
            break;
        }
        default:
            --p->i;
            jfail(p, "bad escape");
            return false;
        }
    }
    o[w] = '\0';
    *out = o;
    *len = (u32)w;
    p->i = end + 1;
    return true;
}

static bool num(jp *p, f64 *out) {
    size_t start = p->i;
    if (p->i < p->n && p->s[p->i] == '-')
        ++p->i;
    if (p->i >= p->n) {
        jfail(p, "bad number");
        return false;
    }
    if (p->s[p->i] == '0') {
        ++p->i;
    } else if (p->s[p->i] >= '1' && p->s[p->i] <= '9') {
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
            ++p->i;
    } else {
        jfail(p, "bad number");
        return false;
    }
    if (p->i < p->n && p->s[p->i] == '.') {
        ++p->i;
        size_t d = p->i;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
            ++p->i;
        if (p->i == d) {
            jfail(p, "bad number: no digit after '.'");
            return false;
        }
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        ++p->i;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-'))
            ++p->i;
        size_t d = p->i;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
            ++p->i;
        if (p->i == d) {
            jfail(p, "bad number: no exponent digit");
            return false;
        }
    }
    if (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
        jfail(p, "bad number: leading zero");
        return false;
    }
    size_t len = p->i - start;
    char buf[64];
    if (len >= sizeof buf) {
        jfail(p, "number too long");
        return false;
    }
    memcpy(buf, p->s + start, len);
    buf[len] = '\0';
    *out = strtod(buf, NULL);
    if (!(*out == *out) || *out - *out != 0.0) {
        jfail(p, "number out of range");
        return false;
    }
    return true;
}

static bool value(jp *p, tmj_val *v, u32 depth);

static bool array(jp *p, tmj_val *v, u32 depth) {
    ++p->i; /* [ */
    v->kind = TMJ_ARR;
    tm_vec items;
    tm_vec_init(&items, sizeof(tmj_val));
    ws(p);
    if (p->i < p->n && p->s[p->i] == ']') {
        ++p->i;
        return true;
    }
    for (;;) {
        tmj_val *slot = (tmj_val *)tm_vec_push(p->ar, &items, NULL);
        if (slot == NULL) {
            jfail(p, "out of memory");
            return false;
        }
        if (!value(p, slot, depth + 1))
            return false;
        ws(p);
        if (p->i < p->n && p->s[p->i] == ',') {
            ++p->i;
            continue;
        }
        if (p->i < p->n && p->s[p->i] == ']') {
            ++p->i;
            break;
        }
        jfail(p, "expected ',' or ']'");
        return false;
    }
    v->count = items.count;
    v->u.items = (tmj_val *)items.data;
    return true;
}

static bool object(jp *p, tmj_val *v, u32 depth) {
    ++p->i; /* { */
    v->kind = TMJ_OBJ;
    tm_vec members;
    tm_vec_init(&members, sizeof(tmj_member));
    ws(p);
    if (p->i < p->n && p->s[p->i] == '}') {
        ++p->i;
        return true;
    }
    for (;;) {
        ws(p);
        if (p->i >= p->n || p->s[p->i] != '"') {
            jfail(p, "expected a member name");
            return false;
        }
        tmj_member *m = (tmj_member *)tm_vec_push(p->ar, &members, NULL);
        if (m == NULL) {
            jfail(p, "out of memory");
            return false;
        }
        if (!str(p, &m->key, &m->key_len))
            return false;
        ws(p);
        if (p->i >= p->n || p->s[p->i] != ':') {
            jfail(p, "expected ':'");
            return false;
        }
        ++p->i;
        if (!value(p, &m->val, depth + 1))
            return false;
        ws(p);
        if (p->i < p->n && p->s[p->i] == ',') {
            ++p->i;
            continue;
        }
        if (p->i < p->n && p->s[p->i] == '}') {
            ++p->i;
            break;
        }
        jfail(p, "expected ',' or '}'");
        return false;
    }
    v->count = members.count;
    v->u.members = (tmj_member *)members.data;
    return true;
}

static bool value(jp *p, tmj_val *v, u32 depth) {
    if (depth > TMJ_MAX_DEPTH) {
        jfail(p, "nesting too deep");
        return false;
    }
    ws(p);
    if (p->i >= p->n) {
        jfail(p, "unexpected end of input");
        return false;
    }
    memset(v, 0, sizeof *v);
    char c = p->s[p->i];
    switch (c) {
    case '{':
        return object(p, v, depth);
    case '[':
        return array(p, v, depth);
    case '"': {
        v->kind = TMJ_STR;
        return str(p, &v->u.s, &v->count);
    }
    case 't':
        if (!lit(p, "true"))
            break;
        v->kind = TMJ_BOOL;
        v->u.b = true;
        return true;
    case 'f':
        if (!lit(p, "false"))
            break;
        v->kind = TMJ_BOOL;
        v->u.b = false;
        return true;
    case 'n':
        if (!lit(p, "null"))
            break;
        v->kind = TMJ_NULL;
        return true;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            v->kind = TMJ_NUM;
            return num(p, &v->u.n);
        }
        break;
    }
    jfail(p, "unexpected character");
    return false;
}

const tmj_val *tmj_parse(tm_arena *ar, const char *text, size_t len, char *err, size_t err_cap) {
    jp p = {ar, text, len, 0, err, err_cap, false};
    if (err != NULL && err_cap > 0)
        err[0] = '\0';
    /* A UTF-8 byte order mark is allowed before the value. */
    if (len >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF)
        p.i = 3;
    tmj_val *root = (tmj_val *)tm_alloc(ar, sizeof *root, 8);
    if (root == NULL) {
        jfail(&p, "out of memory");
        return NULL;
    }
    if (!value(&p, root, 0))
        return NULL;
    ws(&p);
    if (p.i != p.n) {
        jfail(&p, "trailing characters after the value");
        return NULL;
    }
    return root;
}

const tmj_val *tmj_get(const tmj_val *obj, const char *key) {
    if (obj == NULL || obj->kind != TMJ_OBJ)
        return NULL;
    size_t n = strlen(key);
    const tmj_val *found = NULL;
    for (u32 i = 0; i < obj->count; ++i) {
        const tmj_member *m = &obj->u.members[i];
        if (m->key_len == n && memcmp(m->key, key, n) == 0)
            found = &m->val;
    }
    return found;
}

f64 tmj_num(const tmj_val *obj, const char *key, f64 fallback) {
    const tmj_val *v = tmj_get(obj, key);
    return v != NULL && v->kind == TMJ_NUM ? v->u.n : fallback;
}

const char *tmj_str(const tmj_val *obj, const char *key, const char *fallback) {
    const tmj_val *v = tmj_get(obj, key);
    return v != NULL && v->kind == TMJ_STR ? v->u.s : fallback;
}

bool tmj_bool(const tmj_val *obj, const char *key, bool fallback) {
    const tmj_val *v = tmj_get(obj, key);
    return v != NULL && v->kind == TMJ_BOOL ? v->u.b : fallback;
}

const tmj_val *tmj_arr(const tmj_val *obj, const char *key) {
    const tmj_val *v = tmj_get(obj, key);
    return v != NULL && v->kind == TMJ_ARR ? v : NULL;
}
