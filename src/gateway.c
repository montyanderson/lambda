#include "gateway.h"
#include "config.h"
#include "jsonx.h"
#include "ui.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static struct {
    char url[2048], host[256], port[6], prefix[1536];
    int plain, enabled;
} g_gateway;

typedef struct {
    model_info info;
    char id[LAMBDA_MODEL_ID_MAX];
    char note[128];
} gateway_model;

/* Refresh into the inactive bank, retaining the previous list on errors. */
static gateway_model g_catalog[2][LAMBDA_MODELS_MAX];
static int g_bank, g_count;
static int g_metadata_attempted;
static char g_response[LAMBDA_MODELS_BODY_MAX];
static char g_config_key[LAMBDA_REQ_HDRS_MAX];

int gateway_load_config(char *err, size_t errsz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char path_store[LAMBDA_PATH_MAX];
    buf path;
    buf_attach(&path, path_store, sizeof path_store);
    if (xdg && *xdg)
        buf_appendf(&path, "%s/lambda/config.json", xdg);
    else if (home && *home)
        buf_appendf(&path, "%s/.config/lambda/config.json", home);
    else
        return 0;
    if (path.overflow) {
        snprintf(err, errsz, "lambda config path too long");
        return -1;
    }
    FILE *f = fopen(path.data, "r");
    if (!f) {
        if (errno == ENOENT)
            return 0;
        snprintf(err, errsz, "cannot read lambda config: %s", strerror(errno));
        return -1;
    }
    char data[16384];
    size_t len = fread(data, 1, sizeof data - 1, f);
    int invalid = ferror(f) || !feof(f);
    fclose(f);
    data[len] = '\0';
    jdoc d;
    if (invalid || jdoc_parse(&d, data, len) != 0) {
        snprintf(err, errsz, "lambda config must be a JSON object under 16 KiB");
        return -1;
    }
    char url_store[2048];
    buf url, key;
    buf_attach(&url, url_store, sizeof url_store);
    buf_attach(&key, g_config_key, sizeof g_config_key);
    int ki = j_get(&d, 0, "api_key");
    if (j_str(&d, 0, "base_url", &url) != 0 || url.overflow ||
        strlen(url.data) != url.len ||
        (ki >= 0 && (j_str_tok(&d, ki, &key) != 0 || key.overflow ||
                    strlen(key.data) != key.len))) {
        snprintf(err, errsz, "lambda config needs a base_url string and an optional api_key string");
        return -1;
    }
    return gateway_configure(url.data, err, errsz);
}

int gateway_enabled(void) { return g_gateway.enabled; }
const char *gateway_url(void) { return g_gateway.url; }
int gateway_model_count(void) { return g_count; }
const model_info *gateway_model_at(int i)
{
    return i >= 0 && i < g_count ? &g_catalog[g_bank][i].info : NULL;
}

int gateway_configure(const char *url, char *err, size_t errsz)
{
    int plain = strncmp(url, "http://", 7) == 0;
    if (!plain && strncmp(url, "https://", 8) != 0)
        goto invalid;
    if (strlen(url) >= sizeof g_gateway.url || strpbrk(url, "?#@\\"))
        goto invalid;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 32 || *p == 127)
            goto invalid;
    const char *host = url + (plain ? 7 : 8);
    const char *end = strchr(host, '/');
    if (!end)
        end = host + strlen(host);
    const char *host_end = end, *port = NULL;
    if (*host == '[') {
        host++;
        host_end = memchr(host, ']', (size_t)(end - host));
        if (!host_end)
            goto invalid;
        if (host_end + 1 < end) {
            if (host_end[1] != ':')
                goto invalid;
            port = host_end + 2;
        }
    } else {
        const char *colon = memchr(host, ':', (size_t)(end - host));
        if (colon) {
            host_end = colon;
            port = colon + 1;
        }
    }
    size_t host_len = (size_t)(host_end - host);
    if (!host_len || host_len >= sizeof g_gateway.host)
        goto invalid;
    unsigned long port_num = plain ? 80 : 443;
    if (port) {
        if (port == end || end - port > 5)
            goto invalid;
        port_num = 0;
        for (const char *p = port; p < end; p++) {
            if (*p < '0' || *p > '9')
                goto invalid;
            port_num = port_num * 10 + (unsigned)(*p - '0');
        }
        if (!port_num || port_num > 65535)
            goto invalid;
    }
    size_t prefix_len = strlen(end);
    while (prefix_len && end[prefix_len - 1] == '/')
        prefix_len--;
    if (prefix_len >= 3 && memcmp(end + prefix_len - 3, "/v1", 3) == 0)
        prefix_len -= 3;
    if (prefix_len >= sizeof g_gateway.prefix)
        goto invalid;

    memcpy(g_gateway.host, host, host_len);
    g_gateway.host[host_len] = '\0';
    snprintf(g_gateway.port, sizeof g_gateway.port, "%lu", port_num);
    memcpy(g_gateway.prefix, end, prefix_len);
    g_gateway.prefix[prefix_len] = '\0';
    snprintf(g_gateway.url, sizeof g_gateway.url, "%s", url);
    g_gateway.plain = plain;
    g_gateway.enabled = 1;
    g_count = 0;
    g_metadata_attempted = 0;
    return 0;
invalid:
    snprintf(err, errsz, "invalid LiteLLM URL: use http(s)://host[:port][/prefix] without credentials, query or fragment");
    return -1;
}

int gateway_headers(buf *headers, char *err, size_t errsz)
{
    const char *key = getenv("LITELLM_API_KEY");
    if (!key)
        key = g_config_key;
    if (key && *key) {
        /* Never reuse Anthropic credentials for a different host. */
        for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
            if (*p <= 32 || *p == 127) {
                snprintf(err, errsz, "LITELLM_API_KEY contains whitespace or control characters");
                return -1;
            }
        }
        buf_appendf(headers, "Authorization: Bearer %s\r\n", key);
    }
    if (headers->overflow) {
        snprintf(err, errsz, "LiteLLM authorization header too large");
        return -1;
    }
    return 0;
}

int gateway_route(http_request *req, const char *endpoint, buf *path)
{
    buf_appendf(path, "%s/v1/%s", g_gateway.prefix, endpoint);
    req->host = g_gateway.host;
    req->port = g_gateway.port;
    req->plain_http = g_gateway.plain;
    req->path = path->data;
    return path->overflow ? -1 : 0;
}

static void receive_models(const char *data, size_t len, void *ud)
{
    buf_append(ud, data, len);
}

static void discovery_idle(void *ud)
{
    (void)ud;
    ui_pump();
}

int gateway_refresh(char *err, size_t errsz)
{
    g_metadata_attempted = 1;
    char header_store[LAMBDA_REQ_HDRS_MAX], path_store[2048];
    buf headers, path, response;
    buf_attach(&headers, header_store, sizeof header_store);
    buf_attach(&path, path_store, sizeof path_store);
    buf_attach(&response, g_response, sizeof g_response);
    if (gateway_headers(&headers, err, errsz) != 0)
        return -1;
    int status = 0;
    http_request req = {
        .method = "GET", .extra_headers = headers.data, .status = &status,
        .on_body = receive_models, .body_ud = &response,
        .on_idle = discovery_idle, .watch_input = ui_is_tty(),
    };
    if (gateway_route(&req, "models?include_metadata=true", &path) != 0)
        return -1;
    http_interrupted = 0;
    int rc = http_post(&req, err, errsz);
    if (rc == HTTP_INTERRUPTED) {
        snprintf(err, errsz, "model discovery interrupted");
        return -1;
    }
    if (rc != HTTP_OK)
        return -1;
    if (status != 200) {
        snprintf(err, errsz, "LiteLLM model discovery: HTTP %d%s", status,
                 status == 401 || status == 403 ? " (check LITELLM_API_KEY)" : "");
        return -1;
    }
    jdoc d;
    if (response.overflow || jdoc_parse(&d, response.data, response.len) != 0) {
        snprintf(err, errsz, "LiteLLM model list is invalid JSON or exceeds catalog capacity");
        return -1;
    }
    int data = j_get(&d, 0, "data");
    if (data < 0 || d.toks[data].type != JSMN_ARRAY) {
        snprintf(err, errsz, "LiteLLM model list has no data array");
        return -1;
    }
    int count = j_count(&d, data), n = 0, bank = 1 - g_bank;
    for (int i = 0; i < count; i++) {
        if (n == LAMBDA_MODELS_MAX) {
            snprintf(err, errsz, "LiteLLM model list exceeds %d entries; increase LAMBDA_MODELS_MAX", LAMBDA_MODELS_MAX);
            return -1;
        }
        gateway_model *m = &g_catalog[bank][n];
        int item = j_elem(&d, data, i);
        buf id, note;
        buf_attach(&id, m->id, sizeof m->id);
        buf_attach(&note, m->note, sizeof m->note);
        if (j_str(&d, item, "id", &id) != 0 || !id.len || id.overflow ||
            strlen(id.data) != id.len) {
            snprintf(err, errsz, "LiteLLM returned an invalid or oversized model id");
            return -1;
        }
        for (size_t j = 0; j < id.len; j++) {
            if ((unsigned char)id.data[j] < 32 || id.data[j] == 127) {
                snprintf(err, errsz, "LiteLLM returned a model id containing control characters");
                return -1;
            }
        }
        j_str(&d, item, "owned_by", &note);
        m->info.context_tokens = j_long(&d, item, "max_input_tokens", 0);
        m->info.output_tokens = j_long(&d, item, "max_output_tokens", 0);
        for (size_t j = 0; j < note.len; j++)
            if ((unsigned char)note.data[j] < 32 || note.data[j] == 127)
                note.data[j] = ' ';
        n++;
    }
    /* libc qsort can allocate; keep sorting inside our fixed arenas. */
    for (int i = 1; i < n; i++) {
        gateway_model tmp = g_catalog[bank][i];
        int j = i;
        while (j > 0 && strcmp(tmp.id, g_catalog[bank][j - 1].id) < 0) {
            g_catalog[bank][j] = g_catalog[bank][j - 1];
            j--;
        }
        g_catalog[bank][j] = tmp;
    }
    int unique = 0;
    for (int i = 0; i < n; i++) {
        if (unique && strcmp(g_catalog[bank][i].id, g_catalog[bank][unique - 1].id) == 0)
            continue;
        if (unique != i)
            g_catalog[bank][unique] = g_catalog[bank][i];
        gateway_model *m = &g_catalog[bank][unique++];
        m->info.id = m->id;
        m->info.note = m->note;
    }
    g_bank = bank;
    g_count = unique;
    return 0;
}

const model_info *gateway_model_info(const char *id)
{
    if (!g_metadata_attempted) {
        char err[512];
        gateway_refresh(err, sizeof err); /* metadata failure is non-fatal */
    }
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_catalog[g_bank][i].id, id) == 0)
            return &g_catalog[g_bank][i].info;
    return NULL;
}
