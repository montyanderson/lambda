#ifndef LAMBDA_PROJECT_H
#define LAMBDA_PROJECT_H

/* Project context files: AGENTS.md and CLAUDE.md, collected by walking from
 * the current directory up to the filesystem root. These are placed at the
 * front of the context window (the system prompt) so they are both the first
 * thing the model reads and a stable prefix for prompt caching. */

/* Load them. Safe to call once at startup; returns the number of files. */
int project_load(void);

/* Concatenated text, "" when nothing was found. */
const char *project_context(void);

int project_count(void);
const char *project_path(int i); /* path of the i'th loaded file */
int project_truncated(void);     /* 1 if the context cap was hit */

#endif
