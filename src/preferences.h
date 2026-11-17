#ifndef LAMBDA_PREFERENCES_H
#define LAMBDA_PREFERENCES_H

#include <stddef.h>

/* Separate defaults for LiteLLM and direct Anthropic; no credentials here.
 * Load returns 1 when found, 0 when absent, -1 on error (errno set). */
int preferences_model_load(int gateway, char *out, size_t cap);
int preferences_model_save(int gateway, const char *model);

#endif
