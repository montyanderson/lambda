/* Full archive retention is independent of the model's active context. */
#include <assert.h>
#include "../src/jsonx.c"
#include "../src/api.c"

int gateway_enabled(void) { return 0; }
void gateway_reasoning(const char *id, reasoning_options *out) { (void)id; memset(out, 0, sizeof *out); }
const model_info *gateway_model_info(const char *id) { (void)id; return NULL; }
int gateway_model_count(void) { return 0; }
const model_info *gateway_model_at(int i) { (void)i; return NULL; }
int ui_reserve(size_t bytes) { (void)bytes; return 0; }
void tools_emit_json(buf *b, int enabled) { (void)enabled; buf_appends(b, "[]"); }

int main(void)
{
    chat *c = chat_get();
    chat_init(c);
    assert(chat_prepare_storage(c) == 0);
    size_t reserved = chat_history_capacity(c);
    assert(reserved >= 1000000u * 8);
    buf msg = {.grow = 1};
    char padding[16385];
    memset(padding, 'a', sizeof padding - 1);
    padding[sizeof padding - 1] = '\0';
    for (int i = 0; i < 1200; i++) {
        buf_reset(&msg);
        buf_appendf(&msg, "{\"role\":\"user\",\"content\":\"entry-%d %s\"}", i, padding);
        assert(chat_push_raw(c, msg.data, msg.len) == 0);
    }
    assert(chat_history_count(c) == 1200 && chat_msg_count(c) == 1200);
    assert(chat_history_bytes(c) > (16u << 20));
    size_t bytes = chat_history_bytes(c), cap = chat_history_capacity(c), len;
    assert(strncmp(chat_history_msg(c, 0, &len), "{\"role\":\"user\",\"content\":\"entry-0 ", 33) == 0);
    assert(cap > reserved);
    /* A request also grows past the former 16 MiB ceiling. */
    buf body = {.grow = 1};
    build_body(c, &body, 0);
    assert(!body.overflow && body.len > (16u << 20));
    assert(strstr(body.data, "entry-0 ") && strstr(body.data, "entry-1199 "));

    assert(chat_restore_compact(c, 1190, 0, "summary") == 0);
    assert(chat_msg_count(c) == 11);
    assert(chat_history_count(c) == 1200 && chat_history_bytes(c) == bytes);
    assert(c->msgs[0].off == c->history[0].off);
    assert(c->msgs[1].off == c->history[1190].off);
    assert(chat_restore_compact(c, 5, 0, "new summary") == 0);
    assert(chat_msg_count(c) == 7 && chat_history_count(c) == 1200);
    /* Summary fragments follow active order, including a pinned old prompt. */
    buf fragment = {.grow = 1};
    active_slice(c, 2, 0, 100000, &fragment);
    assert(strstr(fragment.data, "entry-0 ") && strstr(fragment.data, "entry-1194 "));
    assert(!strstr(fragment.data, "entry-1 "));

    const char *pending = "{\"role\":\"user\",\"content\":\"rolled-back\"}";
    assert(chat_push_raw(c, pending, strlen(pending)) == 0);
    chat_drop(c, 1);
    assert(chat_history_count(c) == 1200 && chat_history_bytes(c) == bytes);
    assert(chat_msg_count(c) == 7);
    chat_set_model(c, "claude-haiku-4-5");
    assert(chat_prepare_storage(c) == 0);
    assert(chat_history_capacity(c) == cap);
    assert(chat_history_count(c) == 1200);
    chat_clear(c);
    assert(!chat_history_count(c) && !chat_msg_count(c) && !chat_history_bytes(c));
    assert(!chat_summary(c)[0]);
    chat_destroy(c);
    buf_free(&msg); buf_free(&body); buf_free(&fragment);
    puts("history: >16 MiB, 1200 messages, compaction/pinning, rollback, model switch and clear: ok");
    return 0;
}
