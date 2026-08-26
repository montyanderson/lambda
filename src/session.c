/* Newline-delimited JSON transcripts under ./.lambda/chats/.
 *
 * Writes are deferred: records land in a static queue as the conversation
 * happens, and session_flush() drains it only where a disk wait is harmless
 * (network idle, between turns, while waiting for keyboard input). The render
 * loop and the request path never touch the file. */

#include "session.h"
#include "config.h"
#include "jsonx.h"
#include "ui.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHATS_DIR ".lambda/chats"

static int g_fd = -1;
static char g_path[LAMBDA_PATH_MAX];
static char g_qstore[LAMBDA_SESSION_QUEUE];
static buf g_q;
static int g_logged;   /* history messages already written */
static int g_warned;

const char *session_path(void) { return g_fd >= 0 ? g_path : ""; }
int session_enabled(void) { return g_fd >= 0; }

void session_disable(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    buf_reset(&g_q);
}

static void warn_once(const char *msg)
{
    if (!g_warned) {
        g_warned = 1;
        ui_add(UI_ERROR, msg);
    }
}

/* ---- queue -------------------------------------------------------------- */

/* Queue a finished record. If the queue is nearly full we flush right here —
 * losing the transcript matters more than a rare millisecond of latency. */
static void queue(const char *data, size_t len)
{
    if (g_fd < 0)
        return;
    if (g_q.len + len + 1 >= g_q.cap)
        session_flush();
    if (buf_append(&g_q, data, len) != 0 || buf_appends(&g_q, "\n") != 0) {
        buf_reset(&g_q);
        warn_once("session log overflowed; some records were dropped");
    }
}

void session_flush(void)
{
    if (g_fd < 0 || g_q.len == 0)
        return;
    size_t off = 0;
    while (off < g_q.len) {
        ssize_t n = write(g_fd, g_q.data + off, g_q.len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; /* keep the remainder queued */
            warn_once("session log write failed; logging disabled");
            session_disable();
            return;
        }
        off += (size_t)n;
    }
    if (off > 0 && off < g_q.len) {
        memmove(g_q.data, g_q.data + off, g_q.len - off);
        g_q.len -= off;
        g_q.data[g_q.len] = '\0';
    } else if (off >= g_q.len) {
        buf_reset(&g_q);
    }
}

/* ---- opening ------------------------------------------------------------ */

static int ensure_dir(void)
{
    if (mkdir(".lambda", 0700) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(CHATS_DIR, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void now_stamp(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%dT%H-%M-%SZ", &tm);
}

static void now_iso(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static int open_append(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK, 0600);
    if (fd < 0)
        return -1;
    g_fd = fd;
    snprintf(g_path, sizeof g_path, "%s", path);
    buf_attach(&g_q, g_qstore, sizeof g_qstore);
    return 0;
}

int session_open(chat *c)
{
    if (ensure_dir() != 0)
        return -1;

    char stamp[64], path[LAMBDA_PATH_MAX];
    now_stamp(stamp, sizeof stamp);
    snprintf(path, sizeof path, "%s/%s.jsonl", CHATS_DIR, stamp);
    /* if that name exists (same second), fall back to a suffix */
    for (int i = 1; access(path, F_OK) == 0 && i < 100; i++)
        snprintf(path, sizeof path, "%s/%s-%d.jsonl", CHATS_DIR, stamp, i);

    if (open_append(path) != 0)
        return -1;

    char iso[64], cwd[LAMBDA_PATH_MAX];
    now_iso(iso, sizeof iso);
    if (!getcwd(cwd, sizeof cwd))
        cwd[0] = '\0';

    static char rec[LAMBDA_PATH_MAX + 512];
    buf b;
    buf_attach(&b, rec, sizeof rec);
    buf_appends(&b, "{\"t\":\"session\",\"v\":1,\"time\":");
    buf_append_json_str(&b, iso);
    buf_appends(&b, ",\"model\":");
    buf_append_json_str(&b, chat_model(c));
    buf_appends(&b, ",\"cwd\":");
    buf_append_json_str(&b, cwd);
    buf_appends(&b, "}");
    queue(b.data, b.len);
    g_logged = 0;
    return 0;
}

void session_meta(const char *key, const char *value)
{
    if (g_fd < 0)
        return;
    static char rec[8192];
    buf b;
    buf_attach(&b, rec, sizeof rec);
    buf_appends(&b, "{\"t\":\"meta\",\"k\":");
    buf_append_json_str(&b, key);
    buf_appends(&b, ",\"v\":");
    buf_append_json_str(&b, value ? value : "");
    buf_appends(&b, "}");
    queue(b.data, b.len);
}

/* ---- syncing history ---------------------------------------------------- */

void session_sync(chat *c)
{
    if (g_fd < 0)
        return;
    int n = chat_msg_count(c);

    if (n < g_logged) { /* the exchange failed and was rolled back */
        char rec[64];
        int len = snprintf(rec, sizeof rec, "{\"t\":\"drop\",\"n\":%d}",
                           g_logged - n);
        queue(rec, (size_t)len);
        g_logged = n;
        return;
    }
    for (; g_logged < n; g_logged++) {
        size_t len;
        const char *m = chat_msg(c, g_logged, &len);
        if (!m)
            break;
        /* {"t":"msg","m":<message>} — the body is appended without copying */
        if (g_q.len + len + 32 >= g_q.cap)
            session_flush();
        int rc = buf_appends(&g_q, "{\"t\":\"msg\",\"m\":");
        rc |= buf_append(&g_q, m, len);
        rc |= buf_appends(&g_q, "}\n");
        if (rc != 0) {
            buf_reset(&g_q);
            warn_once("session log overflowed; some records were dropped");
        }
    }
}

void session_close(void)
{
    session_flush();
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

/* ---- resume ------------------------------------------------------------- */

/* replay one stored message into the transcript view */
static void replay_ui(const char *json, size_t len)
{
    jdoc d;
    static char sv_store[LAMBDA_TOOL_OUTPUT_MAX];
    buf sv;
    buf_attach(&sv, sv_store, sizeof sv_store);
    if (jdoc_parse(&d, json, len) != 0)
        return;

    int role = j_get(&d, 0, "role");
    int content = j_get(&d, 0, "content");
    int is_user = j_is(&d, role, "user");

    if (content >= 0 && d.toks[content].type == JSMN_STRING) {
        if (j_str_tok(&d, content, &sv) == 0)
            ui_add(is_user ? UI_USER : UI_ASSISTANT, sv.data);
        return;
    }
    int nblocks = j_count(&d, content);
    for (int i = 0; i < nblocks; i++) {
        int blk = j_elem(&d, content, i);
        int ty = j_get(&d, blk, "type");
        if (j_is(&d, ty, "text")) {
            if (j_str(&d, blk, "text", &sv) == 0 && sv.len)
                ui_add(is_user ? UI_USER : UI_ASSISTANT, sv.data);
        } else if (j_is(&d, ty, "tool_use")) {
            if (j_str(&d, j_get(&d, blk, "input"), "command", &sv) == 0)
                ui_add(UI_TOOL_CMD, sv.data);
        } else if (j_is(&d, ty, "tool_result")) {
            if (j_str(&d, blk, "content", &sv) == 0 && sv.len)
                ui_add(UI_TOOL_OUT, sv.data);
        }
        /* thinking blocks are replayed to the api but not shown */
    }
}

static char g_line[LAMBDA_MSG_MAX + 1024];
static long g_offs[LAMBDA_MAX_MSGS]; /* file offset of each surviving msg */

/* strip the trailing newline; returns the remaining length */
static size_t chomp(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
    return len;
}

int session_resume(const char *path, chat *c)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    /* Pass 1: note where each message lives, honouring `drop` records so
     * rolled-back exchanges are not resurrected. */
    int nlive = 0;
    for (;;) {
        long off = ftell(f);
        if (!fgets(g_line, sizeof g_line, f))
            break;
        size_t len = chomp(g_line);
        if (len == 0)
            continue;
        jdoc d;
        if (jdoc_parse(&d, g_line, len) != 0)
            continue;
        int t = j_get(&d, 0, "t");

        if (j_is(&d, t, "msg")) {
            if (nlive < LAMBDA_MAX_MSGS)
                g_offs[nlive++] = off;
        } else if (j_is(&d, t, "drop")) {
            long n = j_long(&d, 0, "n", 0);
            nlive -= (int)n;
            if (nlive < 0)
                nlive = 0;
        } else if (j_is(&d, t, "session") || j_is(&d, t, "meta")) {
            static char sv_store[LAMBDA_SYSTEM_MAX];
            buf sv;
            buf_attach(&sv, sv_store, sizeof sv_store);
            if (j_is(&d, t, "session")) {
                if (j_str(&d, 0, "model", &sv) == 0 && sv.len)
                    chat_set_model(c, sv.data);
            } else {
                int k = j_get(&d, 0, "k");
                if (j_is(&d, k, "model") && j_str(&d, 0, "v", &sv) == 0)
                    chat_set_model(c, sv.data);
                else if (j_is(&d, k, "system") && j_str(&d, 0, "v", &sv) == 0)
                    chat_set_system(c, sv.data);
                else if (j_is(&d, k, "effort") && j_str(&d, 0, "v", &sv) == 0)
                    chat_set_effort(c, sv.data);
            }
        }
    }

    /* Pass 2: restore the survivors, in order. */
    int restored = 0;
    for (int i = 0; i < nlive; i++) {
        if (fseek(f, g_offs[i], SEEK_SET) != 0)
            break;
        if (!fgets(g_line, sizeof g_line, f))
            break;
        size_t len = chomp(g_line);
        jdoc d;
        if (jdoc_parse(&d, g_line, len) != 0)
            continue;
        const char *m;
        size_t mlen;
        if (j_raw(&d, j_get(&d, 0, "m"), &m, &mlen) != 0)
            continue;
        if (chat_push_raw(c, m, mlen) == 0) {
            replay_ui(m, mlen);
            restored++;
        }
    }
    fclose(f);

    if (open_append(path) != 0)
        return -1;
    g_logged = chat_msg_count(c);
    return restored;
}

int session_find_last(char *out, size_t cap)
{
    DIR *d = opendir(CHATS_DIR);
    if (!d)
        return -1;
    char best[LAMBDA_PATH_MAX] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 6, ".jsonl") != 0)
            continue;
        /* names are ISO timestamps, so lexical order is chronological */
        if (strcmp(e->d_name, best) > 0)
            snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(d);
    if (!best[0])
        return -1;
    snprintf(out, cap, "%s/%s", CHATS_DIR, best);
    return 0;
}
