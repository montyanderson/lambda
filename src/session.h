#ifndef LAMBDA_SESSION_H
#define LAMBDA_SESSION_H

#include <stddef.h>

#include "api.h"

/* Newline-delimited JSON transcript under ./.lambda/chats/.
 *
 * Records are appended to an in-memory queue as the conversation happens and
 * written out only at idle points (see session_flush), so neither the render
 * loop nor an in-flight request ever waits on the disk.
 *
 * Record shapes, one JSON object per line:
 *   {"t":"session","v":1,"time":...,"model":...,"cwd":...}
 *   {"t":"msg","m":{"role":...,"content":...}}   verbatim api message
 *   {"t":"drop","n":N}                           last N msgs were rolled back
 *   {"t":"meta","k":...,"v":...}                 model/system changes
 */

/* Start a new transcript. 0 on success. */
int session_open(chat *c);

/* Continue an existing transcript (appends to it). 0 on success. */
int session_resume(const char *path, chat *c);

/* Most recent transcript in ./.lambda/chats; 0 if found. */
int session_find_last(char *out, size_t cap);

/* Bring the log in line with chat history — appends newly committed messages
 * and records rollbacks. Cheap; call after every history change. */
void session_sync(chat *c);

void session_meta(const char *key, const char *value);

/* Write queued bytes. Call only where a brief disk wait is harmless. */
void session_flush(void);
void session_close(void);

const char *session_path(void); /* "" when logging is off */
int session_enabled(void);
void session_disable(void);

#endif
