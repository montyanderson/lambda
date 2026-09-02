#ifndef LAMBDA_UI_H
#define LAMBDA_UI_H

#include <stddef.h>

/* transcript item kinds (styling) */
enum {
    UI_USER,      /* what you typed */
    UI_ASSISTANT, /* model reply */
    UI_THINKING,  /* summarised reasoning, shown while the model works */
    UI_TOOL_CMD,  /* a shell command the model ran (grey) */
    UI_TOOL_OUT,  /* its output (grey) */
    UI_NOTICE,    /* dim status/info */
    UI_ERROR,     /* red */
};

int ui_is_tty(void);
void ui_init(const char *model); /* enters fullscreen tui if on a tty */
void ui_shutdown(void);
void ui_set_model(const char *model);

/* completed transcript items */
void ui_add(int kind, const char *text);

/* streaming model output; kind is UI_ASSISTANT or UI_THINKING */
void ui_stream_begin(int kind);
void ui_stream_delta(const char *text);
void ui_stream_end(void);

/* bottom status line; NULL clears. ui_status is transient (spinner) and is
 * suppressed in headless mode; ui_status_final also prints there. */
void ui_status(const char *text);
void ui_status_final(const char *text);

/* Pump input events + redraw while waiting on the network or a command.
 * Keystrokes keep editing the input box; pressing enter queues that prompt
 * instead of sending it. Returns 1 if the user asked to interrupt (ctrl-c). */
int ui_pump(void);

/* Take the oldest queued prompt, if any. 1 on success. */
int ui_take_queued(char *out, size_t cap);
int ui_queued_count(void);

/* right-hand indicators in the top border ("" clears) */
void ui_badge(const char *text);

/* Modal list over the transcript: up/down or 1-9 to move, enter to choose,
 * esc to cancel. `notes[i]` is optional trailing detail and may be NULL, as
 * may `notes` itself. Returns the chosen index, or -1 if cancelled or if
 * there is no tui. */
int ui_pick(const char *title, const char *const *items,
            const char *const *notes, int n, int cur);

/* ui_render coalesces repaints (~40fps ceiling); ui_render_force paints now */
void ui_render(void);
void ui_render_force(void);

/* read one line into out (cap incl. NUL); returns 1 on Enter, 0 on quit/EOF.
 * handles history, cursor motion, and scrollback while composing. */
int ui_readline(const char *prompt, char *out, size_t cap);

#endif
