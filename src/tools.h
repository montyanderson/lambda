#ifndef LAMBDA_TOOLS_H
#define LAMBDA_TOOLS_H

#include "util.h"

/* Run `command` via /bin/sh -c, capturing stdout+stderr into out (truncated
 * at its capacity, with a marker appended). Returns the exit status (or 128+
 * signal), -1 on spawn failure.
 *
 * `poll` is called several times a second while the command runs; it should
 * service the UI and return non-zero to request cancellation, which sends the
 * child SIGTERM. May be NULL. This is how ctrl-c reaches a running command:
 * the tui holds the terminal in raw mode, so ctrl-c arrives as a keystroke
 * rather than as a signal. */
int tool_bash(const char *command, buf *out, int (*poll)(void));

/* JSON fragment describing the bash tool for the request "tools" array */
extern const char *TOOL_BASH_JSON;

/* Emit the whole "tools" array value, bash plus every available plugin.
 * `with_bash` mirrors the /tools toggle. */
void tools_emit_json(buf *b, int with_bash);

/* One line describing the call, for the transcript. */
void tools_label(const char *name, const char *args_json, char *out,
                 size_t cap);

/* Dispatch a tool_use by name. Returns 0 on success, non-zero on error
 * (`out` holds the text to send back either way). */
int tools_run(const char *name, const char *args_json, buf *out,
              int (*poll)(void));

#endif
