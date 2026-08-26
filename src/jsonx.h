#ifndef LAMBDA_JSONX_H
#define LAMBDA_JSONX_H

#include <stddef.h>

#include "util.h"

#define JSMN_HEADER
#include "jsmn.h"

/* thin helpers over jsmn's token array. All functions accept -1 token
 * indices and propagate them, so lookups can be chained without checks. */
typedef struct {
    const char *js;
    const jsmntok_t *toks;
    int n;
} jdoc;

/* parse into a static token pool (single-threaded, one live jdoc at a time);
 * 0 on success with d->toks pointing at the pool */
int jdoc_parse(jdoc *d, const char *js, size_t len);

/* token index of obj's property `name`, or -1 */
int j_get(const jdoc *d, int obj, const char *name);
/* 1 if token ix is the string `lit` (raw compare; fine for enum values) */
int j_is(const jdoc *d, int ix, const char *lit);
/* decode string token (JSON escapes incl. \uXXXX -> utf-8) into out;
 * out is reset first. 0 ok, -1 if absent/not a string */
int j_str_tok(const jdoc *d, int ix, buf *out);
int j_str(const jdoc *d, int obj, const char *name, buf *out);
long j_long(const jdoc *d, int obj, const char *name, long dflt);
/* raw (still-escaped) bytes of any token, e.g. to replay json verbatim */
int j_raw(const jdoc *d, int ix, const char **p, size_t *len);

/* array/object element count, and the i'th element of an array */
int j_count(const jdoc *d, int ix);
int j_elem(const jdoc *d, int arr, int i);

#endif
