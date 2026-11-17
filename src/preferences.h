#ifndef LAMBDA_PREFERENCES_H
#define LAMBDA_PREFERENCES_H

#include <stddef.h>
#include "config.h"

typedef struct {
    char model[LAMBDA_MODEL_ID_MAX];
    char effort[16];
    int effort_set; /* legacy model-only files have not answered the picker */
} model_preference;

/* Separate defaults for LiteLLM and direct Anthropic; no credentials here.
 * Load returns 1 when found, 0 when absent, -1 on error (errno set). */
int preferences_load(int gateway, model_preference *out);
int preferences_save(int gateway, const char *model, const char *effort);

#endif
