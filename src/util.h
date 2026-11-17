#ifndef LAMBDA_UTIL_H
#define LAMBDA_UTIL_H

#include <stddef.h>

/* Checked, geometrically growing storage. Capacity is in elements. Existing
 * storage and capacity are unchanged on failure; new bytes are uninitialised. */
int storage_reserve(void **data, size_t *capacity, size_t need, size_t element);

/* NUL-terminated byte buffer. buf_attach uses fixed caller-owned storage;
 * a zero-initialised buf with .grow = 1 owns growable storage. */
typedef struct {
    char *data;
    size_t len, cap;
    int overflow; /* set (sticky) when an append was truncated */
    int grow;
} buf;

void buf_attach(buf *b, char *storage, size_t cap);
void buf_reset(buf *b);
int buf_reserve(buf *b, size_t capacity);
void buf_free(buf *b);
int buf_append(buf *b, const char *data, size_t len); /* 0 ok, -1 truncated */
int buf_appends(buf *b, const char *s);
int buf_appendf(buf *b, const char *fmt, ...);
/* Length of one valid UTF-8 scalar, or 0 for an invalid/incomplete sequence. */
size_t utf8_span(const char *s, size_t len);
/* Copy complete text, replacing invalid bytes with U+FFFD. Not for chunks. */
int buf_append_utf8(buf *b, const char *s, size_t len);
/* append s as a JSON string literal, including surrounding quotes */
int buf_append_json_str(buf *b, const char *s);
int buf_append_json_bytes(buf *b, const char *s, size_t len);
/* same, but concatenating two pieces into one literal (blank-line separated
 * when both are non-empty) */
int buf_append_json_str2(buf *b, const char *a, const char *c);

#endif
