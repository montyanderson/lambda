#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

int storage_reserve(void **data, size_t *capacity, size_t need, size_t element)
{
    if (!element || need > (size_t)PTRDIFF_MAX / element) {
        errno = ENOMEM;
        return -1;
    }
    if (need <= *capacity) return 0;
    size_t cap = *capacity ? *capacity : 16;
    if (cap > (size_t)PTRDIFF_MAX / element) cap = need;
    while (cap < need) {
        if (cap > (size_t)PTRDIFF_MAX / element / 2) { cap = need; break; }
        cap *= 2;
    }
    void *p = realloc(*data, cap * element);
    if (!p) return -1;
    *data = p;
    *capacity = cap;
    return 0;
}

void buf_attach(buf *b, char *storage, size_t cap)
{
    b->data = storage;
    b->cap = cap;
    b->len = 0;
    b->overflow = 0;
    b->grow = 0;
    if (cap > 0)
        b->data[0] = '\0';
}

void buf_reset(buf *b)
{
    b->len = 0;
    b->overflow = 0;
    if (b->cap > 0)
        b->data[0] = '\0';
}

int buf_reserve(buf *b, size_t capacity)
{
    if (capacity <= b->cap) return 0;
    void *p = b->data;
    if (!b->grow || storage_reserve(&p, &b->cap, capacity, 1) != 0) {
        b->overflow = 1;
        return -1;
    }
    b->data = p;
    b->data[b->len] = '\0';
    return 0;
}

void buf_free(buf *b)
{
    if (b->grow) free(b->data);
    memset(b, 0, sizeof *b);
}

int buf_append(buf *b, const char *data, size_t len)
{
    /* Preserve an internal source offset if growth moves this buffer. */
    uintptr_t source = (uintptr_t)data, base = (uintptr_t)b->data;
    int internal = b->data && source >= base && source - base < b->cap;
    size_t offset = internal ? (size_t)(source - base) : 0;
    if (b->grow && (len >= SIZE_MAX - b->len ||
        buf_reserve(b, b->len + len + 1) != 0)) {
        b->overflow = 1;
        return -1;
    }
    if (internal) data = b->data + offset;
    size_t room = b->cap > b->len + 1 ? b->cap - b->len - 1 : 0;
    size_t take = len < room ? len : room;
    if (take)
        memmove(b->data + b->len, data, take);
    b->len += take;
    if (b->cap)
        b->data[b->len] = '\0';
    if (take < len) {
        b->overflow = 1;
        return -1;
    }
    return 0;
}

int buf_appends(buf *b, const char *s)
{
    return buf_append(b, s, strlen(s));
}

int buf_appendf(buf *b, const char *fmt, ...)
{
    va_list ap;
    if (b->grow) {
        va_start(ap, fmt);
        int needed = vsnprintf(NULL, 0, fmt, ap);
        va_end(ap);
        if (needed < 0 || (size_t)needed >= SIZE_MAX - b->len ||
            buf_reserve(b, b->len + (size_t)needed + 1) != 0) {
            b->overflow = 1;
            return -1;
        }
    }
    size_t room = b->cap > b->len ? b->cap - b->len : 0;
    va_start(ap, fmt);
    int n = vsnprintf(room ? b->data + b->len : NULL, room, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n >= room) {
        b->len = b->cap > 0 ? b->cap - 1 : 0;
        b->overflow = 1;
        return -1;
    }
    b->len += (size_t)n;
    return 0;
}

size_t utf8_span(const char *s, size_t len)
{
    if (!len)
        return 0;
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80)
        return 1;
    size_t n = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 :
               p[0] >= 0xe0 && p[0] <= 0xef ? 3 :
               p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!n || len < n)
        return 0;
    for (size_t i = 1; i < n; i++)
        if (p[i] < 0x80 || p[i] > 0xbf)
            return 0;
    if ((p[0] == 0xe0 && p[1] < 0xa0) ||
        (p[0] == 0xed && p[1] >= 0xa0) ||
        (p[0] == 0xf0 && p[1] < 0x90) ||
        (p[0] == 0xf4 && p[1] >= 0x90))
        return 0; /* overlong, surrogate, or beyond U+10FFFF */
    return n;
}

int buf_append_utf8(buf *b, const char *s, size_t len)
{
    int rc = 0;
    for (size_t i = 0; i < len;) {
        size_t n = utf8_span(s + i, len - i);
        rc |= n ? buf_append(b, s + i, n) : buf_appends(b, "�");
        i += n ? n : 1;
    }
    return rc ? -1 : 0;
}

/* Validate at the JSON boundary, after all tool/stream chunks are joined.
 * Tool output is arbitrary bytes, and byte limits can bisect a character. */
static int json_str_body(buf *b, const char *s, size_t len)
{
    if (!len) return 0;
    int rc = 0;
    for (const unsigned char *p = (const unsigned char *)s;
         p < (const unsigned char *)s + len; p++) {
        switch (*p) {
        case '"':  rc |= buf_appends(b, "\\\""); break;
        case '\\': rc |= buf_appends(b, "\\\\"); break;
        case '\n': rc |= buf_appends(b, "\\n"); break;
        case '\r': rc |= buf_appends(b, "\\r"); break;
        case '\t': rc |= buf_appends(b, "\\t"); break;
        default:
            if (*p < 0x20)
                rc |= buf_appendf(b, "\\u%04x", *p);
            else {
                size_t n = utf8_span((const char *)p,
                                    (size_t)((const unsigned char *)s + len - p));
                rc |= n ? buf_append(b, (const char *)p, n) : buf_appends(b, "�");
                if (n)
                    p += n - 1;
            }
        }
    }
    return rc;
}

int buf_append_json_str(buf *b, const char *s)
{
    return buf_append_json_bytes(b, s, strlen(s));
}

int buf_append_json_bytes(buf *b, const char *s, size_t len)
{
    int rc = buf_append(b, "\"", 1);
    rc |= json_str_body(b, s, len);
    rc |= buf_append(b, "\"", 1);
    return rc ? -1 : 0;
}

int buf_append_json_str2(buf *b, const char *a, const char *c)
{
    int rc = buf_append(b, "\"", 1);
    rc |= json_str_body(b, a, strlen(a));
    if (*a && *c)
        rc |= buf_appends(b, "\\n\\n");
    rc |= json_str_body(b, c, strlen(c));
    rc |= buf_append(b, "\"", 1);
    return rc ? -1 : 0;
}
