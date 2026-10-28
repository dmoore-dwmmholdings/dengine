/*
 * tilemap_xml.c: the XML reader of the tilemap importer (DENG-80), for the subset Tiled's TMX and
 * TSX files use. Elements, attributes, character data, CDATA, comments, processing instructions
 * and a DOCTYPE (both skipped), the predefined entities and numeric character references. No
 * namespaces, no DTD entities. Recursion is bounded by TMX_MAX_DEPTH; every byte comes from the
 * import arena.
 */
#include "cook/importers/tilemap/tilemap_ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct xp {
    tm_arena *ar;
    const char *s;
    size_t n, i;
    char *err;
    size_t err_cap;
    bool failed;
} xp;

static void xfail(xp *p, const char *msg) {
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

static bool starts(const xp *p, const char *w) {
    size_t m = strlen(w);
    return p->n - p->i >= m && memcmp(p->s + p->i, w, m) == 0;
}

static void ws(xp *p) {
    while (p->i < p->n &&
           (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r'))
        ++p->i;
}

/* Skips to just after `end`. */
static bool skip_to(xp *p, const char *end) {
    size_t m = strlen(end);
    while (p->n - p->i >= m) {
        if (memcmp(p->s + p->i, end, m) == 0) {
            p->i += m;
            return true;
        }
        ++p->i;
    }
    xfail(p, "unterminated markup");
    return false;
}

static bool name_char(char c, bool first) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' ||
        (unsigned char)c >= 0x80)
        return true;
    return !first && ((c >= '0' && c <= '9') || c == '-' || c == '.');
}

static const char *name(xp *p) {
    size_t start = p->i;
    if (p->i >= p->n || !name_char(p->s[p->i], true)) {
        xfail(p, "expected a name");
        return NULL;
    }
    while (p->i < p->n && name_char(p->s[p->i], false))
        ++p->i;
    char *s = tm_strndup(p->ar, p->s + start, p->i - start);
    if (s == NULL)
        xfail(p, "out of memory");
    return s;
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

/* An entity at p->s[p->i] == '&': appends its expansion to `out`. */
static bool entity(xp *p, tm_vec *out) {
    size_t semi = p->i + 1;
    while (semi < p->n && semi < p->i + 12 && p->s[semi] != ';')
        ++semi;
    if (semi >= p->n || p->s[semi] != ';') {
        xfail(p, "bad entity reference");
        return false;
    }
    const char *e = p->s + p->i + 1;
    size_t m = semi - p->i - 1;
    char buf[4];
    size_t len = 0;
    if (m == 2 && memcmp(e, "lt", 2) == 0)
        buf[len++] = '<';
    else if (m == 2 && memcmp(e, "gt", 2) == 0)
        buf[len++] = '>';
    else if (m == 3 && memcmp(e, "amp", 3) == 0)
        buf[len++] = '&';
    else if (m == 4 && memcmp(e, "quot", 4) == 0)
        buf[len++] = '"';
    else if (m == 4 && memcmp(e, "apos", 4) == 0)
        buf[len++] = '\'';
    else if (m >= 2 && e[0] == '#') {
        u32 cp = 0;
        bool hex = e[1] == 'x';
        size_t k = hex ? 2 : 1;
        if (k >= m) {
            xfail(p, "bad character reference");
            return false;
        }
        for (; k < m; ++k) {
            char c = e[k];
            u32 d;
            if (c >= '0' && c <= '9')
                d = (u32)(c - '0');
            else if (hex && c >= 'a' && c <= 'f')
                d = (u32)(c - 'a' + 10);
            else if (hex && c >= 'A' && c <= 'F')
                d = (u32)(c - 'A' + 10);
            else {
                xfail(p, "bad character reference");
                return false;
            }
            cp = cp * (hex ? 16u : 10u) + d;
            if (cp > 0x10FFFF) {
                xfail(p, "bad character reference");
                return false;
            }
        }
        if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            xfail(p, "bad character reference");
            return false;
        }
        len = utf8(cp, buf);
    } else {
        xfail(p, "unknown entity");
        return false;
    }
    for (size_t k = 0; k < len; ++k)
        if (tm_vec_push(p->ar, out, &buf[k]) == NULL) {
            xfail(p, "out of memory");
            return false;
        }
    p->i = semi + 1;
    return true;
}

static bool attr_value(xp *p, const char **out) {
    if (p->i >= p->n || (p->s[p->i] != '"' && p->s[p->i] != '\'')) {
        xfail(p, "expected a quoted attribute value");
        return false;
    }
    char q = p->s[p->i++];
    tm_vec v;
    tm_vec_init(&v, 1);
    while (p->i < p->n && p->s[p->i] != q) {
        char c = p->s[p->i];
        if (c == '<') {
            xfail(p, "'<' in attribute value");
            return false;
        }
        if (c == '&') {
            if (!entity(p, &v))
                return false;
            continue;
        }
        if (tm_vec_push(p->ar, &v, &c) == NULL) {
            xfail(p, "out of memory");
            return false;
        }
        ++p->i;
    }
    if (p->i >= p->n) {
        xfail(p, "unterminated attribute value");
        return false;
    }
    ++p->i;
    char *s = tm_strndup(p->ar, v.count ? (const char *)v.data : "", v.count);
    if (s == NULL) {
        xfail(p, "out of memory");
        return false;
    }
    *out = s;
    return true;
}

/* Skips comments, processing instructions and a DOCTYPE between elements. */
static bool misc(xp *p) {
    for (;;) {
        ws(p);
        if (starts(p, "<!--")) {
            p->i += 4;
            if (!skip_to(p, "-->"))
                return false;
        } else if (starts(p, "<?")) {
            p->i += 2;
            if (!skip_to(p, "?>"))
                return false;
        } else if (starts(p, "<!DOCTYPE")) {
            /* Skip to the closing '>' outside an internal subset. */
            int bracket = 0;
            while (p->i < p->n) {
                char c = p->s[p->i++];
                if (c == '[')
                    ++bracket;
                else if (c == ']')
                    --bracket;
                else if (c == '>' && bracket <= 0)
                    break;
            }
        } else {
            return true;
        }
    }
}

static bool element(xp *p, tmx_node *node, u32 depth) {
    if (depth > TMX_MAX_DEPTH) {
        xfail(p, "nesting too deep");
        return false;
    }
    ++p->i; /* '<' */
    node->name = name(p);
    if (node->name == NULL)
        return false;
    tm_vec attrs;
    tm_vec_init(&attrs, sizeof(tmx_attr));
    for (;;) {
        size_t before = p->i;
        ws(p);
        if (p->i >= p->n) {
            xfail(p, "unterminated start tag");
            return false;
        }
        char c = p->s[p->i];
        if (c == '/' || c == '>')
            break;
        if (p->i == before) {
            xfail(p, "expected whitespace before an attribute");
            return false;
        }
        tmx_attr *a = (tmx_attr *)tm_vec_push(p->ar, &attrs, NULL);
        if (a == NULL) {
            xfail(p, "out of memory");
            return false;
        }
        a->name = name(p);
        if (a->name == NULL)
            return false;
        ws(p);
        if (p->i >= p->n || p->s[p->i] != '=') {
            xfail(p, "expected '=' after an attribute name");
            return false;
        }
        ++p->i;
        ws(p);
        if (!attr_value(p, &a->value))
            return false;
    }
    node->attrs = (tmx_attr *)attrs.data;
    node->attr_count = attrs.count;
    node->text = "";
    if (p->s[p->i] == '/') {
        ++p->i;
        if (p->i >= p->n || p->s[p->i] != '>') {
            xfail(p, "expected '>' after '/'");
            return false;
        }
        ++p->i;
        return true;
    }
    ++p->i; /* '>' */
    tm_vec kids, text;
    tm_vec_init(&kids, sizeof(tmx_node));
    tm_vec_init(&text, 1);
    for (;;) {
        if (p->i >= p->n) {
            xfail(p, "unterminated element");
            return false;
        }
        char c = p->s[p->i];
        if (c == '<') {
            if (starts(p, "</")) {
                p->i += 2;
                const char *end = name(p);
                if (end == NULL)
                    return false;
                if (strcmp(end, node->name) != 0) {
                    xfail(p, "mismatched end tag");
                    return false;
                }
                ws(p);
                if (p->i >= p->n || p->s[p->i] != '>') {
                    xfail(p, "expected '>' in end tag");
                    return false;
                }
                ++p->i;
                break;
            }
            if (starts(p, "<!--")) {
                p->i += 4;
                if (!skip_to(p, "-->"))
                    return false;
                continue;
            }
            if (starts(p, "<?")) {
                p->i += 2;
                if (!skip_to(p, "?>"))
                    return false;
                continue;
            }
            if (starts(p, "<![CDATA[")) {
                p->i += 9;
                size_t start = p->i;
                if (!skip_to(p, "]]>"))
                    return false;
                for (size_t k = start; k < p->i - 3; ++k)
                    if (tm_vec_push(p->ar, &text, &p->s[k]) == NULL) {
                        xfail(p, "out of memory");
                        return false;
                    }
                continue;
            }
            tmx_node *kid = (tmx_node *)tm_vec_push(p->ar, &kids, NULL);
            if (kid == NULL) {
                xfail(p, "out of memory");
                return false;
            }
            if (!element(p, kid, depth + 1))
                return false;
            continue;
        }
        if (c == '&') {
            if (!entity(p, &text))
                return false;
            continue;
        }
        if (tm_vec_push(p->ar, &text, &c) == NULL) {
            xfail(p, "out of memory");
            return false;
        }
        ++p->i;
    }
    node->children = (tmx_node *)kids.data;
    node->child_count = kids.count;
    char *t = tm_strndup(p->ar, text.count ? (const char *)text.data : "", text.count);
    if (t == NULL) {
        xfail(p, "out of memory");
        return false;
    }
    node->text = t;
    node->text_len = text.count;
    return true;
}

const tmx_node *tmx_parse(tm_arena *ar, const char *text, size_t len, char *err, size_t err_cap) {
    xp p = {ar, text, len, 0, err, err_cap, false};
    if (err != NULL && err_cap > 0)
        err[0] = '\0';
    if (len >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF)
        p.i = 3;
    if (!misc(&p))
        return NULL;
    if (p.i >= p.n || p.s[p.i] != '<') {
        xfail(&p, "expected the root element");
        return NULL;
    }
    tmx_node *root = (tmx_node *)tm_alloc(ar, sizeof *root, 8);
    if (root == NULL) {
        xfail(&p, "out of memory");
        return NULL;
    }
    if (!element(&p, root, 0))
        return NULL;
    if (!misc(&p))
        return NULL;
    if (p.i != p.n) {
        xfail(&p, "content after the root element");
        return NULL;
    }
    return root;
}

const char *tmx_attr_str(const tmx_node *n, const char *attr, const char *fallback) {
    if (n == NULL)
        return fallback;
    for (u32 i = 0; i < n->attr_count; ++i)
        if (strcmp(n->attrs[i].name, attr) == 0)
            return n->attrs[i].value;
    return fallback;
}

f64 tmx_attr_num(const tmx_node *n, const char *attr, f64 fallback) {
    const char *s = tmx_attr_str(n, attr, NULL);
    if (s == NULL || s[0] == '\0')
        return fallback;
    char *end = NULL;
    f64 v = strtod(s, &end);
    if (end == s || *end != '\0' || !(v == v) || v - v != 0.0)
        return fallback;
    return v;
}

const tmx_node *tmx_child(const tmx_node *n, const char *child) {
    if (n == NULL)
        return NULL;
    for (u32 i = 0; i < n->child_count; ++i)
        if (strcmp(n->children[i].name, child) == 0)
            return &n->children[i];
    return NULL;
}
