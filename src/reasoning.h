#ifndef LAMBDA_REASONING_H
#define LAMBDA_REASONING_H

#include <string.h>

#define REASONING_LEVELS 8
typedef struct {
    int supported; /* 0 unknown, 1 yes, -1 no */
    unsigned levels; /* bits indexed by reasoning_name; bit 0 is API default */
    int native_thinking, always_on;
    char default_effort[16];
} reasoning_options;

static inline const char *reasoning_name(int i)
{
    static const char *names[] = {"", "none", "minimal", "low", "medium", "high", "xhigh", "max"};
    return i >= 0 && i < REASONING_LEVELS ? names[i] : NULL;
}

static inline int reasoning_index(const char *name)
{
    if (!name || !strcmp(name, "default")) return 0;
    for (int i = 0; i < REASONING_LEVELS; i++)
        if (!strcmp(name, reasoning_name(i))) return i;
    return -1;
}

#endif
