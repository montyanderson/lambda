/* Speech runs in an isolated process so TLS, JSON parsing and player
 * backpressure cannot stall the model stream or the terminal editor. */
#include "voice.h"
#include "tts.h"
#include "http.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int enabled, player_kind;
static char player_path[4096];
static pid_t worker_pid;
static int text_fd = -1, error_fd = -1, ending;
static char queued[1u << 20], error_message[256];
static size_t qbegin, qend;
static int error_pending;

static void report(const char *message)
{
    snprintf(error_message, sizeof error_message, "%s", message);
    error_pending = 1;
}

static int find_player(void)
{
    const char *names[] = {"mpv", "mpg123", "ffplay"};
    const char *path = getenv("PATH");
    if (!path) return -1;
    for (int i = 0; i < 3; i++) {
        const char *p = path;
        do {
            const char *end = strchr(p, ':');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            if (n + strlen(names[i]) + 2 < sizeof player_path) {
                snprintf(player_path, sizeof player_path, "%.*s/%s",
                         (int)(n ? n : 1), n ? p : ".", names[i]);
                struct stat st;
                if (access(player_path, X_OK) == 0 && stat(player_path, &st) == 0 &&
                    S_ISREG(st.st_mode)) {
                    player_kind = i;
                    return 0;
                }
            }
            p = end ? end + 1 : NULL;
        } while (p);
    }
    return -1;
}

int voice_enable(int on, char *err, size_t cap)
{
    if (!on) {
        enabled = 0;
        voice_stop();
        return 0;
    }
    if (tts_load_voice(err, cap) != 0 || tts_configured(err, cap) != 0) return -1;
    if (find_player() != 0) {
        snprintf(err, cap, "voice: install mpv, mpg123 or ffplay (macOS: brew install mpv)");
        return -1;
    }
    enabled = 1;
    return 0;
}

int voice_enabled(void) { return enabled; }

/* Close inherited transcript files and sockets. /dev/fd exists on Linux
 * and macOS; the fallback also works on other POSIX systems. */
static void close_except(int a, int b)
{
    DIR *dir = opendir("/dev/fd");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            char *end;
            long fd = strtol(entry->d_name, &end, 10);
            if (*entry->d_name && !*end && fd > 2 && fd != a && fd != b && fd != dirfd(dir))
                close((int)fd);
        }
        closedir(dir);
    } else {
        long maxfd = sysconf(_SC_OPEN_MAX);
        for (int fd = 3; fd < maxfd; fd++)
            if (fd != a && fd != b) close(fd);
    }
}

static void worker_interrupt(int sig)
{
    (void)sig;
    http_interrupted = 1;
}

typedef struct { int fd; pid_t pid; } player;

static int start_player(player *p)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    p->pid = fork();
    if (p->pid == 0) {
        if (dup2(fds[0], STDIN_FILENO) < 0) _exit(127);
        close_except(-1, -1);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGALRM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        unsetenv("INWORLD_API_KEY");
        if (player_kind == 0)
            execl(player_path, "mpv", "--no-config", "--no-video", "--no-terminal",
                  "--really-quiet", "--cache=no", "--audio-buffer=0.2",
                  "--demuxer=lavf", "--demuxer-lavf-format=mp3", "-", (char *)NULL);
        else if (player_kind == 1)
            execl(player_path, "mpg123", "-q", "-", (char *)NULL);
        else
            execl(player_path, "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet",
                  "-f", "mp3", "-i", "pipe:0", (char *)NULL);
        _exit(127);
    }
    close(fds[0]);
    if (p->pid < 0) { close(fds[1]); return -1; }
    p->fd = fds[1];
    return 0;
}

static int play_audio(const char *bytes, size_t len, void *ud)
{
    player *p = ud;
    if (p->fd < 0 && start_player(p) != 0) return -1;
    while (len && !http_interrupted) {
        ssize_t n = write(p->fd, bytes, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        bytes += n;
        len -= (size_t)n;
    }
    return len ? -1 : 0;
}

static int space(unsigned char ch)
{
    return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
}

static long voice_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* Prefer complete sentences, then whitespace, always at a UTF-8 boundary.
 * Short unpunctuated replies flush after at most 400 ms or at EOF. */
static size_t phrase_size(const char *s, size_t len, int flush)
{
    size_t end = 0, word = 0;
    while (end < len && end < TTS_TEXT_MAX) {
        size_t n = utf8_span(s + end, len - end);
        if (!n || end + n > TTS_TEXT_MAX) break;
        if (s[end] == '\n' || (end && space((unsigned char)s[end]) &&
            (s[end - 1] == '.' || s[end - 1] == '!' || s[end - 1] == '?')))
            return end + n;
        if (space((unsigned char)s[end])) word = end + n;
        end += n;
    }
    if (flush && end == len)
        return flush == 1 && word && !space((unsigned char)s[end - 1]) ? word : end;
    if (len >= TTS_TEXT_MAX) return word ? word : end;
    return 0;
}

static void worker(int input, int errors)
{
    close_except(input, errors);
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd < 0) _exit(1);
    for (int fd = 0; fd < 3; fd++)
        if (dup2(nullfd, fd) < 0) _exit(1);
    if (nullfd > 2) close(nullfd);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = worker_interrupt;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    http_interrupted = 0;
    char pending[4096], err[256] = "";
    size_t len = 0;
    long buffered_at = 0;
    int eof = 0, flush = 0;
    player p = {.fd = -1};
    while (!http_interrupted) {
        if (len && voice_ms() - buffered_at >= 400) flush = 1;
        size_t take = phrase_size(pending, len, eof ? 2 : flush);
        if (take) {
            char phrase[TTS_TEXT_MAX + 1];
            memcpy(phrase, pending, take);
            phrase[take] = '\0';
            memmove(pending, pending + take, len - take);
            len -= take;
            buffered_at = voice_ms();
            flush = 0;
            size_t n = take;
            while (n && space((unsigned char)phrase[n - 1])) phrase[--n] = '\0';
            char *text = phrase;
            while (space((unsigned char)*text)) text++;
            if (*text) {
                alarm(120); /* covers connect, synthesis and a stalled sink */
                int rc = tts_stream(text, play_audio, &p, err, sizeof err);
                alarm(0);
                if (rc != 0) break;
            }
            continue;
        }
        if (eof) {
            if (len) snprintf(err, sizeof err, "voice: incomplete UTF-8 in reply");
            break;
        }
        struct pollfd fd = {.fd = input, .events = POLLIN};
        long remaining = 400 - (voice_ms() - buffered_at);
        int timeout = len && !flush ? (remaining > 0 ? (int)remaining : 0) : -1;
        int rc = poll(&fd, 1, timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            snprintf(err, sizeof err, "voice: cannot read reply stream");
            break;
        }
        if (!rc) { flush = 1; continue; }
        ssize_t n = read(input, pending + len, sizeof pending - len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { snprintf(err, sizeof err, "voice: cannot read reply stream"); break; }
        if (!n) eof = 1;
        else {
            if (!len) buffered_at = voice_ms();
            len += (size_t)n;
            flush = 0;
        }
    }
    close(input);
    if (p.fd >= 0) close(p.fd);
    if (p.pid > 0) {
        if (http_interrupted || *err) kill(p.pid, SIGTERM);
        int status = 0;
        while (waitpid(p.pid, &status, 0) < 0 && errno == EINTR)
            if (http_interrupted) kill(p.pid, SIGTERM);
        if (!http_interrupted && !*err && (!WIFEXITED(status) || WEXITSTATUS(status)))
            snprintf(err, sizeof err, "voice: audio player failed; check your audio output device");
    }
    if (*err) {
        ssize_t ignored = write(errors, err, strlen(err));
        (void)ignored;
    }
    close(errors);
    _exit(*err ? 1 : 0);
}

static void close_pipes(void)
{
    if (text_fd >= 0) close(text_fd);
    if (error_fd >= 0) close(error_fd);
    text_fd = error_fd = -1;
    qbegin = qend = 0;
}

void voice_stop(void)
{
    close_pipes();
    if (worker_pid > 0) {
        kill(-worker_pid, SIGTERM);
        for (int i = 0; i < 25; i++) {
            pid_t r = waitpid(worker_pid, NULL, WNOHANG);
            if (r == worker_pid || (r < 0 && errno == ECHILD)) {
                worker_pid = 0;
                return;
            }
            struct timespec pause = {0, 10000000};
            nanosleep(&pause, NULL);
        }
        kill(-worker_pid, SIGKILL);
        while (waitpid(worker_pid, NULL, 0) < 0 && errno == EINTR) {}
        worker_pid = 0;
    }
}

void voice_begin(void)
{
    voice_stop();
    ending = 0;
    if (!enabled) return;
    int in[2], out[2];
    if (pipe(in) != 0) { report("voice: cannot open speech pipe"); return; }
    if (pipe(out) != 0) {
        close(in[0]); close(in[1]);
        report("voice: cannot open speech pipe"); return;
    }
    /* CLOEXEC stops tools inheriting a writer and keeping synthesis alive. */
    for (int i = 0; i < 2; i++) {
        fcntl(in[i], F_SETFD, FD_CLOEXEC);
        fcntl(out[i], F_SETFD, FD_CLOEXEC);
    }
    worker_pid = fork();
    if (worker_pid == 0) {
        setpgid(0, 0);
        worker(in[0], out[1]);
    }
    close(in[0]); close(out[1]);
    if (worker_pid < 0) {
        worker_pid = 0;
        close(in[1]); close(out[0]);
        report("voice: cannot start speech worker"); return;
    }
    setpgid(worker_pid, worker_pid);
    text_fd = in[1]; error_fd = out[0];
    if (fcntl(text_fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(error_fd, F_SETFL, O_NONBLOCK) < 0) {
        voice_stop();
        report("voice: cannot configure speech pipe");
    }
}

int voice_poll(void)
{
    if (!worker_pid) return 0;
    if (http_interrupted) { voice_stop(); return 0; }
    char message[sizeof error_message];
    ssize_t n = read(error_fd, message, sizeof message - 1);
    if (n > 0) {
        message[n] = '\0';
        report(message);
    }
    int status = 0;
    pid_t r = waitpid(worker_pid, &status, WNOHANG);
    if (r == worker_pid || (r < 0 && errno == ECHILD)) {
        /* Read after wait too: an error can arrive between read and exit. */
        n = read(error_fd, message, sizeof message - 1);
        if (n > 0) { message[n] = '\0'; report(message); }
        if ((!WIFEXITED(status) || WEXITSTATUS(status)) && !error_pending)
            report("voice: speech worker failed");
        kill(-worker_pid, SIGKILL); /* clean up a player after abnormal exit */
        worker_pid = 0;
        close_pipes();
        return 0;
    }
    while (qbegin < qend && text_fd >= 0) {
        n = write(text_fd, queued + qbegin, qend - qbegin);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) { close(text_fd); text_fd = -1; qbegin = qend = 0; break; }
        qbegin += (size_t)n;
    }
    if (qbegin == qend) {
        qbegin = qend = 0;
        if (ending && text_fd >= 0) { close(text_fd); text_fd = -1; }
    }
    return 1;
}

void voice_text(const char *text)
{
    voice_poll();
    if (text_fd < 0 || ending) return;
    size_t n = strlen(text);
    if (n > sizeof queued - qend && qbegin) {
        memmove(queued, queued + qbegin, qend - qbegin);
        qend -= qbegin; qbegin = 0;
    }
    if (n > sizeof queued - qend) {
        voice_stop();
        report("voice: speech queue full; stopped audio for this reply");
        return;
    }
    memcpy(queued + qend, text, n);
    qend += n;
    voice_poll();
}

void voice_end(void) { ending = 1; voice_poll(); }

const char *voice_error(void)
{
    if (!error_pending) return NULL;
    error_pending = 0;
    return error_message;
}
