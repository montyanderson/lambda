#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void buf_attach(buf *b, char *storage, size_t cap)
{
    b->data = storage;
    b->cap = cap;
    b->len = 0;
    b->overflow = 0;
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

int buf_append(buf *b, const char *data, size_t len)
{
    size_t room = b->cap > b->len + 1 ? b->cap - b->len - 1 : 0;
    size_t take = len < room ? len : room;
    memcpy(b->data + b->len, data, take);
    b->len += take;
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
    size_t room = b->cap > b->len ? b->cap - b->len : 0;
    va_start(ap, fmt);
    int n = vsnprintf(b->data + b->len, room, fmt, ap);
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

/* body of a JSON string literal, without the surrounding quotes */
static int json_str_body(buf *b, const char *s)
{
    int rc = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  rc |= buf_appends(b, "\\\""); break;
        case '\\': rc |= buf_appends(b, "\\\\"); break;
        case '\n': rc |= buf_appends(b, "\\n"); break;
        case '\r': rc |= buf_appends(b, "\\r"); break;
        case '\t': rc |= buf_appends(b, "\\t"); break;
        default:
            if (*p < 0x20)
                rc |= buf_appendf(b, "\\u%04x", *p);
            else
                rc |= buf_append(b, (const char *)p, 1);
        }
    }
    return rc;
}

int buf_append_json_str(buf *b, const char *s)
{
    int rc = buf_append(b, "\"", 1);
    rc |= json_str_body(b, s);
    rc |= buf_append(b, "\"", 1);
    return rc ? -1 : 0;
}

int buf_append_json_str2(buf *b, const char *a, const char *c)
{
    int rc = buf_append(b, "\"", 1);
    rc |= json_str_body(b, a);
    if (*a && *c)
        rc |= buf_appends(b, "\\n\\n");
    rc |= json_str_body(b, c);
    rc |= buf_append(b, "\"", 1);
    return rc ? -1 : 0;
}
