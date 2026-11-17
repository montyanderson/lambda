/* Newline-delimited JSON transcripts under ./.lambda/chats/.
 *
 * Writes are deferred: records land in a growable queue as the conversation
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
#include <stdlib.h>
#include <limits.h>

#define CHATS_DIR ".lambda/chats"

static int g_fd = -1;
static FILE *g_reader; /* keep the replay fd open: closing it releases POSIX locks */
static char g_path[LAMBDA_PATH_MAX];
static buf g_q = {.grow = 1};
static int g_logged;   /* history messages already written */
static int g_warned;

const char *session_path(void) { return g_fd >= 0 ? g_path : ""; }
int session_enabled(void) { return g_fd >= 0; }

void session_disable(void)
{
    if (g_fd >= 0) {
        if (g_reader)
            fclose(g_reader);
        else
            close(g_fd);
        g_reader = NULL;
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

static int open_append(const char *path, int create)
{
    int flags = O_RDWR | O_APPEND | O_NONBLOCK | O_CLOEXEC;
    int fd = open(path, flags | (create ? O_CREAT | O_EXCL : 0), 0600);
    if (fd < 0)
        return -1;
    struct flock lock;
    memset(&lock, 0, sizeof lock);
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        int error = errno == EACCES || errno == EAGAIN ? EBUSY : errno;
        close(fd);
        errno = error;
        return -1;
    }
    g_fd = fd;
    snprintf(g_path, sizeof g_path, "%s", path);
    buf_reset(&g_q);
    return 0;
}

int session_open(chat *c)
{
    if (ensure_dir() != 0)
        return -1;

    char stamp[64], path[LAMBDA_PATH_MAX];
    now_stamp(stamp, sizeof stamp);
    snprintf(path, sizeof path, "%s/%s.jsonl", CHATS_DIR, stamp);
    /* Claim the filename atomically; concurrent starts never append together. */
    for (int i = 1; open_append(path, 1) != 0; i++) {
        if (errno != EEXIST || i >= 10000)
            return -1;
        snprintf(path, sizeof path, "%s/%s-%04d.jsonl", CHATS_DIR, stamp, i);
    }

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
    static buf storage = {.grow = 1};
    buf *b = &storage;
    buf_reset(b);
    buf_appends(b, "{\"t\":\"meta\",\"k\":");
    buf_append_json_str(b, key);
    buf_appends(b, ",\"v\":");
    buf_append_json_str(b, value ? value : "");
    buf_appends(b, "}");
    if (b->overflow) warn_once("not enough memory to save session settings");
    else queue(b->data, b->len);
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

void session_compact(chat *c, int drop, int pin)
{
    if (g_fd < 0)
        return;
    static char rec[LAMBDA_SUMMARY_MAX * 6 + 128];
    buf b;
    buf_attach(&b, rec, sizeof rec);
    buf_appendf(&b, "{\"t\":\"compact\",\"drop\":%d,\"pin\":%d,\"summary\":", drop, pin);
    buf_append_json_str(&b, chat_summary(c));
    buf_appends(&b, "}");
    queue(b.data, b.len);
    g_logged = chat_msg_count(c);
    session_flush(); /* one complete record is the compaction commit */
}

void session_clear(void)
{
    queue("{\"t\":\"clear\"}", strlen("{\"t\":\"clear\"}"));
    g_logged = 0;
}

void session_close(void)
{
    session_flush();
    session_disable();
}

/* ---- resume ------------------------------------------------------------- */

/* replay one stored message into the transcript view */
static void replay_ui(const char *json, size_t len)
{
    jdoc d;
    static buf sv = {.grow = 1};
    buf_reset(&sv);
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

int session_resume(const char *path, chat *c)
{
    /* Lock before reading. Replay all records chronologically: compact alters
     * only the active selection, while original messages stay in the archive. */
    if (open_append(path, 0) != 0) return -1;
    FILE *f = fdopen(g_fd, "r");
    if (!f) {
        int error = errno;
        session_disable();
        errno = error;
        return -1;
    }
    g_reader = f;
    chat_clear(c);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int needs_newline = 0, error = ENOMEM;
    buf value = {.grow = 1};
    while ((n = getline(&line, &cap, f)) >= 0) {
        needs_newline = n > 0 && line[n - 1] != '\n';
        size_t len = (size_t)n;
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
        if (!len) continue;
        if (len > INT_MAX) { error = EOVERFLOW; goto fail; }
        jdoc d;
        if (jdoc_parse(&d, line, len) != 0) continue;
        int t = j_get(&d, 0, "t");
        if (j_is(&d, t, "msg")) {
            const char *m;
            size_t mlen;
            if (j_raw(&d, j_get(&d, 0, "m"), &m, &mlen) != 0) continue;
            if (chat_push_raw(c, m, mlen) != 0) goto fail;
        } else if (j_is(&d, t, "clear")) {
            chat_clear(c);
        } else if (j_is(&d, t, "compact")) {
            long drop = j_long(&d, 0, "drop", -1), pin = j_long(&d, 0, "pin", -1);
            if (drop < 0 || drop > chat_msg_count(c) || pin < -1 || pin >= drop) continue;
            if (j_str(&d, 0, "summary", &value) == 0 && !value.overflow)
                chat_restore_compact(c, (int)drop, (int)pin, value.data);
            else if (value.overflow) goto fail;
        } else if (j_is(&d, t, "drop")) {
            long drop = j_long(&d, 0, "n", 0);
            if (drop > 0 && drop <= chat_msg_count(c)) chat_drop(c, (int)drop);
        } else if (j_is(&d, t, "session") || j_is(&d, t, "meta")) {
            if (j_is(&d, t, "session")) {
                if (j_str(&d, 0, "model", &value) == 0 && value.len)
                    chat_set_model(c, value.data);
            } else {
                int k = j_get(&d, 0, "k");
                if (j_is(&d, k, "model") && j_str(&d, 0, "v", &value) == 0)
                    chat_set_model(c, value.data);
                else if (j_is(&d, k, "system") && j_str(&d, 0, "v", &value) == 0)
                    chat_set_system(c, value.data);
                else if (j_is(&d, k, "effort") && j_str(&d, 0, "v", &value) == 0)
                    chat_set_effort(c, value.data);
            }
            if (value.overflow) goto fail;
        }
    }
    if (!feof(f)) { error = errno ? errno : EIO; goto fail; }
    free(line);
    buf_free(&value);
    /* Repaint every surviving original, including messages covered by a
     * summary. Never replay rolled-back turns or pre-/clear history. */
    size_t total = chat_history_count(c);
    for (size_t i = 0; i < total; i++) {
        size_t len;
        const char *m = chat_history_msg(c, i, &len);
        replay_ui(m, len);
    }
    if (needs_newline) queue("", 0);
    g_logged = chat_msg_count(c);
    return total > INT_MAX ? INT_MAX : (int)total;
fail:
    free(line);
    buf_free(&value);
    session_disable();
    errno = error;
    return -1;
}

int session_find_last(char *out, size_t cap)
{
    DIR *d = opendir(CHATS_DIR);
    if (!d)
        return -1;
    char best[LAMBDA_PATH_MAX] = "";
    struct timespec best_time = {0, 0};
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 6, ".jsonl") != 0)
            continue;
        struct stat info;
        if (fstatat(dirfd(d), e->d_name, &info, 0) != 0 || !S_ISREG(info.st_mode))
            continue;
#ifdef __APPLE__
        struct timespec modified = info.st_mtimespec;
#else
        struct timespec modified = info.st_mtim;
#endif
        /* Same-second suffixes do not sort after the unsuffixed filename.
         * Modification time also makes a resumed chat the latest again. */
        if (!best[0] || modified.tv_sec > best_time.tv_sec ||
            (modified.tv_sec == best_time.tv_sec && modified.tv_nsec > best_time.tv_nsec) ||
            (modified.tv_sec == best_time.tv_sec && modified.tv_nsec == best_time.tv_nsec &&
             strcmp(e->d_name, best) > 0)) {
            snprintf(best, sizeof best, "%s", e->d_name);
            best_time = modified;
        }
    }
    closedir(d);
    if (!best[0])
        return -1;
    snprintf(out, cap, "%s/%s", CHATS_DIR, best);
    return 0;
}
