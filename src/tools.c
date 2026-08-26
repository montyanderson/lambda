/* The one tool: bash. fork + /bin/sh -c with merged stdout/stderr piped
 * back, output capped, interruptible. */

#include "tools.h"
#include "http.h"
#include "plugin.h"
#include "ui.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

const char *TOOL_BASH_JSON =
    "{\"name\":\"bash\","
    "\"description\":\"Run a shell command on the user's machine via "
    "/bin/sh -c. stdout and stderr are captured (output is truncated if "
    "very long). Runs from the current working directory; state such as cd "
    "and environment variables does not persist between calls.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"The shell command "
    "to run\"}},\"required\":[\"command\"]}}";

int tool_bash(const char *command, buf *out, int (*poll)(void))
{
    int pfd[2];
    if (pipe(pfd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        /* own process group: cancelling kills the whole job, not just the
         * shell — otherwise grandchildren keep the pipe open and we hang */
        setpgid(0, 0);
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        signal(SIGINT, SIG_DFL);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid); /* also set in the parent, to close the race */
    close(pfd[1]);

    const int watch_input = poll && ui_is_tty();
    int truncated = 0;
    int killed = 0;
    int kill_ticks = 0;
    for (;;) {
        if (poll && poll())
            http_interrupted = 1;
        if (http_interrupted) {
            if (!killed) {
                kill(-pid, SIGTERM);
                killed = 1;
            } else if (++kill_ticks == 8) { /* ~1s grace, then insist */
                kill(-pid, SIGKILL);
            }
        }
        fd_set rf;
        struct timeval tv = {0, 120 * 1000};
        int maxfd = pfd[0];
        FD_ZERO(&rf);
        FD_SET(pfd[0], &rf);
        if (watch_input) { /* keep typing responsive while the command runs */
            FD_SET(STDIN_FILENO, &rf);
            if (STDIN_FILENO > maxfd)
                maxfd = STDIN_FILENO;
        }
        int r = select(maxfd + 1, &rf, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            continue;
        if (watch_input && FD_ISSET(STDIN_FILENO, &rf) &&
            !FD_ISSET(pfd[0], &rf))
            continue; /* poll() at the top of the loop handled it */
        char tmp[4096];
        ssize_t n = read(pfd[0], tmp, sizeof tmp);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break; /* EOF */
        if (!truncated && buf_append(out, tmp, (size_t)n) != 0)
            truncated = 1; /* keep draining so the child can exit */
    }
    close(pfd[0]);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (truncated)
        out->overflow = 1;
    if (killed)
        return 130;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return -1;
}

/* ---- dispatch ----------------------------------------------------------- */

void tools_emit_json(buf *b, int with_bash)
{
    int n = 0;
    buf_appends(b, "[");
    if (with_bash) {
        buf_appends(b, TOOL_BASH_JSON);
        n++;
    }
    for (int i = 0; i < plugin_count(); i++) {
        const lambda_tool *t = plugin_get(i);
        char why[128] = "";
        if (t->available && !t->available(why, sizeof why))
            continue; /* e.g. no api key configured */
        if (n++)
            buf_appends(b, ",");
        buf_appends(b, "{\"name\":");
        buf_append_json_str(b, t->name);
        buf_appends(b, ",\"description\":");
        buf_append_json_str(b, t->description ? t->description : "");
        buf_appends(b, ",\"input_schema\":");
        buf_appends(b, t->schema ? t->schema
                                 : "{\"type\":\"object\",\"properties\":{}}");
        buf_appends(b, "}");
    }
    buf_appends(b, "]");
}

void tools_label(const char *name, const char *args_json, char *out,
                 size_t cap)
{
    const lambda_tool *t = plugin_find(name);
    if (t && t->label) {
        t->label(args_json, out, cap);
        if (*out)
            return;
    }
    snprintf(out, cap, "%s %s", name, args_json ? args_json : "{}");
}

int tools_run(const char *name, const char *args_json, buf *out,
              int (*poll)(void))
{
    if (strcmp(name, "bash") == 0) {
        /* the command was already extracted by the caller */
        return tool_bash(args_json, out, poll);
    }
    const lambda_tool *t = plugin_find(name);
    if (!t) {
        buf_appendf(out, "no such tool: %s", name);
        return -1;
    }
    return t->run(args_json, out);
}
