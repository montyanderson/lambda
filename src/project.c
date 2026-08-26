/* Collect AGENTS.md / CLAUDE.md from the working directory upward.
 *
 * Outermost directory first, so the nearest (most specific) file lands last
 * and takes precedence when instructions conflict. */

#include "project.h"
#include "config.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char g_text[LAMBDA_CONTEXT_MAX];
static buf g_buf;
static char g_paths[LAMBDA_CONTEXT_FILES][LAMBDA_PATH_MAX];
static int g_count;
static int g_loaded;

static const char *FILENAMES[] = {"AGENTS.md", "CLAUDE.md", NULL};

const char *project_context(void) { return g_loaded ? g_buf.data : ""; }
int project_count(void) { return g_count; }
int project_truncated(void) { return g_loaded && g_buf.overflow; }

const char *project_path(int i)
{
    return (i >= 0 && i < g_count) ? g_paths[i] : "";
}

static int load_one(const char *dir, const char *name)
{
    char path[LAMBDA_PATH_MAX];
    if (g_count >= LAMBDA_CONTEXT_FILES)
        return 0;
    int n = snprintf(path, sizeof path, "%s/%s",
                     strcmp(dir, "/") == 0 ? "" : dir, name);
    if (n < 0 || (size_t)n >= sizeof path)
        return 0;

    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;

    buf_appendf(&g_buf, "--- %s ---\n", path);
    char tmp[4096];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_append(&g_buf, tmp, r);
    fclose(f);
    if (g_buf.len > 0 && g_buf.data[g_buf.len - 1] != '\n')
        buf_appends(&g_buf, "\n");
    buf_appends(&g_buf, "\n");

    snprintf(g_paths[g_count], LAMBDA_PATH_MAX, "%s", path);
    g_count++;
    return 1;
}

int project_load(void)
{
    if (g_loaded)
        return g_count;
    g_loaded = 1;
    buf_attach(&g_buf, g_text, sizeof g_text);

    char cwd[LAMBDA_PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        return 0;

    /* record each ancestor directory, nearest first */
    static char dirs[LAMBDA_CONTEXT_FILES * 4][LAMBDA_PATH_MAX];
    int ndirs = 0;
    for (;;) {
        if (ndirs >= (int)(sizeof dirs / sizeof dirs[0]))
            break;
        snprintf(dirs[ndirs++], LAMBDA_PATH_MAX, "%s", cwd);
        if (strcmp(cwd, "/") == 0)
            break;
        char *slash = strrchr(cwd, '/');
        if (!slash)
            break;
        if (slash == cwd)
            cwd[1] = '\0'; /* "/foo" -> "/" */
        else
            *slash = '\0';
    }

    buf_appends(&g_buf,
                "The following project context files were found in and above "
                "the working directory. Later files are more specific and "
                "take precedence.\n\n");
    size_t header_len = g_buf.len;

    /* outermost first */
    for (int i = ndirs - 1; i >= 0; i--)
        for (int j = 0; FILENAMES[j]; j++)
            load_one(dirs[i], FILENAMES[j]);

    if (g_count == 0)
        buf_reset(&g_buf); /* nothing found: contribute nothing at all */
    else
        (void)header_len;
    return g_count;
}
