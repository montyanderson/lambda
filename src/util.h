#ifndef LAMBDA_UTIL_H
#define LAMBDA_UTIL_H

#include <stddef.h>

/* fixed-capacity byte buffer over caller-provided storage; always
 * NUL-terminated. lambda does no dynamic allocation — storage is static. */
typedef struct {
    char *data;
    size_t len, cap;
    int overflow; /* set (sticky) when an append was truncated */
} buf;

void buf_attach(buf *b, char *storage, size_t cap);
void buf_reset(buf *b);
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
