#ifndef LAMBDA_HERDR_H
#define LAMBDA_HERDR_H

#include "api.h"

/* Optional native Herdr integration. No-op unless the pane environment is
 * complete. Reports use static buffers and at most 50 ms of local socket IO. */
void herdr_init(int argc, char **argv);
void herdr_report(chat *c, int show_thinking, const char *state, const char *message);
void herdr_release(void);

#endif
