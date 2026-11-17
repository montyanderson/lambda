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
 *   {"t":"compact","drop":N,"pin":I,"summary":...} prefix -> memory
 *   {"t":"clear"}                                clear messages + memory
 */

/* Start a new transcript. 0 on success. */
int session_open(chat *c);

/* Continue an existing transcript (appends to it). Returns restored message
 * count, or -1 (errno=EBUSY if another process owns it). Locked until close. */
int session_resume(const char *path, chat *c);

/* Most recent transcript in ./.lambda/chats; 0 if found. */
int session_find_last(char *out, size_t cap);

/* Bring the log in line with chat history — appends newly committed messages
 * and records rollbacks. Cheap; call after every history change. */
void session_sync(chat *c);
/* Atomically replace a prefix with chat_summary(c), retaining an optional
 * pinned message from that prefix and the untouched tail. Originals stay on disk. */
void session_compact(chat *c, int drop, int pin);
void session_clear(void);

void session_meta(const char *key, const char *value);

/* Write queued bytes. Call only where a brief disk wait is harmless. */
void session_flush(void);
void session_close(void);

const char *session_path(void); /* "" when logging is off */
int session_enabled(void);
void session_disable(void);

#endif
