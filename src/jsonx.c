#define JSMN_STATIC
#include "jsmn.h"

#include "jsonx.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>

static jsmntok_t g_toks[LAMBDA_JSON_TOKENS];

int jdoc_parse(jdoc *d, const char *js, size_t len)
{
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, js, len, g_toks, LAMBDA_JSON_TOKENS);
    if (n < 1 || g_toks[0].type != JSMN_OBJECT)
        return -1;
    d->js = js;
    d->toks = g_toks;
    d->n = n;
    return 0;
}

/* index just past token ix's subtree */
static int tok_skip(const jdoc *d, int ix)
{
    int j = ix + 1;
    for (int k = 0; k < d->toks[ix].size; k++)
        j = tok_skip(d, j);
    return j;
}

int j_get(const jdoc *d, int obj, const char *name)
{
    if (obj < 0 || obj >= d->n || d->toks[obj].type != JSMN_OBJECT)
        return -1;
    size_t nlen = strlen(name);
    int j = obj + 1;
    for (int k = 0; k < d->toks[obj].size; k++) {
        const jsmntok_t *key = &d->toks[j];
        if (key->type == JSMN_STRING &&
            (size_t)(key->end - key->start) == nlen &&
            memcmp(d->js + key->start, name, nlen) == 0)
            return j + 1;
        j = tok_skip(d, j);
    }
    return -1;
}

int j_is(const jdoc *d, int ix, const char *lit)
{
    if (ix < 0 || ix >= d->n || d->toks[ix].type != JSMN_STRING)
        return 0;
    size_t llen = strlen(lit);
    return (size_t)(d->toks[ix].end - d->toks[ix].start) == llen &&
           memcmp(d->js + d->toks[ix].start, lit, llen) == 0;
}

static unsigned read_hex4(const char *s, unsigned *cp)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9')
            v = v * 16 + (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v = v * 16 + (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v = v * 16 + (unsigned)(c - 'A' + 10);
        else
            return 0;
    }
    *cp = v;
    return 1;
}

static void put_utf8(buf *out, unsigned cp)
{
    char u[4];
    if (cp < 0x80) {
        u[0] = (char)cp;
        buf_append(out, u, 1);
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        buf_append(out, u, 2);
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        buf_append(out, u, 3);
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        buf_append(out, u, 4);
    }
}

int j_str_tok(const jdoc *d, int ix, buf *out)
{
    buf_reset(out);
    if (ix < 0 || ix >= d->n || d->toks[ix].type != JSMN_STRING)
        return -1;
    const char *s = d->js + d->toks[ix].start;
    const char *e = d->js + d->toks[ix].end;
    while (s < e) {
        if (*s != '\\') {
            const char *run = s;
            while (s < e && *s != '\\')
                s++;
            buf_append(out, run, (size_t)(s - run));
            continue;
        }
        if (++s >= e)
            break;
        switch (*s) {
        case 'n': buf_append(out, "\n", 1); s++; break;
        case 't': buf_append(out, "\t", 1); s++; break;
        case 'r': buf_append(out, "\r", 1); s++; break;
        case 'b': buf_append(out, "\b", 1); s++; break;
        case 'f': buf_append(out, "\f", 1); s++; break;
        case 'u': {
            unsigned cp;
            if (e - s < 5 || !read_hex4(s + 1, &cp)) {
                s++;
                break;
            }
            s += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF && e - s >= 6 && s[0] == '\\' &&
                s[1] == 'u') {
                unsigned lo;
                if (read_hex4(s + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s += 6;
                }
            }
            put_utf8(out, cp);
            break;
        }
        default: /* covers \" \\ \/ */
            buf_append(out, s, 1);
            s++;
        }
    }
    return 0;
}

int j_str(const jdoc *d, int obj, const char *name, buf *out)
{
    return j_str_tok(d, j_get(d, obj, name), out);
}

long j_long(const jdoc *d, int obj, const char *name, long dflt)
{
    int ix = j_get(d, obj, name);
    if (ix < 0 || d->toks[ix].type != JSMN_PRIMITIVE)
        return dflt;
    return strtol(d->js + d->toks[ix].start, NULL, 10);
}

int j_raw(const jdoc *d, int ix, const char **p, size_t *len)
{
    if (ix < 0 || ix >= d->n)
        return -1;
    *p = d->js + d->toks[ix].start;
    *len = (size_t)(d->toks[ix].end - d->toks[ix].start);
    return 0;
}

int j_count(const jdoc *d, int ix)
{
    if (ix < 0 || ix >= d->n)
        return 0;
    return d->toks[ix].size;
}

int j_elem(const jdoc *d, int arr, int i)
{
    if (arr < 0 || arr >= d->n || d->toks[arr].type != JSMN_ARRAY)
        return -1;
    if (i < 0 || i >= d->toks[arr].size)
        return -1;
    int j = arr + 1;
    for (int k = 0; k < i; k++)
        j = tok_skip(d, j);
    return j;
}
