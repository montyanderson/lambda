/* Herdr's documented newline-delimited Unix socket API. No helper process,
 * background thread, allocator, credentials, or terminal escape sequences. */
#include "herdr.h"
#include "config.h"
#include "gateway.h"
#include "session.h"
#include "tts.h"
#include "util.h"
#include "voice.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static const char *g_pane, *g_socket, *g_max_tokens, *g_context_window;
static int g_enabled, g_no_context, g_no_fallback;
static pid_t g_owner;
static uint64_t g_seq;
static char g_request[65536], g_resume[49153], g_path[LAMBDA_PATH_MAX];

static int64_t milliseconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int ready(int fd, short events, int64_t deadline)
{
    for (;;) {
        int64_t left = deadline - milliseconds();
        if (left <= 0) return 0;
        struct pollfd p = {fd, events, 0};
        int rc = poll(&p, 1, (int)left);
        if (rc < 0 && errno == EINTR) continue;
        return rc > 0 && (p.revents & events);
    }
}

static void transmit(const buf *b)
{
    if (b->overflow) return;
    int64_t deadline = milliseconds() + 50;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) goto done;
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, g_socket, strlen(g_socket) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        if (errno != EINPROGRESS || !ready(fd, POLLOUT, deadline)) goto done;
        int error = 0;
        socklen_t len = sizeof error;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error) goto done;
    }
    for (size_t off = 0; off < b->len;) {
        if (!ready(fd, POLLOUT, deadline)) goto done;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t n = send(fd, b->data + off, b->len - off, flags);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) goto done;
        off += (size_t)n;
    }
    /* Let the server acknowledge before closing. Bound slow or broken peers
     * by the same deadline as connect/send, including fragmented replies. */
    while (ready(fd, POLLIN, deadline)) {
        char response[1024];
        ssize_t n = recv(fd, response, sizeof response, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0 || memchr(response, '\n', (size_t)n)) break;
    }
done:
    close(fd);
}

static void begin(buf *b, const char *method)
{
    struct timespec ts;
    uint64_t seq = 0;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        seq = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    g_seq = seq > g_seq ? seq : g_seq + 1;
    buf_attach(b, g_request, sizeof g_request);
    buf_appends(b, "{\"id\":\"lambda\",\"method\":");
    buf_append_json_str(b, method);
    buf_appends(b, ",\"params\":{\"pane_id\":");
    buf_append_json_str(b, g_pane);
    buf_appendf(b, ",\"source\":\"lambda\",\"agent\":\"lambda\",\"seq\":%" PRIu64, g_seq);
}

void herdr_init(int argc, char **argv)
{
    const char *env = getenv("HERDR_ENV"), *bin = getenv("HERDR_BIN_PATH");
    g_pane = getenv("HERDR_PANE_ID");
    g_socket = getenv("HERDR_SOCKET_PATH");
    struct sockaddr_un address;
    if (!env || strcmp(env, "1") || !bin || !*bin || !g_pane || !*g_pane ||
        strlen(g_pane) > 256 || !g_socket || !*g_socket ||
        strlen(g_socket) >= sizeof address.sun_path) return;
    g_owner = getpid();
    g_enabled = 1;
    /* Only retain startup options not represented by the live chat/settings.
     * Skip option values so a system prompt cannot masquerade as a flag. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--no-context")) g_no_context = 1;
        else if (!strcmp(a, "--no-fallback")) g_no_fallback = 1;
        else if (!strcmp(a, "--max-tokens") && i + 1 < argc) g_max_tokens = argv[++i];
        else if (!strcmp(a, "--context-window") && i + 1 < argc) g_context_window = argv[++i];
        else if ((!strcmp(a, "-m") || !strcmp(a, "-e") || !strcmp(a, "-s") ||
                  !strcmp(a, "-p") || !strcmp(a, "-r") || !strcmp(a, "--resume") ||
                  !strcmp(a, "--litellm") || !strcmp(a, "--voice-id")) && i + 1 < argc) i++;
    }
    atexit(herdr_release);
}

/* Herdr 0.9.2+ rejects the whole report for unsafe/oversized resume argv.
 * Omit only resume metadata in that case; lifecycle reports must still work. */
static int resume_command(chat *c, int thinking)
{
    const char *path = session_path();
    if (!*path) return 0;
    if (*path == '/') {
        if (strlen(path) >= sizeof g_path) return 0;
        memcpy(g_path, path, strlen(path) + 1);
    } else {
        if (!getcwd(g_path, sizeof g_path)) return 0;
        size_t len = strlen(g_path);
        if (len + 1 + strlen(path) >= sizeof g_path) return 0;
        g_path[len++] = '/';
        memcpy(g_path + len, path, strlen(path) + 1);
    }
    const char *args[64];
    int n = 0;
    args[n++] = "lambda";
    args[n++] = "-r";
    args[n++] = g_path;
    args[n++] = "-m";
    args[n++] = chat_model(c);
    if (*chat_effort(c)) { args[n++] = "-e"; args[n++] = chat_effort(c); }
    if (gateway_enabled()) { args[n++] = "--litellm"; args[n++] = gateway_url(); }
    else args[n++] = "--anthropic";
    if (!chat_tools_enabled(c)) args[n++] = "--no-tools";
    if (!thinking) args[n++] = "--no-thinking";
    if (g_no_context) args[n++] = "--no-context";
    if (g_no_fallback) args[n++] = "--no-fallback";
    if (g_max_tokens) { args[n++] = "--max-tokens"; args[n++] = g_max_tokens; }
    if (g_context_window) { args[n++] = "--context-window"; args[n++] = g_context_window; }
    if (voice_enabled()) {
        args[n++] = "--voice";
        args[n++] = "--voice-id";
        args[n++] = tts_voice_id();
    }
    size_t bytes = 0;
    for (int i = 0; i < n; i++) {
        bytes += strlen(args[i]) + 1;
        if (bytes > 8192) return 0;
        for (const unsigned char *p = (const unsigned char *)args[i]; *p; p++)
            if (*p == '\'' || *p < 32 || *p == 127 ||
                (*p == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f)) return 0;
    }
    buf b;
    buf_attach(&b, g_resume, sizeof g_resume);
    buf_appends(&b, "[");
    for (int i = 0; i < n; i++) {
        if (i) buf_appends(&b, ",");
        buf_append_json_str(&b, args[i]);
    }
    buf_appends(&b, "]");
    return !b.overflow;
}

void herdr_report(chat *c, int thinking, const char *state, const char *message)
{
    if (!g_enabled || getpid() != g_owner) return;
    int saved = errno;
    buf b;
    begin(&b, "pane.report_agent");
    buf_appends(&b, ",\"state\":");
    buf_append_json_str(&b, state);
    if (message) {
        buf_appends(&b, ",\"message\":");
        buf_append_json_str(&b, message);
    }
    if (resume_command(c, thinking)) {
        buf_appends(&b, ",\"agent_session_path\":");
        buf_append_json_str(&b, g_path);
        buf_appends(&b, ",\"resume_argv\":");
        buf_appends(&b, g_resume);
    }
    buf_appends(&b, "}}\n");
    transmit(&b);
    errno = saved;
}

void herdr_release(void)
{
    if (!g_enabled || getpid() != g_owner) return;
    int saved = errno;
    buf b;
    begin(&b, "pane.release_agent");
    buf_appends(&b, "}}\n");
    transmit(&b);
    g_enabled = 0;
    errno = saved;
}
