#ifndef LAMBDA_VOICE_H
#define LAMBDA_VOICE_H

#include <stddef.h>

int voice_enable(int enabled, char *err, size_t cap);
int voice_enabled(void);
void voice_begin(void); /* cancel previous audio, start this reply */
void voice_text(const char *text); /* nonblocking; assistant text only */
void voice_end(void); /* finish queued text; playback continues */
int voice_poll(void); /* service IPC; returns 1 while playing */
const char *voice_error(void); /* consume a pending, sanitised error */
void voice_stop(void); /* cancel network + player, discard queued text */

#endif
