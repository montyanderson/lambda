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
static int g_reasoning_attempted;
static reasoning_options g_reasoning[LAMBDA_MODELS_MAX];
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
    g_reasoning_attempted = 0;
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
    g_reasoning_attempted = 0;
    memset(g_reasoning, 0, sizeof g_reasoning);
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

/* LiteLLM separates the callable alias catalog from capability metadata.
 * Group levels are useful, but deployment overrides/always-on flags win. */
static int json_bool(const jdoc *d, int obj, const char *key)
{
    int i = j_get(d, obj, key);
    if (i < 0 || d->toks[i].type != JSMN_PRIMITIVE) return 0;
    const char *p = d->js + d->toks[i].start;
    size_t n = (size_t)(d->toks[i].end - d->toks[i].start);
    if (n == 4 && !memcmp(p, "true", 4)) return 1;
    if (n == 5 && !memcmp(p, "false", 5)) return -1;
    return 0;
}

static int array_has(const jdoc *d, int obj, const char *key, const char *value)
{
    int a = j_get(d, obj, key);
    for (int i = 0; i < j_count(d, a); i++)
        if (j_is(d, j_elem(d, a, i), value)) return 1;
    return 0;
}

static void merge_reasoning(const jdoc *d, int obj, reasoning_options *out)
{
    int support = json_bool(d, obj, "supports_reasoning");
    if (support) out->supported = support;
    int effort_param = array_has(d, obj, "supported_openai_params", "reasoning_effort");
    if (!out->supported && effort_param) out->supported = 1;
    /* The common effort interface is low/medium/high. Extra levels require
     * advertised support; an explicit ladder below replaces this baseline. */
    if (out->supported == 1 && !out->levels && effort_param)
        out->levels = (1u << 3) | (1u << 4) | (1u << 5);
    int explicit_levels = 0;
    const char *lists[] = {"supported_reasoning_efforts", "reasoning_effort_levels"};
    for (size_t k = 0; k < sizeof lists / sizeof lists[0]; k++) {
        int a = j_get(d, obj, lists[k]);
        if (a < 0 || d->toks[a].type != JSMN_ARRAY) continue;
        explicit_levels = 1;
        out->levels = 0;
        for (int i = 0; i < j_count(d, a); i++)
            for (int level = 1; level < REASONING_LEVELS; level++)
                if (j_is(d, j_elem(d, a, i), reasoning_name(level))) out->levels |= 1u << level;
        if (out->levels && !support) out->supported = 1;
    }
    for (int i = 1; i < REASONING_LEVELS; i++) {
        char key[64];
        snprintf(key, sizeof key, "supports_%s_reasoning_effort", reasoning_name(i));
        int flag = json_bool(d, obj, key);
        if (flag == 1 && !explicit_levels) out->levels |= 1u << i;
        else if (flag == -1) out->levels &= ~(1u << i);
    }
    if (json_bool(d, obj, "thinking_always_on") == 1) out->always_on = 1;
    const char *native_providers[] = {"anthropic", "bedrock", "vertex_ai", "azure_ai"};
    if (array_has(d, obj, "supported_openai_params", "thinking"))
        for (size_t i = 0; i < sizeof native_providers / sizeof native_providers[0]; i++)
            if (j_is(d, j_get(d, obj, "litellm_provider"), native_providers[i]) ||
                array_has(d, obj, "providers", native_providers[i])) out->native_thinking = 1;
    if (json_bool(d, obj, "supports_adaptive_thinking") == 1 ||
        json_bool(d, obj, "supports_anthropic_thinking_payload") == 1 ||
        j_is(d, j_get(d, obj, "litellm_provider"), "anthropic") ||
        array_has(d, obj, "providers", "anthropic")) out->native_thinking = 1;
    if (out->always_on) out->levels &= ~(1u << 1);
    if (out->supported == -1) out->levels = 0;
    int def = j_get(d, obj, "default_reasoning_effort");
    if (def >= 0) {
        buf value;
        char store[16];
        buf_attach(&value, store, sizeof store);
        out->default_effort[0] = '\0';
        if (j_str_tok(d, def, &value) == 0 && !value.overflow && reasoning_index(store) > 0)
            snprintf(out->default_effort, sizeof out->default_effort, "%s", store);
    }
}

static void receive_metadata(const char *data, size_t len, void *ud)
{
    buf *b = ud;
    if (b->overflow) return;
    if (len > (16u << 20) - b->len) { b->overflow = 1; return; }
    buf_append(b, data, len);
}

static void read_reasoning_metadata(const char *endpoint, int deployments)
{
    char header_store[LAMBDA_REQ_HDRS_MAX], path_store[2048], err[512];
    buf headers, path, response = {.grow = 1};
    buf_attach(&headers, header_store, sizeof header_store);
    buf_attach(&path, path_store, sizeof path_store);
    if (gateway_headers(&headers, err, sizeof err) != 0) return;
    int status = 0;
    http_request req = {.method = "GET", .extra_headers = headers.data, .status = &status,
        .on_body = receive_metadata, .body_ud = &response, .on_idle = discovery_idle,
        .watch_input = ui_is_tty()};
    /* Management endpoints live under the configured prefix, outside /v1. */
    buf_appendf(&path, "%s/%s", g_gateway.prefix, endpoint);
    req.host = g_gateway.host; req.port = g_gateway.port;
    req.plain_http = g_gateway.plain; req.path = path.data;
    if (path.overflow || http_post(&req, err, sizeof err) != HTTP_OK || status != 200 || response.overflow) goto done;
    jdoc d;
    if (jdoc_parse(&d, response.data ? response.data : "", response.len) != 0) goto done;
    int data = j_get(&d, 0, "data");
    if (data < 0 || d.toks[data].type != JSMN_ARRAY) goto done;
    unsigned char seen[LAMBDA_MODELS_MAX] = {0};
    for (int i = 0; i < j_count(&d, data); i++) {
        int entry = j_elem(&d, data, i);
        char name[LAMBDA_MODEL_ID_MAX]; buf b;
        buf_attach(&b, name, sizeof name);
        if (j_str(&d, entry, deployments ? "model_name" : "model_group", &b) != 0 || b.overflow) continue;
        for (int m = 0; m < g_count; m++) {
            if (strcmp(name, g_catalog[g_bank][m].id)) continue;
            reasoning_options candidate = g_reasoning[m];
            int obj = deployments ? j_get(&d, entry, "model_info") : entry;
            merge_reasoning(&d, obj, &candidate);
            if (seen[m]) {
                /* A load-balanced alias must work on every deployment. */
                candidate.levels &= g_reasoning[m].levels;
                if (g_reasoning[m].supported == -1) candidate.supported = -1;
                if (strcmp(candidate.default_effort, g_reasoning[m].default_effort)) candidate.default_effort[0] = '\0';
            }
            g_reasoning[m] = candidate;
            seen[m] = 1;
            break;
        }
    }
done:
    buf_free(&response);
}

void gateway_reasoning(const char *id, reasoning_options *out)
{
    memset(out, 0, sizeof *out);
    gateway_model_info(id);
    if (!g_reasoning_attempted) {
        g_reasoning_attempted = 1;
        read_reasoning_metadata("model_group/info", 0);
        if (!http_interrupted) read_reasoning_metadata("model/info", 1);
    }
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_catalog[g_bank][i].id, id)) { *out = g_reasoning[i]; return; }
}
