#ifndef LAMBDA_PLUGIN_H
#define LAMBDA_PLUGIN_H

#include <stddef.h>

#include "util.h"

/* Plugin tools.
 *
 * A plugin is a single .c file in plugins/. Dropping it there is the whole
 * build step — the Makefile globs the directory, and each file registers
 * itself at startup, so there is no registry to edit and no codegen.
 *
 * Minimal plugin:
 *
 *   #include "plugin.h"
 *   static int run(const char *args_json, buf *out) {
 *       buf_appends(out, "hello");
 *       return 0;
 *   }
 *   static const lambda_tool hello = {
 *       .name = "hello", .description = "Say hello",
 *       .schema = "{\"type\":\"object\",\"properties\":{}}",
 *       .run = run,
 *   };
 *   LAMBDA_TOOL_REGISTER(hello)
 */
typedef struct {
    const char *name;
    const char *description;
    const char *schema; /* the tool's input_schema, as a JSON object */

    /* Execute the call. `args_json` is the raw tool input object. Write the
     * result the model should see into `out`. Return 0 on success, non-zero
     * to mark the result as an error (the text in `out` is still sent). */
    int (*run)(const char *args_json, buf *out);

    /* Optional. Return 0 to hide the tool this run — e.g. no API key — and
     * write a short reason into `why`. */
    int (*available)(char *why, size_t whysz);

    /* Optional. One-line summary of the call for the transcript. */
    void (*label)(const char *args_json, char *out, size_t cap);
} lambda_tool;

void plugin_register(const lambda_tool *t);
int plugin_count(void);
const lambda_tool *plugin_get(int i);
const lambda_tool *plugin_find(const char *name);

/* Convenience for plugins: one-shot HTTPS POST. The response body is
 * appended to `out` and the HTTP status returned via `status`. */
int plugin_https_post(const char *host, const char *path, const char *headers,
                      const char *body, buf *out, int *status, char *err,
                      size_t errsz);

#define LAMBDA_TOOL_REGISTER(sym)                                            \
    __attribute__((constructor)) static void sym##_register_(void)           \
    {                                                                        \
        plugin_register(&sym);                                               \
    }

#endif
