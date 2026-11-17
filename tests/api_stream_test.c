/* Parser tests independent of TCP's unpredictable coalescing. */
#include <assert.h>
#include "../src/jsonx.c"
#include "../src/api.c"
void ui_stream_begin(int kind) { (void)kind; }
void ui_stream_delta(const char *s) { (void)s; }
void ui_stream_end(void) {}
void ui_status(const char *s) { (void)s; }

static const char stream[] =
    ": heartbeat\r\n\r\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\r\n\r\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}\n\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"reason\"}}\n\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"£ ✓ 中文 😀\"}}\n\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "data: {\"type\":\n"
    "data: \"message_stop\"}\n\n";

int main(void)
{
    static turn t;
    const char *expected = "{\"type\":\"text\",\"text\":\"£ ✓ 中文 😀\"},"
                           "{\"type\":\"thinking\",\"thinking\":\"reason\",\"signature\":\"\"}";
    for (size_t split = 0; split <= strlen(stream); split++) {
        turn_init(&t);
        feed_sse(&t, stream, split);
        feed_sse(&t, stream + split, strlen(stream) - split);
        assert(t.complete && !t.error && !t.truncated);
        assert(!strcmp(t.content.data, expected));
    }
    for (unsigned seed = 1; seed <= 200; seed++) {
        unsigned random = seed;
        turn_init(&t);
        for (size_t pos = 0; pos < strlen(stream);) {
            random = random * 1664525u + 1013904223u;
            size_t n = seed == 1 ? 1 : 1 + (random >> 24);
            if (n > strlen(stream) - pos) n = strlen(stream) - pos;
            feed_sse(&t, stream + pos, n);
            pos += n;
        }
        assert(t.complete && !t.error && !strcmp(t.content.data, expected));
    }
    static char oversized[LAMBDA_SSE_PENDING_MAX + 8];
    memset(oversized, 'x', sizeof oversized);
    turn_init(&t);
    feed_sse(&t, oversized, sizeof oversized);
    assert(t.error && !t.complete);
    puts("SSE: every split point, byte-by-byte and 200 random chunk schedules, overlapping blocks, overflow: ok");
    return 0;
}
