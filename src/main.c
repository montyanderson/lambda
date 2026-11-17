/* lambda — minimal agent harness. */

#include "api.h"
#include "config.h"
#include "http.h"
#include "gateway.h"
#include "herdr.h"
#include "project.h"
#include "preferences.h"
#include "session.h"
#include "ui.h"
#include "util.h"
#include "voice.h"
#include "tts.h"

#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

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
           "  -m MODEL       model for this run (otherwise saved choice or picker)\n"
           "  --litellm URL  use a LiteLLM proxy (http or https)\n"
           "  --anthropic    use Anthropic directly, ignoring saved LiteLLM config\n"
           "  --models       fetch and print available model ids, then exit\n"
           "  --max-tokens N output limit (LiteLLM: 4096; direct: 64000)\n"
           "  --context-window N override model context size (default: auto)\n"
           "  -s SYSTEM      system prompt\n"
           "  -p PROMPT      one-shot prompt, then exit\n"
           "  -r, --resume F resume a transcript (.lambda/chats/*.jsonl)\n"
           "  -c, --continue resume the most recent transcript\n"
           "  -e EFFORT      override reasoning effort for this run (or default)\n"
           "  --no-thinking  hide the model's reasoning while it works\n"
           "  --voice        speak replies with Inworld (mpv, mpg123 or ffplay)\n"
           "  --voice-id ID  choose an Inworld voice (overrides INWORLD_VOICE_ID)\n"
           "  --no-tools     disable the bash tool\n"
           "  --no-log       do not record a transcript\n"
           "  --no-context   ignore AGENTS.md / CLAUDE.md\n"
           "  --no-fallback  disable server-side refusal fallbacks\n"
           "  -v, --version  print version\n"
           "  -h, --help     this help\n\n"
           "environment:\n"
           "  LITELLM_BASE_URL       LiteLLM proxy URL (enables LiteLLM)\n"
           "  LITELLM_PROXY_URL      alias for LITELLM_BASE_URL\n"
           "  LITELLM_API_KEY        proxy key (optional on unauthenticated proxies)\n"
           "  ANTHROPIC_API_KEY      api key (x-api-key)\n"
           "  ANTHROPIC_AUTH_TOKEN   bearer token (used if no api key)\n"
           "  INWORLD_API_KEY        Inworld key for --voice (Basic, unchanged)\n"
           "  INWORLD_VOICE_ID       speech voice (overrides lambda/voice.json)\n"
           "  SSL_CERT_FILE          override CA bundle path\n"
           "  NO_COLOR               disable colors (non-tty output)\n\n"
           "config: $XDG_CONFIG_HOME/lambda/config.json or ~/.config/lambda/config.json\n"
           "  {\"base_url\":\"http://localhost:4000\",\"api_key\":\"sk-...\"}\n\n"
           "transcripts are written to ./.lambda/chats/ as newline-delimited\n"
           "json, one record per line, and can be resumed with -c or -r.\n",
           LAMBDA_VERSION);
}

static const char *HELP_TEXT =
    "commands:\n"
    "  /model [ID]   fetch models and open a searchable picker, or switch to ID\n"
    "  /models       refresh the available model list\n"
    "  /context      show context usage and automatic summary threshold\n"
    "  /compact      summarise older conversation now\n"
    "  /system TEXT  set system prompt (empty clears)\n"
    "  /effort [L]   pick or set reasoning effort and save it with the model\n"
    "  /thinking     toggle showing the model's reasoning\n"
    "  /voice [on|off|stop] toggle speech, or stop current playback\n"
    "  /tools        toggle the bash tool\n"
    "  /clear        clear conversation history\n"
    "  /help         this help\n"
    "  /quit         exit\n"
    "\n"
    "keys: enter send · pgup/pgdn or wheel scroll · ctrl-c interrupt\n"
    "      ctrl-d quit · ctrl-a/e/k/u/w edit · up/down history";

static int show_thinking = 1;
static int sending;

static int speech_tick(void)
{
    int active = voice_poll();
    /* A notice must not split a live transcript item. */
    if (!sending) {
        const char *err = voice_error();
        if (err) ui_add(UI_ERROR, err);
    }
    return active;
}

static int send_reply(chat *c, const char *text, int wait_audio)
{
    herdr_report(c, show_thinking, "working", NULL);
    speech_tick();
    sending = 1;
    voice_begin();
    int rc = chat_send(c, text);
    if (rc == 0) voice_end();
    else voice_stop();
    sending = 0;
    speech_tick();
    if (wait_audio) {
        while (speech_tick()) {
            ui_pump();
            struct timespec pause = {0, 40000000};
            nanosleep(&pause, NULL);
        }
    }
    session_flush();
    if (rc != 0 && ui_queued_count())
        herdr_report(c, show_thinking, "blocked", "queued prompts paused; press enter to resume");
    else if (rc != 0 && !http_interrupted)
        herdr_report(c, show_thinking, "blocked", "turn failed; retry or send another prompt");
    else
        herdr_report(c, show_thinking, rc == 0 && ui_queued_count() ? "working" : "idle", NULL);
    return rc;
}

/* right-hand top-border indicators for anything not at its default */
static void refresh_badge(chat *c)
{
    char b[128];
    buf t;
    buf_attach(&t, b, sizeof b);
    if (gateway_enabled())
        buf_appends(&t, "litellm ");
    if (chat_effort(c)[0])
        buf_appendf(&t, "effort:%s ", chat_effort(c));
    if (!chat_tools_enabled(c))
        buf_appends(&t, "no-tools ");
    if (!show_thinking)
        buf_appends(&t, "quiet ");
    if (voice_enabled())
        buf_appends(&t, "voice ");
    if (t.len && b[t.len - 1] == ' ')
        b[t.len - 1] = '\0';
    ui_badge(b);
}

static int set_model(chat *c, const char *id)
{
    char previous[LAMBDA_MODEL_ID_MAX];
    snprintf(previous, sizeof previous, "%s", chat_model(c));
    if (chat_set_model(c, id) != 0) {
        ui_add(UI_ERROR, "model id is empty or too long");
        return -1;
    }
    if (chat_prepare_storage(c) != 0) {
        chat_set_model(c, previous);
        ui_add(UI_ERROR, "not enough memory for that model; previous model and history kept");
        return -1;
    }
    ui_set_model(id);
    session_meta("model", id);
    return 0;
}

static void save_selection(chat *c)
{
    if (preferences_save(gateway_enabled(), chat_model(c), chat_effort(c)) != 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "model selected for this chat, but could not save default: %s", strerror(errno));
        ui_add(UI_ERROR, msg);
    }
}

static int pick_effort(const char *id, const char *current, char *out, size_t cap)
{
    reasoning_options options;
    ui_status("loading reasoning options…");
    ui_render_force();
    chat_reasoning_options(id, &options);
    ui_status(NULL);
    if (http_interrupted) return -1;
    out[0] = '\0';
    if (!options.levels || options.supported == -1) {
        ui_add(UI_NOTICE, options.supported == -1 ? "this model does not advertise reasoning controls" :
               "reasoning options unavailable; using the provider default");
        return 0;
    }
    const char *names[REASONING_LEVELS], *notes[REASONING_LEVELS];
    int levels[REASONING_LEVELS], n = 1, cur = 0;
    char default_note[96];
    snprintf(default_note, sizeof default_note, "provider decides%s%s",
             options.default_effort[0] ? " · advertised: " : "", options.default_effort);
    names[0] = "default"; notes[0] = default_note; levels[0] = 0;
    for (int i = 1; i < REASONING_LEVELS; i++) {
        if (!(options.levels & (1u << i))) continue;
        names[n] = reasoning_name(i);
        notes[n] = i == 1 ? "disable reasoning" : i <= 3 ? "less reasoning" :
                   i >= 6 ? "more reasoning" : "";
        levels[n] = i;
        if (current && !strcmp(current, names[n])) cur = n;
        n++;
    }
    char title[160];
    snprintf(title, sizeof title, "reasoning effort · %.100s", id);
    int sel = ui_pick(title, names, notes, n, cur);
    if (sel < 0) return -1;
    snprintf(out, cap, "%s", reasoning_name(levels[sel]));
    return 0;
}

static int effort_allowed(const char *id, const char *effort)
{
    int i = reasoning_index(effort);
    if (i < 0) return 0;
    if (!i) return 1;
    reasoning_options options;
    chat_reasoning_options(id, &options);
    return options.supported != -1 && (!options.levels || (options.levels & (1u << i))) &&
           !(options.always_on && i == 1);
}

static int select_model(chat *c, const char *id)
{
    char effort[16];
    if (pick_effort(id, !strcmp(id, chat_model(c)) ? chat_effort(c) : "",
                    effort, sizeof effort) != 0) return -1;
    if (set_model(c, id) != 0) return -1;
    chat_set_effort(c, effort);
    session_meta("effort", effort);
    refresh_badge(c);
    save_selection(c);
    return 0;
}

static int refresh_models(chat *c)
{
    if (!gateway_enabled())
        return 0;
    char err[512];
    herdr_report(c, show_thinking, "working", NULL);
    ui_status("loading LiteLLM models…");
    ui_render_force();
    int rc = gateway_refresh(err, sizeof err);
    ui_status(NULL);
    if (rc != 0)
        ui_add(UI_ERROR, err);
    else if (!chat_model_count())
        ui_add(UI_NOTICE, "LiteLLM returned no models for this key");
    return rc;
}

/* `/model` with no argument: pick from the shortlist. Returns 0 if the user
 * backed out, so the caller can leave the transcript untouched. */
static int choose_model(chat *c)
{
    int n = chat_model_count();
    static const char *ids[LAMBDA_MODELS_MAX];
    static const char *notes[LAMBDA_MODELS_MAX];
    static char noteb[LAMBDA_MODELS_MAX][160];
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

    herdr_report(c, show_thinking, "blocked", "select a model");
    int sel = ui_pick("select model", ids, notes, n, cur);
    if (sel < 0)
        return 0;
    return select_model(c, ids[sel]) == 0;
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
        voice_stop();
        chat_clear(c);
        session_clear();
        ui_add(UI_NOTICE, "history cleared");
    } else if (strcmp(line, "/context") == 0) {
        long window = chat_context_window(c);
        snprintf(msg, sizeof msg, "context: ~%ld / %ld tokens; auto-summary at %ld%s",
                 chat_context_tokens(c), window, chat_context_threshold(c),
                 chat_summary(c)[0] ? " · earlier work summarised" : "");
        ui_add(UI_NOTICE, msg);
        snprintf(msg, sizeof msg,
                 "history: %zu messages, %.2f mib stored, %.2f mib reserved; %d messages in active context",
                 chat_history_count(c), (double)chat_history_bytes(c) / 1048576.0,
                 (double)chat_history_capacity(c) / 1048576.0, chat_msg_count(c));
        ui_add(UI_NOTICE, msg);
    } else if (strcmp(line, "/compact") == 0) {
        http_interrupted = 0;
        herdr_report(c, show_thinking, "working", NULL);
        int rc = chat_compact(c, msg, sizeof msg);
        if (rc < 0)
            ui_add(UI_ERROR, http_interrupted ? "interrupted; history preserved" : msg);
        else
            ui_add(UI_NOTICE, rc ? "conversation memory updated" : "nothing to summarise yet");
    } else if (!strcmp(line, "/effort") || !strncmp(line, "/effort ", 8)) {
        const char *arg = line + 7;
        while (*arg == ' ')
            arg++;
        char selected[16];
        if (!*arg) {
            if (pick_effort(chat_model(c), chat_effort(c), selected, sizeof selected) != 0) return 0;
            arg = selected;
        }
        if (!effort_allowed(chat_model(c), arg)) {
            ui_add(UI_NOTICE, "unsupported effort for this model; use /effort to see available settings");
            return 0;
        }
        chat_set_effort(c, reasoning_name(reasoning_index(arg)));
        session_meta("effort", chat_effort(c));
        refresh_badge(c);
        save_selection(c);
        snprintf(msg, sizeof msg, "effort: %s",
                 chat_effort(c)[0] ? chat_effort(c) : "(api default)");
        ui_add(UI_NOTICE, msg);
    } else if (strcmp(line, "/thinking") == 0) {
        show_thinking = !show_thinking;
        chat_set_show_thinking(c, show_thinking);
        refresh_badge(c);
        ui_add(UI_NOTICE, show_thinking ? "showing reasoning"
                                        : "reasoning hidden");
    } else if (strcmp(line, "/voice") == 0 || strncmp(line, "/voice ", 7) == 0) {
        const char *arg = line + 6;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "stop") == 0) {
            voice_stop();
            ui_add(UI_NOTICE, "voice playback stopped");
        } else if (!*arg || !strcmp(arg, "on") || !strcmp(arg, "off")) {
            int on = *arg ? !strcmp(arg, "on") : !voice_enabled();
            if (voice_enable(on, msg, sizeof msg) != 0)
                ui_add(UI_ERROR, msg);
            else {
                snprintf(msg, sizeof msg, on ? "voice on · %s · Inworld TTS 2" : "voice off", tts_voice_id());
                ui_add(UI_NOTICE, msg);
            }
            refresh_badge(c);
        } else {
            ui_add(UI_NOTICE, "usage: /voice [on|off|stop]");
        }
    } else if (strcmp(line, "/tools") == 0) {
        chat_set_tools(c, !chat_tools_enabled(c));
        refresh_badge(c);
        snprintf(msg, sizeof msg, "bash tool %s",
                 chat_tools_enabled(c) ? "enabled" : "disabled");
        ui_add(UI_NOTICE, msg);
    } else if (strcmp(line, "/models") == 0) {
        if (refresh_models(c) == 0) {
            snprintf(msg, sizeof msg, "%d models available; /model to select", chat_model_count());
            ui_add(UI_NOTICE, msg);
        }
    } else if (strcmp(line, "/model") == 0 || strncmp(line, "/model ", 7) == 0) {
        const char *arg = line + 6;
        while (*arg == ' ')
            arg++;
        if (*arg) {
            if (select_model(c, arg) != 0)
                return 0;
        } else {
            refresh_models(c);
            if (choose_model(c) == 0)
                return 0;
        }
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
    const char *cli_model = NULL, *cli_system = NULL, *cli_effort = NULL;
    const char *litellm_url = getenv("LITELLM_BASE_URL");
    if (!litellm_url || !*litellm_url)
        litellm_url = getenv("LITELLM_PROXY_URL");
    int list_models = 0, restored = 0, direct = 0;
    int continue_last = 0, no_log = 0, no_context = 0;
    int cli_voice = 0;

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
        } else if (strcmp(a, "--litellm") == 0 && i + 1 < argc) {
            litellm_url = argv[++i];
            direct = 0;
        } else if (strcmp(a, "--anthropic") == 0) {
            direct = 1;
        } else if (strcmp(a, "--models") == 0) {
            list_models = 1;
        } else if (strcmp(a, "--max-tokens") == 0 && i + 1 < argc) {
            char *end;
            const char *value = argv[++i];
            long n = strtol(value, &end, 10);
            if (!*value || *end || n < 2 || n > INT_MAX) {
                fprintf(stderr, "lambda: --max-tokens must be an integer from 2 to %d\n", INT_MAX);
                return 2;
            }
            chat_set_max_tokens(c, (int)n);
        } else if (strcmp(a, "--context-window") == 0 && i + 1 < argc) {
            char *end;
            const char *value = argv[++i];
            long n = strtol(value, &end, 10);
            if (!*value || *end || n < 1024 || n > INT_MAX) {
                fprintf(stderr, "lambda: --context-window must be an integer from 1024 to %d\n", INT_MAX);
                return 2;
            }
            chat_set_context_window(c, n);
        } else if (strcmp(a, "--voice") == 0) {
            cli_voice = 1;
        } else if (strcmp(a, "--voice-id") == 0 && i + 1 < argc) {
            if (setenv("INWORLD_VOICE_ID", argv[++i], 1) != 0) {
                fprintf(stderr, "lambda: cannot set voice id\n");
                return 2;
            }
        } else if (strcmp(a, "--no-tools") == 0) {
            chat_set_tools(c, 0);
        } else if (strcmp(a, "--no-thinking") == 0) {
            show_thinking = 0;
            chat_set_show_thinking(c, 0);
        } else if (strcmp(a, "-e") == 0 && i + 1 < argc) {
            cli_effort = argv[++i];
            if (reasoning_index(cli_effort) < 0) {
                fprintf(stderr, "lambda: effort must be default|none|minimal|low|medium|high|xhigh|max\n");
                return 2;
            }
            chat_set_effort(c, reasoning_name(reasoning_index(cli_effort)));
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
            if (chat_set_model(c, cli_model) != 0) {
                fprintf(stderr, "lambda: model id is empty or too long\n");
                return 2;
            }
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

    char err[512];
    if (!direct && (gateway_load_config(err, sizeof err) != 0 ||
        (litellm_url && *litellm_url && gateway_configure(litellm_url, err, sizeof err) != 0))) {
        fprintf(stderr, "lambda: %s\n", err);
        return 2;
    }
    if (list_models) {
        if (gateway_enabled() && gateway_refresh(err, sizeof err) != 0) {
            fprintf(stderr, "lambda: %s\n", err);
            return 1;
        }
        for (int i = 0; i < chat_model_count(); i++)
            puts(chat_model_at(i)->id);
        return 0;
    }

    model_preference preference;
    int saved = preferences_load(gateway_enabled(), &preference);
    int preference_error = saved < 0 ? errno : 0;
    if (saved == 1 && !cli_model) {
        chat_set_model(c, preference.model);
        if (!cli_effort) chat_set_effort(c, preference.effort);
    }

    if (cli_voice && voice_enable(1, err, sizeof err) != 0) {
        fprintf(stderr, "lambda: %s\n", err);
        return 2;
    }
    atexit(voice_stop);
    ui_set_output_hooks(voice_text, speech_tick, voice_stop);
    ui_init(chat_model(c));
    if (preference_error) {
        snprintf(err, sizeof err, "could not read saved model: %s; select a model again", strerror(preference_error));
        ui_add(UI_NOTICE, err);
    }

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
            chat_set_effort(c, ""); /* the transcript outranks global defaults */
            snprintf(msg, sizeof msg, "resuming %s", resume_path);
            ui_add(UI_NOTICE, msg);
            int n = session_resume(resume_path, c);
            if (n < 0) {
                snprintf(msg, sizeof msg, "cannot resume %s: %s", resume_path,
                         errno == EBUSY ? "already open in another lambda process" : strerror(errno));
                ui_add(UI_ERROR, msg);
                ui_shutdown();
                return 1;
            } else {
                restored = 1;
                if (cli_model)
                    set_model(c, cli_model);
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
        chat_set_effort(c, "");
        /* resume the history without recording anything further */
        if (session_resume(resume_path, c) >= 0) {
            restored = 1;
            if (cli_model)
                chat_set_model(c, cli_model);
            if (cli_system)
                chat_set_system(c, cli_system);
            ui_set_model(chat_model(c));
            session_disable();
        } else {
            char msg[LAMBDA_PATH_MAX + 128];
            snprintf(msg, sizeof msg, "cannot resume %s: %s", resume_path,
                     errno == EBUSY ? "already open in another lambda process" : strerror(errno));
            ui_add(UI_ERROR, msg);
            ui_shutdown();
            return 1;
        }
    }

    if (restored && cli_model) chat_set_effort(c, "");
    if (cli_effort) chat_set_effort(c, reasoning_name(reasoning_index(cli_effort)));

    /* Persist an explicit system prompt for resume without putting its text
     * into Herdr's shell command (it may contain newlines or private context). */
    if (cli_system) session_meta("system", cli_system);
    session_flush();
    herdr_init(argc, argv);

    /* With an explicit/resumed/saved model, defer discovery until /model so an
     * unavailable catalog endpoint never prevents a known model from running. */
    if (!cli_model && !restored && saved != 1 &&
        (gateway_enabled() || (ui_is_tty() && !oneshot))) {
        if (refresh_models(c) != 0 || !chat_model_count()) {
            if (!ui_is_tty() || oneshot) {
                session_close();
                ui_shutdown();
                return 1;
            }
            ui_add(UI_NOTICE, "use /model to retry, or /model ID to select directly");
        } else if (!ui_is_tty() || oneshot) {
            if (chat_model_count() != 1) {
                ui_add(UI_ERROR, "select a LiteLLM model with -m ID (list them with --models)");
                session_close();
                ui_shutdown();
                return 1;
            }
            set_model(c, chat_model_at(0)->id);
        } else {
            choose_model(c);
        }
    }

    if (saved == 1 && !cli_model && !cli_effort && !restored && !preference.effort_set && ui_is_tty() && !oneshot)
        select_model(c, chat_model(c)); /* migrate the old model-only default */

    if (cli_effort) chat_set_effort(c, reasoning_name(reasoning_index(cli_effort)));
    if (!effort_allowed(chat_model(c), chat_effort(c))) {
        if (cli_effort) {
            ui_add(UI_ERROR, "selected reasoning effort is not supported by this model");
            session_close(); ui_shutdown(); return 2;
        }
        ui_add(UI_NOTICE, "saved reasoning effort is no longer supported; using the provider default");
        chat_set_effort(c, "");
        session_meta("effort", "");
    }

    if (chat_prepare_storage(c) != 0) {
        ui_add(UI_ERROR, "not enough memory for the selected model");
        session_close();
        ui_shutdown();
        return 1;
    }

    if (chat_effort(c)[0] || (restored && (cli_model || cli_effort)))
        session_meta("effort", chat_effort(c));

    if (oneshot) {
        int rc = send_reply(c, oneshot, 1);
        session_close();
        ui_shutdown();
        return rc == 0 ? 0 : 1;
    }
    if (!ui_is_tty()) {
        char *prompt = read_all_stdin();
        int rc = *prompt ? send_reply(c, prompt, 1) : 0;
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
        if (md && *md == '3')
            ui_add(UI_ASSISTANT,
                   "- [first link](https://example.com/a_b?q=x_y)\n"
                   "12. [**bold label**](https://example.com/long_path) trailing words\n"
                   "  - [nested](https://example.com/nested_path)\n"
                   "`[literal](url)`");
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
    herdr_report(c, show_thinking, "idle", NULL);

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
        if (!*s) {
            if (!ui_queued_count()) herdr_report(c, show_thinking, "idle", NULL);
            continue;
        }

        if (*s == '/') {
            if (slash_command(c, s))
                break;
            session_flush();
            herdr_report(c, show_thinking, ui_queued_count() ? "working" : "idle", NULL);
            continue;
        }
        ui_add(UI_USER, s);
        if (send_reply(c, s, 0) != 0)
            ui_pause_queued();
        /* ui_readline serves successful turns' queued prompts in FIFO order.
         * All input follows the same trimming/command/error path. */
    }

    session_close();
    ui_shutdown();
    return 0;
}
