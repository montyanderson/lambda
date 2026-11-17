#include "util.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    buf b = {.grow = 1};
    assert(buf_appends(&b, "first") == 0);
    for (int i = 0; i < 17; i++) {
        size_t old = b.len;
        assert(buf_append(&b, b.data, old) == 0);
        assert(b.len == old * 2 && !memcmp(b.data, b.data + old, old));
    }
    char *old = b.data;
    size_t len = b.len, cap = b.cap;
    assert(buf_reserve(&b, SIZE_MAX) == -1);
    assert(b.overflow && b.data == old && b.len == len && b.cap == cap);
    assert(buf_append(&b, "x", SIZE_MAX) == -1);
    assert(b.data == old && b.len == len && !memcmp(b.data, "first", 5));
    buf_reset(&b);
    assert(buf_appendf(&b, "%s %d", "retry", 42) == 0);
    assert(!strcmp(b.data, "retry 42") && !b.overflow);
    buf_reset(&b);
    assert(buf_append_json_bytes(&b, NULL, 0) == 0 && !strcmp(b.data, "\"\""));
    buf_free(&b);
    char fixed[4];
    buf_attach(&b, fixed, sizeof fixed);
    assert(buf_appends(&b, "long") == -1 && b.overflow && !strcmp(fixed, "lon"));
    buf_free(&b); /* caller storage is never freed */
    puts("storage: growth, overlapping append, overflow preserves contents, retry and fixed buffers: ok");
    return 0;
}
