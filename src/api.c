/* Anthropic Messages API over the raw wire: request building, SSE stream
 * parsing (jsmn), and the tool-use loop. All state is static — no malloc.
 *
 * History is stored as fully-formed JSON message objects so that text,
 * thinking, tool_use and tool_result blocks all round-trip verbatim. */

#include "api.h"
#include "config.h"
#include "http.h"
#include "jsonx.h"
#include "session.h"
#include "tools.h"
#include "ui.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define API_HOST "api.anthropic.com"
#define API_VERSION "2023-06-01"
#define MAX_TOKENS 64000

struct chat {
    char model[128];
    char system[LAMBDA_SYSTEM_MAX];
    const char *prelude; /* project context, emitted before `system` */
    int fallbacks;
    int tools;
    int show_thinking;
    char effort[16];

    /* history: raw JSON message objects in an arena */
    char arena[LAMBDA_HISTORY_ARENA];
    size_t used;
    struct {
        size_t off, len;
    } msgs[LAMBDA_MAX_MSGS];
    int nmsgs;
};

static struct chat g_chat;

chat *chat_get(void)
{
    return &g_chat;
}

void chat_init(chat *c)
{
    c->used = 0;
    c->nmsgs = 0;
    c->system[0] = '\0';
    c->prelude = NULL;
    c->fallbacks = 1;
    c->tools = 1;
    c->show_thinking = 1;
    c->effort[0] = '\0';
    snprintf(c->model, sizeof c->model, "%s", LAMBDA_DEFAULT_MODEL);
}

void chat_clear(chat *c)
{
    c->used = 0;
    c->nmsgs = 0;
}

const char *chat_model(const chat *c) { return c->model; }
void chat_set_model(chat *c, const char *m)
{
    snprintf(c->model, sizeof c->model, "%s", m);
}
void chat_set_system(chat *c, const char *s)
{
    snprintf(c->system, sizeof c->system, "%s", s ? s : "");
}
void chat_set_fallbacks(chat *c, int on) { c->fallbacks = on; }
void chat_set_show_thinking(chat *c, int on) { c->show_thinking = on; }
const char *chat_effort(const chat *c) { return c->effort; }
void chat_set_effort(chat *c, const char *e)
{
    snprintf(c->effort, sizeof c->effort, "%s", e ? e : "");
}
void chat_set_prelude(chat *c, const char *t)
{
    c->prelude = (t && *t) ? t : NULL;
}
int chat_msg_count(const chat *c) { return c->nmsgs; }

const char *chat_msg(const chat *c, int i, size_t *len)
{
    if (i < 0 || i >= c->nmsgs)
        return NULL;
    *len = c->msgs[i].len;
    return c->arena + c->msgs[i].off;
}
void chat_set_tools(chat *c, int on) { c->tools = on; }
int chat_tools_enabled(const chat *c) { return c->tools; }

/* append a raw JSON message object to history; 0 on success */
static int hist_push(chat *c, const char *json, size_t len)
{
    if (c->nmsgs >= LAMBDA_MAX_MSGS || c->used + len > sizeof c->arena)
        return -1;
    memcpy(c->arena + c->used, json, len);
    c->msgs[c->nmsgs].off = c->used;
    c->msgs[c->nmsgs].len = len;
    c->nmsgs++;
    c->used += len;
    return 0;
}

int chat_push_raw(chat *c, const char *json, size_t len)
{
    return hist_push(c, json, len);
}

/* fallbacks only exist on the opus-5 / fable-5 families */
static int model_has_fallbacks(const char *model)
{
    return strncmp(model, "claude-opus-5", 13) == 0 ||
           strncmp(model, "claude-fable-5", 14) == 0 ||
           strncmp(model, "claude-mythos-5", 15) == 0;
}

/* Adaptive thinking and output_config.effort exist on the 4.6-and-later
 * families. Older models reject both, so only send them where they apply. */
static int model_is_modern(const char *model)
{
    static const char *fams[] = {
        "claude-fable-5",   "claude-mythos-5",  "claude-opus-5",
        "claude-opus-4-8",  "claude-opus-4-7",  "claude-opus-4-6",
        "claude-sonnet-5",  "claude-sonnet-4-6", NULL,
    };
    for (int i = 0; fams[i]; i++)
        if (strncmp(model, fams[i], strlen(fams[i])) == 0)
            return 1;
    return 0;
}

/* ---- streamed turn state ------------------------------------------------ */

typedef struct {
    char id[64];
    char name[64];
    char args[LAMBDA_TOOL_INPUT_MAX]; /* raw tool input json */
    char cmd[LAMBDA_TOOL_INPUT_MAX];  /* bash: the extracted command */
} tool_call;

typedef struct {
    int status;

    buf pending; /* raw sse bytes not yet split into lines */
    buf data;    /* accumulated data: lines of the current event */
    buf errbody;

    /* assistant message being reconstructed, as a JSON content array */
    buf content;   /* "[{...},{...}" — closing bracket added at the end */
    int nblocks;

    /* current content block */
    int blk_type; /* 0 none, 1 text, 2 thinking, 3 tool_use */
    buf blk_text;    /* text / thinking body */
    buf blk_sig;     /* thinking signature */
    buf blk_json;    /* tool_use input json */
    char blk_tool_id[64];
    char blk_tool_name[64];
    int stream_kind;  /* ui stream currently open, 0 = none */
    int any_output;   /* anything shown yet this turn (stops the spinner) */

    tool_call calls[LAMBDA_TOOL_CALLS_MAX];
    int ncalls;

    char stop_reason[32];
    char served_model[128];
    char fallback_note[320];
    char errmsg[512];
    int error;
    int truncated;

    long in_tokens, out_tokens;
    int spin;
    long spin_ms;
} turn;

/* static storage for the turn buffers */
static char s_pending[LAMBDA_SSE_PENDING_MAX];
static char s_data[LAMBDA_SSE_DATA_MAX];
static char s_errbody[LAMBDA_ERRBODY_MAX];
static char s_content[LAMBDA_THINKING_MAX + LAMBDA_REPLY_MAX];
static char s_blk_text[LAMBDA_THINKING_MAX];
static char s_blk_sig[8192];
static char s_blk_json[LAMBDA_TOOL_INPUT_MAX];
static char s_scratch[LAMBDA_THINKING_MAX]; /* decoded strings */

static void turn_init(turn *t)
{
    memset(t, 0, sizeof *t);
    buf_attach(&t->pending, s_pending, sizeof s_pending);
    buf_attach(&t->data, s_data, sizeof s_data);
    buf_attach(&t->errbody, s_errbody, sizeof s_errbody);
    buf_attach(&t->content, s_content, sizeof s_content);
    buf_attach(&t->blk_text, s_blk_text, sizeof s_blk_text);
    buf_attach(&t->blk_sig, s_blk_sig, sizeof s_blk_sig);
    buf_attach(&t->blk_json, s_blk_json, sizeof s_blk_json);
}

/* stream text to the ui, switching stream kind (reply vs reasoning) as the
 * model moves between content blocks */
static void emit(turn *t, int kind, const char *s)
{
    if (t->stream_kind != kind) {
        if (t->stream_kind)
            ui_stream_end();
        ui_status(NULL);
        ui_stream_begin(kind);
        t->stream_kind = kind;
        t->any_output = 1;
    }
    ui_stream_delta(s);
}

static void emit_close(turn *t)
{
    if (t->stream_kind) {
        ui_stream_end();
        t->stream_kind = 0;
    }
}

/* close the current content block, appending it to t->content */
static void block_end(turn *t)
{
    buf scratch;
    switch (t->blk_type) {
    case 1: /* text */
        if (t->blk_text.len == 0)
            break;
        buf_appends(&t->content, t->nblocks ? ",{\"type\":\"text\",\"text\":"
                                            : "{\"type\":\"text\",\"text\":");
        buf_append_json_str(&t->content, t->blk_text.data);
        buf_appends(&t->content, "}");
        t->nblocks++;
        break;
    case 2: /* thinking — replay verbatim on the next request */
        buf_appends(&t->content,
                    t->nblocks ? ",{\"type\":\"thinking\",\"thinking\":"
                               : "{\"type\":\"thinking\",\"thinking\":");
        buf_append_json_str(&t->content, t->blk_text.data);
        buf_appends(&t->content, ",\"signature\":");
        buf_append_json_str(&t->content, t->blk_sig.data);
        buf_appends(&t->content, "}");
        t->nblocks++;
        break;
    case 3: /* tool_use */
        buf_appends(&t->content, t->nblocks ? ",{\"type\":\"tool_use\",\"id\":"
                                            : "{\"type\":\"tool_use\",\"id\":");
        buf_append_json_str(&t->content, t->blk_tool_id);
        buf_appends(&t->content, ",\"name\":");
        buf_append_json_str(&t->content, t->blk_tool_name);
        buf_appends(&t->content, ",\"input\":");
        buf_appends(&t->content,
                    t->blk_json.len ? t->blk_json.data : "{}");
        buf_appends(&t->content, "}");
        t->nblocks++;

        /* record the call so we can run it once the stream ends */
        if (t->ncalls < LAMBDA_TOOL_CALLS_MAX) {
            tool_call *tc = &t->calls[t->ncalls];
            snprintf(tc->id, sizeof tc->id, "%s", t->blk_tool_id);
            snprintf(tc->name, sizeof tc->name, "%s", t->blk_tool_name);
            snprintf(tc->args, sizeof tc->args, "%s",
                     t->blk_json.len ? t->blk_json.data : "{}");
            tc->cmd[0] = '\0';
            if (strcmp(tc->name, "bash") == 0) {
                jdoc d;
                if (jdoc_parse(&d, tc->args, strlen(tc->args)) == 0) {
                    buf_attach(&scratch, s_scratch, sizeof s_scratch);
                    if (j_str(&d, 0, "command", &scratch) == 0)
                        snprintf(tc->cmd, sizeof tc->cmd, "%s", scratch.data);
                }
                if (!tc->cmd[0])
                    break; /* malformed bash call: nothing to run */
            }
            t->ncalls++;
        }
        break;
    default:
        break;
    }
    t->blk_type = 0;
    buf_reset(&t->blk_text);
    buf_reset(&t->blk_sig);
    buf_reset(&t->blk_json);
}

static void handle_event(turn *t, char *payload, size_t len)
{
    jdoc d;
    buf sv;
    buf_attach(&sv, s_scratch, sizeof s_scratch);
    if (jdoc_parse(&d, payload, len) != 0)
        return;
    int ty = j_get(&d, 0, "type");
    if (ty < 0)
        return;

    if (j_is(&d, ty, "content_block_delta")) {
        int delta = j_get(&d, 0, "delta");
        int dt = j_get(&d, delta, "type");
        if (j_is(&d, dt, "text_delta")) {
            if (j_str(&d, delta, "text", &sv) == 0) {
                buf_appends(&t->blk_text, sv.data);
                emit(t, UI_ASSISTANT, sv.data);
            }
        } else if (j_is(&d, dt, "thinking_delta")) {
            if (j_str(&d, delta, "thinking", &sv) == 0) {
                buf_appends(&t->blk_text, sv.data);
                if (sv.len)
                    emit(t, UI_THINKING, sv.data);
            }
        } else if (j_is(&d, dt, "signature_delta")) {
            if (j_str(&d, delta, "signature", &sv) == 0)
                buf_appends(&t->blk_sig, sv.data);
        } else if (j_is(&d, dt, "input_json_delta")) {
            if (j_str(&d, delta, "partial_json", &sv) == 0)
                buf_appends(&t->blk_json, sv.data);
        }
    } else if (j_is(&d, ty, "content_block_start")) {
        int cb = j_get(&d, 0, "content_block");
        int ct = j_get(&d, cb, "type");
        buf_reset(&t->blk_text);
        buf_reset(&t->blk_sig);
        buf_reset(&t->blk_json);
        if (j_is(&d, ct, "text")) {
            t->blk_type = 1;
            if (j_str(&d, cb, "text", &sv) == 0 && sv.len) {
                buf_appends(&t->blk_text, sv.data);
                emit(t, UI_ASSISTANT, sv.data);
            }
        } else if (j_is(&d, ct, "thinking")) {
            t->blk_type = 2;
            if (j_str(&d, cb, "thinking", &sv) == 0 && sv.len) {
                buf_appends(&t->blk_text, sv.data);
                emit(t, UI_THINKING, sv.data);
            }
            if (j_str(&d, cb, "signature", &sv) == 0)
                buf_appends(&t->blk_sig, sv.data);
        } else if (j_is(&d, ct, "tool_use")) {
            t->blk_type = 3;
            if (j_str(&d, cb, "id", &sv) == 0)
                snprintf(t->blk_tool_id, sizeof t->blk_tool_id, "%s", sv.data);
            if (j_str(&d, cb, "name", &sv) == 0)
                snprintf(t->blk_tool_name, sizeof t->blk_tool_name, "%s",
                         sv.data);
        } else if (j_is(&d, ct, "fallback")) {
            char from[128] = "?", to[128] = "?";
            if (j_str(&d, j_get(&d, cb, "from"), "model", &sv) == 0)
                snprintf(from, sizeof from, "%s", sv.data);
            if (j_str(&d, j_get(&d, cb, "to"), "model", &sv) == 0)
                snprintf(to, sizeof to, "%s", sv.data);
            snprintf(t->fallback_note, sizeof t->fallback_note,
                     "%s declined; %s continued", from, to);
            t->blk_type = 0;
        } else {
            t->blk_type = 0;
        }
    } else if (j_is(&d, ty, "content_block_stop")) {
        block_end(t);
    } else if (j_is(&d, ty, "message_start")) {
        int msg = j_get(&d, 0, "message");
        if (j_str(&d, msg, "model", &sv) == 0)
            snprintf(t->served_model, sizeof t->served_model, "%s", sv.data);
        t->in_tokens = j_long(&d, j_get(&d, msg, "usage"), "input_tokens", 0);
    } else if (j_is(&d, ty, "message_delta")) {
        int delta = j_get(&d, 0, "delta");
        if (j_str(&d, delta, "stop_reason", &sv) == 0 && sv.len)
            snprintf(t->stop_reason, sizeof t->stop_reason, "%s", sv.data);
        long ot = j_long(&d, j_get(&d, 0, "usage"), "output_tokens", -1);
        if (ot >= 0)
            t->out_tokens = ot;
    } else if (j_is(&d, ty, "error")) {
        if (j_str(&d, j_get(&d, 0, "error"), "message", &sv) == 0)
            snprintf(t->errmsg, sizeof t->errmsg, "%s", sv.data);
        else
            snprintf(t->errmsg, sizeof t->errmsg, "unknown error");
        t->error = 1;
    }
}

/* split the stream into SSE lines; dispatch on a blank line */
static void feed_sse(turn *t, const char *data, size_t len)
{
    if (buf_append(&t->pending, data, len) != 0) {
        snprintf(t->errmsg, sizeof t->errmsg, "sse buffer overflow");
        t->error = 1;
        buf_reset(&t->pending);
        return;
    }
    char *base = t->pending.data;
    size_t start = 0;
    for (;;) {
        char *nl = memchr(base + start, '\n', t->pending.len - start);
        if (!nl)
            break;
        size_t linelen = (size_t)(nl - (base + start));
        if (linelen > 0 && base[start + linelen - 1] == '\r')
            linelen--;
        char *line = base + start;
        char saved = line[linelen];
        line[linelen] = '\0';

        if (linelen == 0) {
            if (t->data.len > 0)
                handle_event(t, t->data.data, t->data.len);
            buf_reset(&t->data);
        } else if (strncmp(line, "data:", 5) == 0) {
            const char *v = line + 5;
            if (*v == ' ')
                v++;
            buf_appends(&t->data, v);
        }
        /* "event:" lines are redundant — data carries a "type" field */

        line[linelen] = saved;
        start += (size_t)(nl - (base + start)) + 1;
    }
    if (start > 0) {
        memmove(base, base + start, t->pending.len - start);
        t->pending.len -= start;
        base[t->pending.len] = '\0';
    }
}

static void on_body(const char *data, size_t len, void *ud)
{
    turn *t = ud;
    if (t->status == 200)
        feed_sse(t, data, len);
    else
        buf_append(&t->errbody, data, len);
}

static void on_idle(void *ud)
{
    turn *t = ud;
    static const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                   "⠴", "⠦", "⠧", "⠇", "⠏"};
    ui_pump();
    session_flush(); /* drain the transcript queue while the socket is quiet */
    if (t->any_output)
        return;
    /* on a clock, not per call: this also fires on every keystroke now */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (ms - t->spin_ms < 100)
        return;
    t->spin_ms = ms;
    char s[64];
    snprintf(s, sizeof s, "%s thinking…", frames[t->spin++ % 10]);
    ui_status(s);
}

/* ---- request building --------------------------------------------------- */

static char s_body[LAMBDA_BODY_MAX];
static char s_hdrs[LAMBDA_REQ_HDRS_MAX];

static void build_body(const chat *c, buf *b)
{
    buf_appends(b, "{\"model\":");
    buf_append_json_str(b, c->model);
    buf_appendf(b, ",\"max_tokens\":%d,\"stream\":true", MAX_TOKENS);
    if (c->fallbacks && model_has_fallbacks(c->model))
        buf_appends(b, ",\"fallbacks\":\"default\"");
    if (model_is_modern(c->model)) {
        /* adaptive thinking is the only supported mode on these models;
         * a summarised display gives the tui something to show during the
         * long turns fable takes */
        buf_appendf(b, ",\"thinking\":{\"type\":\"adaptive\",\"display\":\"%s\"}",
                    c->show_thinking ? "summarized" : "omitted");
        if (c->effort[0]) {
            buf_appends(b, ",\"output_config\":{\"effort\":");
            buf_append_json_str(b, c->effort);
            buf_appends(b, "}");
        }
    }
    /* project context goes first so it heads the context window and forms a
     * stable cacheable prefix */
    if (c->prelude || c->system[0]) {
        buf_appends(b, ",\"system\":");
        buf_appends(b, "[{\"type\":\"text\",\"text\":");
        buf_append_json_str2(b, c->prelude ? c->prelude : "",
                             c->system[0] ? c->system : "");
        buf_appends(b, ",\"cache_control\":{\"type\":\"ephemeral\"}}]");
    }
    {
        static char tools_store[16384];
        buf tb;
        buf_attach(&tb, tools_store, sizeof tools_store);
        tools_emit_json(&tb, c->tools);
        if (strcmp(tb.data, "[]") != 0) {
            buf_appends(b, ",\"tools\":");
            buf_appends(b, tb.data);
        }
    }
    buf_appends(b, ",\"messages\":[");
    for (int i = 0; i < c->nmsgs; i++) {
        if (i)
            buf_appends(b, ",");
        buf_append(b, c->arena + c->msgs[i].off, c->msgs[i].len);
    }
    buf_appends(b, "]}");
}

static int build_headers(const chat *c, buf *h, char *err, size_t errsz)
{
    const char *key = getenv("ANTHROPIC_API_KEY");
    const char *tok = getenv("ANTHROPIC_AUTH_TOKEN");
    if (key && *key)
        buf_appendf(h, "x-api-key: %s\r\n", key);
    else if (tok && *tok)
        buf_appendf(h, "Authorization: Bearer %s\r\n", tok);
    else {
        snprintf(err, errsz,
                 "no credentials: set ANTHROPIC_API_KEY (or "
                 "ANTHROPIC_AUTH_TOKEN)");
        return -1;
    }
    buf_appends(h, "anthropic-version: " API_VERSION "\r\n");
    if (c->fallbacks && model_has_fallbacks(c->model))
        buf_appends(h, "anthropic-beta: server-side-fallback-2026-07-01\r\n");
    return 0;
}

static void extract_api_error(turn *t)
{
    jdoc d;
    buf sv;
    if (t->errbody.len == 0)
        return;
    buf_attach(&sv, s_scratch, sizeof s_scratch);
    if (jdoc_parse(&d, t->errbody.data, t->errbody.len) == 0 &&
        j_str(&d, j_get(&d, 0, "error"), "message", &sv) == 0)
        snprintf(t->errmsg, sizeof t->errmsg, "%s", sv.data);
}

/* ---- one request/response round ---------------------------------------- */

static int do_request(chat *c, turn *t, char *err, size_t errsz)
{
    buf body, hdrs;
    buf_attach(&body, s_body, sizeof s_body);
    buf_attach(&hdrs, s_hdrs, sizeof s_hdrs);

    if (build_headers(c, &hdrs, err, sizeof s_hdrs > errsz ? errsz : errsz) !=
        0)
        return -1;
    build_body(c, &body);
    if (body.overflow) {
        snprintf(err, errsz,
                 "conversation too large for the request buffer (/clear)");
        return -1;
    }

    http_request req = {
        .host = API_HOST,
        .port = "443",
        .path = "/v1/messages",
        .extra_headers = hdrs.data,
        .body = body.data,
        .body_len = body.len,
        .status = &t->status,
        .on_body = on_body,
        .body_ud = t,
        .on_idle = on_idle,
        .idle_ud = t,
        .watch_input = ui_is_tty(),
    };

    int rc = http_post(&req, err, errsz);
    ui_status(NULL);
    emit_close(t);

    if (rc == HTTP_INTERRUPTED)
        return HTTP_INTERRUPTED;
    if (rc != HTTP_OK)
        return -1;
    if (t->status != 200) {
        extract_api_error(t);
        snprintf(err, errsz, "api error (http %d): %s", t->status,
                 t->errmsg[0] ? t->errmsg : "unknown");
        return -1;
    }
    if (t->error) {
        snprintf(err, errsz, "stream error: %s", t->errmsg);
        return -1;
    }
    return 0;
}

/* ---- public: one full exchange ----------------------------------------- */

static char s_toolout[LAMBDA_TOOL_OUTPUT_MAX];
static char s_msg[LAMBDA_MSG_MAX];

int chat_send(chat *c, const char *user_text)
{
    char err[1024] = "";
    /* rollback point: a failed exchange leaves history untouched */
    int save_nmsgs = c->nmsgs;
    size_t save_used = c->used;

    /* user message */
    buf m;
    buf_attach(&m, s_msg, sizeof s_msg);
    buf_appends(&m, "{\"role\":\"user\",\"content\":");
    buf_append_json_str(&m, user_text);
    buf_appends(&m, "}");
    if (m.overflow || hist_push(c, m.data, m.len) != 0) {
        ui_add(UI_ERROR, "message too large for history (/clear to reset)");
        c->nmsgs = save_nmsgs;
        c->used = save_used;
        return -1;
    }
    session_sync(c);

    http_interrupted = 0;
    /* Paint before the request starts. Everything below — parsing the CA
     * bundle on the first turn, DNS, connect, the TLS handshake — blocks
     * without pumping the ui, so a coalesced repaint would leave the user's
     * own prompt invisible until the response begins. */
    ui_render_force();

    for (int iter = 0; iter < LAMBDA_TOOL_TURNS_MAX; iter++) {
        turn t;
        turn_init(&t);

        int rc = do_request(c, &t, err, sizeof err);
        if (rc == HTTP_INTERRUPTED) {
            ui_add(UI_NOTICE, "interrupted");
            c->nmsgs = save_nmsgs;
            c->used = save_used;
            session_sync(c);
            return -1;
        }
        if (rc != 0) {
            ui_add(UI_ERROR, err);
            c->nmsgs = save_nmsgs;
            c->used = save_used;
            session_sync(c);
            return -1;
        }

        /* status line for this turn */
        char st[768];
        snprintf(st, sizeof st, "%s · %ld in · %ld out%s%s",
                 t.served_model[0] ? t.served_model : c->model, t.in_tokens,
                 t.out_tokens, t.fallback_note[0] ? " · " : "",
                 t.fallback_note);
        ui_status_final(st);

        if (strcmp(t.stop_reason, "refusal") == 0)
            ui_add(UI_ERROR, "the model declined to answer (stop_reason: "
                             "refusal)");
        else if (strcmp(t.stop_reason, "max_tokens") == 0)
            ui_add(UI_NOTICE, "response truncated at max_tokens");

        /* store the assistant turn */
        if (t.nblocks == 0)
            break;
        buf_attach(&m, s_msg, sizeof s_msg);
        buf_appends(&m, "{\"role\":\"assistant\",\"content\":[");
        buf_append(&m, t.content.data, t.content.len);
        buf_appends(&m, "]}");
        if (t.content.overflow || m.overflow ||
            hist_push(c, m.data, m.len) != 0) {
            ui_add(UI_ERROR, "reply too large for history (/clear to reset)");
            c->nmsgs = save_nmsgs;
            c->used = save_used;
            session_sync(c);
            return -1;
        }
        session_sync(c);

        if (t.ncalls == 0)
            break; /* end_turn */

        /* run the tool calls, then feed results back as one user message */
        buf results;
        buf_attach(&results, s_msg, sizeof s_msg);
        buf_appends(&results, "{\"role\":\"user\",\"content\":[");
        for (int i = 0; i < t.ncalls; i++) {
            int is_bash = strcmp(t.calls[i].name, "bash") == 0;
            if (is_bash) {
                ui_add(UI_TOOL_CMD, t.calls[i].cmd);
            } else {
                char label[512];
                tools_label(t.calls[i].name, t.calls[i].args, label,
                            sizeof label);
                ui_add(UI_TOOL_CMD, label);
            }
            ui_render_force();
            ui_pump();

            buf out;
            buf_attach(&out, s_toolout, sizeof s_toolout);
            int status = tools_run(t.calls[i].name,
                                  is_bash ? t.calls[i].cmd : t.calls[i].args,
                                  &out, ui_pump);
            if (out.overflow)
                buf_appends(&out, "\n[output truncated]");
            if (status != 0 && is_bash) {
                char note[64];
                snprintf(note, sizeof note, "\n[exit status %d]", status);
                buf_appends(&out, note);
            }
            if (out.len)
                ui_add(UI_TOOL_OUT, out.data);

            if (i)
                buf_appends(&results, ",");
            buf_appends(&results, "{\"type\":\"tool_result\",\"tool_use_id\":");
            buf_append_json_str(&results, t.calls[i].id);
            if (status != 0)
                buf_appends(&results, ",\"is_error\":true");
            buf_appends(&results, ",\"content\":");
            buf_append_json_str(&results, out.len ? out.data : "(no output)");
            buf_appends(&results, "}");

            if (http_interrupted) {
                ui_add(UI_NOTICE, "interrupted");
                c->nmsgs = save_nmsgs;
                c->used = save_used;
                session_sync(c);
                return -1;
            }
        }
        buf_appends(&results, "]}");
        if (results.overflow || hist_push(c, results.data, results.len) != 0) {
            ui_add(UI_ERROR,
                   "tool output too large for history (/clear to reset)");
            c->nmsgs = save_nmsgs;
            c->used = save_used;
            session_sync(c);
            return -1;
        }
        session_sync(c);
        /* loop: send tool results back to the model */
    }

    session_flush();
    return 0;
}
