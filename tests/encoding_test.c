/* Exercise the JSON boundary with the bytes tools actually return. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/jsonx.c"

static void encode(const char *input, size_t len, const char *expected)
{
    char storage[2048];
    buf b;
    buf_attach(&b, storage, sizeof storage);
    assert(buf_append_json_bytes(&b, input, len) == 0);
    assert(strcmp(b.data, expected) == 0);
}

int main(void)
{
    encode("£3k-\xc2…", 9, "\"£3k-�…\"");
    encode("£ ✓ 中文 😀", strlen("£ ✓ 中文 😀"), "\"£ ✓ 中文 😀\"");
    encode("\0\n\r\t\"\\", 6, "\"\\u0000\\n\\r\\t\\\"\\\\\"");
    encode("\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80\xff", 10,
           "\"����������\"");
    const char *valid[] = {"£", "✓", "😀"};
    for (size_t i = 0; i < sizeof valid / sizeof *valid; i++) {
        for (size_t n = 1; n < strlen(valid[i]); n++) {
            char expected[32] = "\"";
            for (size_t j = 0; j < n; j++) strcat(expected, "�");
            strcat(expected, "\"");
            encode(valid[i], n, expected);
        }
    }
    char decoded[64];
    buf b;
    buf_attach(&b, decoded, sizeof decoded);
    jdoc d;
    const char *json = "{\"s\":\"\\ud800x\\udc00\\ud83d\\ude00\"}";
    assert(jdoc_parse(&d, json, strlen(json)) == 0);
    assert(j_str(&d, 0, "s", &b) == 0);
    assert(strcmp(b.data, "�x�😀") == 0);
    const char *invalid[] = {
        "{} {}", "{\"a\":1 \"b\":2}", "{\"a\":trueish}", "{\"a\":01}",
        "{\"a\":1.}", "{\"a\":1e}", "{\"a\":NaN}", "{\"a\":Infinity}",
        "{\"a\": [1,]}", "{\"a\":\"raw\nnewline\"}", "{\"a\":\"\\x00\"}",
        "{\"a\":\"\\u12z4\"}", "{\"a\":false,}", "{\"a\":}", "[]"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
        assert(jdoc_parse(&d, invalid[i], strlen(invalid[i])) != 0);
    const char *numbers = "{\"a\":1.5,\"b\":1e2,\"c\":999999999999999999999999,\"d\":-3}";
    assert(jdoc_parse(&d, numbers, strlen(numbers)) == 0);
    assert(j_long(&d, 0, "a", 7) == 7 && j_long(&d, 0, "b", 7) == 7);
    assert(j_long(&d, 0, "c", 7) == 7 && j_long(&d, 0, "d", 7) == -3);
    /* Every single byte, plus deterministic arbitrary binary buffers. */
    unsigned seed = 19;
    for (int trial = 0; trial < 1024; trial++) {
        char input[64], output[512];
        size_t len = trial < 256 ? 1 : sizeof input;
        for (size_t i = 0; i < len; i++) {
            seed = seed * 1664525u + 1013904223u;
            input[i] = trial < 256 ? (char)trial : (char)(seed >> 24);
        }
        buf_attach(&b, output, sizeof output);
        buf_appends(&b, "{\"s\":");
        assert(buf_append_json_bytes(&b, input, len) == 0);
        buf_appends(&b, "}");
        assert(jdoc_parse(&d, b.data, b.len) == 0);
        for (size_t i = 0; i < b.len;) {
            size_t n = utf8_span(b.data + i, b.len - i);
            assert(n);
            i += n;
        }
    }
    /* Zero-capacity buffers and worst-case escaping must fail safely. */
    char guard = 'x';
    buf_attach(&b, &guard, 0);
    assert(buf_appends(&b, "a") == -1 && guard == 'x');
    for (size_t cap = 1; cap < 16; cap++) {
        char small[16];
        memset(small, 'x', sizeof small);
        buf_attach(&b, small, cap);
        buf_append_json_bytes(&b, "\x01\xff\n", 3);
        assert(b.len < cap && small[b.len] == '\0');
        for (size_t i = cap; i < sizeof small; i++) assert(small[i] == 'x');
    }
    puts("encoding: tool bytes, Unicode boundaries, surrogates, controls, buffer limits: ok");
    return 0;
}
