/* Plugin registry and shared helpers. */

#include "plugin.h"
#include "config.h"
#include "http.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

static const lambda_tool *g_tools[LAMBDA_PLUGINS_MAX];
static int g_ntools;

void plugin_register(const lambda_tool *t)
{
    if (!t || !t->name || !t->run || g_ntools >= LAMBDA_PLUGINS_MAX)
        return;
    for (int i = 0; i < g_ntools; i++)
        if (strcmp(g_tools[i]->name, t->name) == 0)
            return; /* first registration of a name wins */
    g_tools[g_ntools++] = t;
}

int plugin_count(void) { return g_ntools; }

const lambda_tool *plugin_get(int i)
{
    return (i >= 0 && i < g_ntools) ? g_tools[i] : NULL;
}

const lambda_tool *plugin_find(const char *name)
{
    for (int i = 0; i < g_ntools; i++)
        if (strcmp(g_tools[i]->name, name) == 0)
            return g_tools[i];
    return NULL;
}

/* ---- https helper ------------------------------------------------------- */

static void collect(const char *data, size_t len, void *ud)
{
    buf_append((buf *)ud, data, len);
}

static void pump(void *ud)
{
    (void)ud;
    ui_pump(); /* keep the ui alive while the request is in flight */
}

int plugin_https_post(const char *host, const char *path, const char *headers,
                      const char *body, buf *out, int *status, char *err,
                      size_t errsz)
{
    http_request req = {
        .host = host,
        .port = "443",
        .path = path,
        .extra_headers = headers,
        .body = body,
        .body_len = strlen(body),
        .status = status,
        .on_body = collect,
        .body_ud = out,
        .on_idle = pump,
        .idle_ud = NULL,
        .watch_input = ui_is_tty(),
    };
    return http_post(&req, err, errsz);
}
