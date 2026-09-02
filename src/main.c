/* lambda — minimal agent harness. */

#include "api.h"
#include "config.h"
#include "http.h"
#include "project.h"
#include "session.h"
#include "ui.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LAMBDA_VERSION "0.7.0"

static void on_sigint(int sig)
{
    (void)sig;
    http_interrupted = 1;
}

static void usage(void)
{
    printf("lambda %s — minimal agent harness\n\n"
           "usage: lambda [options]\n"
           "       lambda resume [FILE]    resume the last chat (or FILE)\n"
           "       lambda -p \"prompt\"      one-shot, print reply to stdout\n"
           "       echo \"prompt\" | lambda  one-shot from stdin\n\n"
           "options:\n"
           "  -m MODEL       model id (default: %s)\n"
           "  -s SYSTEM      system prompt\n"
           "  -p PROMPT      one-shot prompt, then exit\n"
           "  -r, --resume F resume a transcript (.lambda/chats/*.jsonl)\n"
           "  -c, --continue resume the most recent transcript\n"
           "  -e EFFORT      low|medium|high|xhigh|max (default: api's own)\n"
           "  --no-thinking  hide the model's reasoning while it works\n"
           "  --no-tools     disable the bash tool\n"
           "  --no-log       do not record a transcript\n"
           "  --no-context   ignore AGENTS.md / CLAUDE.md\n"
           "  --no-fallback  disable server-side refusal fallbacks\n"
           "  -v, --version  print version\n"
           "  -h, --help     this help\n\n"
           "environment:\n"
           "  ANTHROPIC_API_KEY      api key (x-api-key)\n"
           "  ANTHROPIC_AUTH_TOKEN   bearer token (used if no api key)\n"
           "  SSL_CERT_FILE          override CA bundle path\n"
           "  NO_COLOR               disable colors (non-tty output)\n\n"
           "transcripts are written to ./.lambda/chats/ as newline-delimited\n"
           "json, one record per line, and can be resumed with -c or -r.\n",
           LAMBDA_VERSION, LAMBDA_DEFAULT_MODEL);
}

static const char *HELP_TEXT =
    "commands:\n"
    "  /model [ID]   pick a model from a list, or switch to ID\n"
    "  /system TEXT  set system prompt (empty clears)\n"
    "  /effort [L]   show or set reasoning effort\n"
    "  /thinking     toggle showing the model's reasoning\n"
    "  /tools        toggle the bash tool\n"
    "  /clear        clear conversation history\n"
    "  /help         this help\n"
    "  /quit         exit\n"
    "\n"
    "keys: enter send · pgup/pgdn or wheel scroll · ctrl-c interrupt\n"
    "      ctrl-d quit · ctrl-a/e/k/u/w edit · up/down history";

static int show_thinking = 1;

/* right-hand top-border indicators for anything not at its default */
static void refresh_badge(chat *c)
{
    char b[128];
    buf t;
    buf_attach(&t, b, sizeof b);
    if (chat_effort(c)[0])
        buf_appendf(&t, "effort:%s ", chat_effort(c));
    if (!chat_tools_enabled(c))
        buf_appends(&t, "no-tools ");
    if (!show_thinking)
        buf_appends(&t, "quiet ");
    if (t.len && b[t.len - 1] == ' ')
        b[t.len - 1] = '\0';
    ui_badge(b);
}

static void set_model(chat *c, const char *id)
{
    chat_set_model(c, id);
    ui_set_model(id);
    session_meta("model", id);
}

/* `/model` with no argument: pick from the shortlist. Returns 0 if the user
 * backed out, so the caller can leave the transcript untouched. */
static int choose_model(chat *c)
{
    int n = chat_model_count();
    const char *ids[16];
    const char *notes[16];
    char noteb[16][96];
    int cur = -1;

    if (n > (int)(sizeof ids / sizeof ids[0]))
        n = (int)(sizeof ids / sizeof ids[0]);
    for (int i = 0; i < n; i++) {
        const model_info *m = chat_model_at(i);
        int is_cur = strcmp(m->id, chat_model(c)) == 0;
        if (is_cur)
            cur = i;
        ids[i] = m->id;
        snprintf(noteb[i], sizeof noteb[i], "%s%s",
                 is_cur ? "current · " : "", m->note);
        notes[i] = noteb[i];
    }

    int sel = ui_pick("select model", ids, notes, n, cur);
    if (sel < 0)
        return 0;
    if (sel != cur)
        set_model(c, ids[sel]);
    return 1;
}

static int slash_command(chat *c, const char *line)
{
    char msg[512];
    if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0 ||
        strcmp(line, "/q") == 0)
        return 1;
    if (strcmp(line, "/help") == 0) {
        ui_add(UI_NOTICE, HELP_TEXT);
    } else if (strcmp(line, "/clear") == 0) {
        chat_clear(c);
        session_sync(c); /* records a drop, so resume won't resurrect it */
        ui_add(UI_NOTICE, "history cleared");
    } else if (strncmp(line, "/effort", 7) == 0) {
        const char *arg = line + 7;
        while (*arg == ' ')
            arg++;
        if (*arg) {
            if (strcmp(arg, "low") && strcmp(arg, "medium") &&
                strcmp(arg, "high") && strcmp(arg, "xhigh") &&
                strcmp(arg, "max")) {
                ui_add(UI_NOTICE, "effort must be low|medium|high|xhigh|max");
                return 0;
            }
            chat_set_effort(c, arg);
            session_meta("effort", arg);
            refresh_badge(c);
        }
        snprintf(msg, sizeof msg, "effort: %s",
                 chat_effort(c)[0] ? chat_effort(c) : "(api default)");
        ui_add(UI_NOTICE, msg);
    } else if (strcmp(line, "/thinking") == 0) {
        show_thinking = !show_thinking;
        chat_set_show_thinking(c, show_thinking);
        refresh_badge(c);
        ui_add(UI_NOTICE, show_thinking ? "showing reasoning"
                                        : "reasoning hidden");
    } else if (strcmp(line, "/tools") == 0) {
        chat_set_tools(c, !chat_tools_enabled(c));
        refresh_badge(c);
        snprintf(msg, sizeof msg, "bash tool %s",
                 chat_tools_enabled(c) ? "enabled" : "disabled");
        ui_add(UI_NOTICE, msg);
    } else if (strncmp(line, "/model", 6) == 0) {
        const char *arg = line + 6;
        while (*arg == ' ')
            arg++;
        if (*arg)
            set_model(c, arg);
        else if (choose_model(c) == 0)
            return 0; /* cancelled: leave the transcript alone */
        snprintf(msg, sizeof msg, "model: %s", chat_model(c));
        ui_add(UI_NOTICE, msg);
    } else if (strncmp(line, "/system", 7) == 0) {
        const char *arg = line + 7;
        while (*arg == ' ')
            arg++;
        chat_set_system(c, arg);
        session_meta("system", arg);
        snprintf(msg, sizeof msg, "system prompt %s", *arg ? "set" : "cleared");
        ui_add(UI_NOTICE, msg);
    } else {
        ui_add(UI_NOTICE, "unknown command (try /help)");
    }
    return 0;
}

static char g_stdin_buf[LAMBDA_STDIN_MAX];

static char *read_all_stdin(void)
{
    buf b;
    buf_attach(&b, g_stdin_buf, sizeof g_stdin_buf);
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0)
        buf_append(&b, tmp, n);
    while (b.len > 0 && (b.data[b.len - 1] == '\n' || b.data[b.len - 1] == '\r'))
        b.data[--b.len] = '\0';
    return b.data;
}

static char g_line[LAMBDA_LINE_MAX];

int main(int argc, char **argv)
{
    chat *c = chat_get();
    chat_init(c);
    const char *oneshot = NULL;
    const char *resume_path = NULL;
    /* explicit flags outrank whatever a resumed transcript recorded */
    const char *cli_model = NULL, *cli_system = NULL;
    int continue_last = 0, no_log = 0, no_context = 0;

    /* bare `lambda resume [file]` — resumes the most recent chat by default */
    int argi = 1;
    if (argc > 1 && strcmp(argv[1], "resume") == 0) {
        argi = 2;
        if (argc > 2 && argv[2][0] != '-')
            resume_path = argv[argi++];
        else
            continue_last = 1;
    }

    for (int i = argi; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("lambda %s\n", LAMBDA_VERSION);
            return 0;
        } else if (strcmp(a, "--no-fallback") == 0) {
            chat_set_fallbacks(c, 0);
        } else if (strcmp(a, "--no-tools") == 0) {
            chat_set_tools(c, 0);
        } else if (strcmp(a, "--no-thinking") == 0) {
            show_thinking = 0;
            chat_set_show_thinking(c, 0);
        } else if (strcmp(a, "-e") == 0 && i + 1 < argc) {
            chat_set_effort(c, argv[++i]);
        } else if (strcmp(a, "--no-log") == 0) {
            no_log = 1;
        } else if (strcmp(a, "--no-context") == 0) {
            no_context = 1;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--continue") == 0) {
            continue_last = 1;
        } else if ((strcmp(a, "-r") == 0 || strcmp(a, "--resume") == 0) &&
                   i + 1 < argc) {
            resume_path = argv[++i];
        } else if (strcmp(a, "-m") == 0 && i + 1 < argc) {
            cli_model = argv[++i];
            chat_set_model(c, cli_model);
        } else if (strcmp(a, "-s") == 0 && i + 1 < argc) {
            cli_system = argv[++i];
            chat_set_system(c, cli_system);
        } else if (strcmp(a, "-p") == 0 && i + 1 < argc) {
            oneshot = argv[++i];
        } else {
            fprintf(stderr, "lambda: unknown option '%s' (try --help)\n", a);
            return 2;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL); /* no SA_RESTART: interrupt blocking io */
    signal(SIGPIPE, SIG_IGN);

    ui_init(chat_model(c));

    /* project context first: it heads the context window */
    if (!no_context && project_load() > 0) {
        chat_set_prelude(c, project_context());
        char msg[512];
        buf b;
        buf_attach(&b, msg, sizeof msg);
        buf_appends(&b, "context: ");
        for (int i = 0; i < project_count(); i++)
            buf_appendf(&b, "%s%s", i ? ", " : "", project_path(i));
        if (project_truncated())
            buf_appends(&b, " (truncated)");
        ui_add(UI_NOTICE, msg);
    }

    /* transcript: resume an existing one, or start a new one */
    char last[LAMBDA_PATH_MAX];
    if (continue_last && !resume_path) {
        if (session_find_last(last, sizeof last) == 0)
            resume_path = last;
        else
            ui_add(UI_NOTICE, "no previous transcript to continue");
    }
    if (!no_log) {
        char msg[LAMBDA_PATH_MAX + 128];
        if (resume_path) {
            snprintf(msg, sizeof msg, "resuming %s", resume_path);
            ui_add(UI_NOTICE, msg);
            int n = session_resume(resume_path, c);
            if (n < 0) {
                snprintf(msg, sizeof msg, "cannot resume %s", resume_path);
                ui_add(UI_ERROR, msg);
            } else {
                if (cli_model)
                    chat_set_model(c, cli_model);
                if (cli_system)
                    chat_set_system(c, cli_system);
                ui_set_model(chat_model(c));
                snprintf(msg, sizeof msg, "— %d messages restored —", n);
                ui_add(UI_NOTICE, msg);
            }
        } else if (session_open(c) == 0) {
            snprintf(msg, sizeof msg, "recording to %s", session_path());
            ui_add(UI_NOTICE, msg);
        } else {
            ui_add(UI_NOTICE, "could not open a transcript; not recording");
        }
    } else if (resume_path) {
        /* resume the history without recording anything further */
        if (session_resume(resume_path, c) >= 0) {
            if (cli_model)
                chat_set_model(c, cli_model);
            if (cli_system)
                chat_set_system(c, cli_system);
            ui_set_model(chat_model(c));
            session_disable();
        }
    }

    if (oneshot) {
        int rc = chat_send(c, oneshot);
        session_close();
        ui_shutdown();
        return rc == 0 ? 0 : 1;
    }
    if (!ui_is_tty()) {
        char *prompt = read_all_stdin();
        int rc = *prompt ? chat_send(c, prompt) : 0;
        session_close();
        ui_shutdown();
        return rc == 0 ? 0 : 1;
    }

    refresh_badge(c);

    /* Test hook: seed the transcript so the scroll/repaint tests can run
     * without an api key. Env-only, so it stays off the cli surface. */
    {
        const char *md = getenv("LAMBDA_SELFTEST_MD");
        if (md && *md == '1')
            ui_add(UI_ASSISTANT,
                   "here is **the** shortlist:\n"
                   "\n"
                   "| model | ctx | price |\n"
                   "|---|:--:|------:|\n"
                   "| `claude-opus-5` | 1m | $5/$25 |\n"
                   "| claude-haiku-4-5 | 200k | $1/$5 |\n"
                   "\n"
                   "that is all.");
        if (md && *md == '2') /* a table inside a fence stays verbatim */
            ui_add(UI_ASSISTANT,
                   "```\n"
                   "| fenced | table |\n"
                   "|---|---|\n"
                   "| stays | verbatim |\n"
                   "```");
        const char *fill = getenv("LAMBDA_SELFTEST_FILL");
        if (fill && *fill) {
            long n = strtol(fill, NULL, 10);
            char line[160];
            for (long i = 0; i < n; i++) {
                snprintf(line, sizeof line,
                         "selftest %ld — the quick brown fox jumps over the "
                         "lazy dog %ld", i, i);
                ui_add(i % 3 == 0 ? UI_TOOL_OUT : UI_ASSISTANT, line);
            }
        }
    }

    ui_add(UI_NOTICE,
           "λ lambda — type a message, /help for commands, ctrl-d to quit");

    /* the transcript queue is drained here, where lambda is about to block on
     * the keyboard and a disk wait costs nothing */
    for (session_flush(); ui_readline("❯ ", g_line, sizeof g_line);
         session_flush()) {
        char *s = g_line;
        while (*s == ' ' || *s == '\t')
            s++;
        size_t len = strlen(s);
        while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
            s[--len] = '\0';
        if (!*s)
            continue;

        if (*s == '/') {
            if (slash_command(c, s))
                break;
            continue;
        }
        ui_add(UI_USER, s);
        chat_send(c, s);

        /* prompts typed while that turn ran are sent now, in order */
        while (ui_take_queued(g_line, sizeof g_line)) {
            session_flush();
            if (g_line[0] == '/') {
                if (slash_command(c, g_line))
                    goto done;
                continue;
            }
            ui_add(UI_USER, g_line);
            chat_send(c, g_line);
        }
    }
done:;

    session_close();
    ui_shutdown();
    return 0;
}
