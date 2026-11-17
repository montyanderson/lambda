#include <assert.h>
#include "../src/jsonx.c"
#include "../plugins/exa.c"
void plugin_register(const lambda_tool *t) { (void)t; }
int plugin_https_post(const char *host, const char *path, const char *headers,
                      const char *body, buf *out, int *status, char *err, size_t cap)
{
    (void)host; (void)path; (void)headers; (void)body; (void)out;
    (void)status; (void)err; (void)cap;
    assert(0); return -1;
}

int main(void)
{
    const char *input = "  price:\n £123 ✓ 中文 😀 rest";
    for (size_t limit = 0; limit < strlen(input) + 5; limit++) {
        char store[128];
        buf b;
        buf_attach(&b, store, sizeof store);
        append_squeezed(&b, input, limit);
        for (size_t i = 0; i < b.len;) {
            size_t width = utf8_span(b.data + i, b.len - i);
            assert(width);
            i += width;
        }
        assert(b.len <= limit + strlen("…"));
    }
    char store[128];
    buf b;
    buf_attach(&b, store, sizeof store);
    append_squeezed(&b, "£3k-£8k", 6);
    assert(!strcmp(b.data, "£3k-…"));
    puts("exa: excerpt limits preserve complete Unicode characters: ok");
    return 0;
}
