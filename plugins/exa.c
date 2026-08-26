/* Exa web search — https://exa.ai
 *
 * POST https://api.exa.ai/search with an Authorization: Bearer key.
 * Enabled when $EXA_API_KEY is set; otherwise the tool is not offered to the
 * model at all.
 *
 * Highlights (short relevant excerpts) are requested rather than full page
 * text: they are far cheaper in tokens and usually enough to decide whether
 * to fetch more. */

#include "plugin.h"
#include "config.h"
#include "jsonx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXA_HOST "api.exa.ai"
#define EXA_PATH "/search"
#define MAX_RESULTS 25
/* Total excerpt budget, shared across results, so a 25-result search stays
 * well inside the tool-output cap instead of being truncated at the end. */
#define EXCERPT_BUDGET 36000
#define EXCERPT_MIN 400
#define EXCERPT_MAX 1600

static int exa_available(char *why, size_t whysz)
{
    const char *k = getenv("EXA_API_KEY");
    if (k && *k)
        return 1;
    snprintf(why, whysz, "EXA_API_KEY is not set");
    return 0;
}

static void exa_label(const char *args_json, char *out, size_t cap)
{
    static char store[2048];
    buf sv;
    jdoc d;
    out[0] = '\0';
    buf_attach(&sv, store, sizeof store);
    if (jdoc_parse(&d, args_json, strlen(args_json)) != 0)
        return;
    if (j_str(&d, 0, "query", &sv) == 0 && sv.len)
        snprintf(out, cap, "exa_search \"%s\"", sv.data);
}

/* copy a decoded string into out, collapsing runs of whitespace so a page of
 * scraped html doesn't turn into a screenful of blank lines */
static void append_squeezed(buf *out, const char *s, size_t limit)
{
    size_t n = 0;
    int sp = 0;
    for (const char *p = s; *p && n < limit; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            sp = 1;
            continue;
        }
        if (sp && n) {
            buf_append(out, " ", 1);
            n++;
        }
        sp = 0;
        buf_append(out, p, 1);
        n++;
    }
    if (n >= limit)
        buf_appends(out, "…");
}

/* turn the response JSON into something compact for the model to read */
static int format_results(const char *js, size_t len, buf *out)
{
    static char sv_store[LAMBDA_PLUGIN_OUT_MAX];
    buf sv;
    jdoc d;
    buf_attach(&sv, sv_store, sizeof sv_store);

    if (jdoc_parse(&d, js, len) != 0) {
        buf_appends(out, "exa: could not parse the response");
        return -1;
    }
    /* an error body carries no results array */
    int results = j_get(&d, 0, "results");
    if (results < 0) {
        if (j_str(&d, 0, "error", &sv) == 0 && sv.len)
            buf_appendf(out, "exa error: %s", sv.data);
        else if (j_str(&d, 0, "message", &sv) == 0 && sv.len)
            buf_appendf(out, "exa error: %s", sv.data);
        else
            buf_appends(out, "exa: no results in response");
        return -1;
    }

    if (j_str(&d, 0, "autopromptString", &sv) == 0 && sv.len)
        buf_appendf(out, "(interpreted as: %s)\n\n", sv.data);

    int n = j_count(&d, results);
    if (n == 0) {
        buf_appends(out, "no results");
        return 0;
    }
    size_t per = (size_t)(EXCERPT_BUDGET / n);
    if (per < EXCERPT_MIN)
        per = EXCERPT_MIN;
    if (per > EXCERPT_MAX)
        per = EXCERPT_MAX;
    for (int i = 0; i < n; i++) {
        int r = j_elem(&d, results, i);
        buf_appendf(out, "%d. ", i + 1);
        if (j_str(&d, r, "title", &sv) == 0 && sv.len)
            buf_appends(out, sv.data);
        else
            buf_appends(out, "(untitled)");
        if (j_str(&d, r, "url", &sv) == 0 && sv.len)
            buf_appendf(out, "\n   %s", sv.data);

        int meta = 0;
        if (j_str(&d, r, "publishedDate", &sv) == 0 && sv.len) {
            buf_appendf(out, "\n   %s", sv.data);
            meta = 1;
        }
        if (j_str(&d, r, "author", &sv) == 0 && sv.len)
            buf_appendf(out, "%s%s", meta ? " · " : "\n   ", sv.data);

        /* highlights is an array of excerpts; text is the whole page */
        int hl = j_get(&d, r, "highlights");
        int wrote = 0;
        if (hl >= 0) {
            int hn = j_count(&d, hl);
            size_t left = per;
            for (int k = 0; k < hn && k < 3 && left > 0; k++) {
                if (j_str_tok(&d, j_elem(&d, hl, k), &sv) != 0 || !sv.len)
                    continue;
                buf_appends(out, "\n   ");
                size_t take = left < sv.len ? left : sv.len;
                append_squeezed(out, sv.data, take);
                left -= take;
                wrote = 1;
            }
        }
        if (!wrote && j_str(&d, r, "text", &sv) == 0 && sv.len) {
            buf_appends(out, "\n   ");
            append_squeezed(out, sv.data, per);
        }
        buf_appends(out, "\n\n");
    }
    return 0;
}

static int exa_run(const char *args_json, buf *out)
{
    static char query[4096], body_store[8192], hdr_store[512];
    static char resp_store[LAMBDA_PLUGIN_OUT_MAX];
    buf sv, body, hdrs, resp;
    jdoc d;

    const char *key = getenv("EXA_API_KEY");
    if (!key || !*key) {
        buf_appends(out, "EXA_API_KEY is not set");
        return -1;
    }

    buf_attach(&sv, query, sizeof query);
    if (jdoc_parse(&d, args_json, strlen(args_json)) != 0 ||
        j_str(&d, 0, "query", &sv) != 0 || sv.len == 0) {
        buf_appends(out, "exa_search needs a \"query\" string");
        return -1;
    }
    long num = j_long(&d, 0, "num_results", 5);
    if (num < 1)
        num = 1;
    if (num > MAX_RESULTS)
        num = MAX_RESULTS;

    /* optional passthroughs */
    static char type_store[32], cat_store[64];
    buf type, cat;
    buf_attach(&type, type_store, sizeof type_store);
    buf_attach(&cat, cat_store, sizeof cat_store);
    int have_type = j_str(&d, 0, "type", &type) == 0 && type.len;
    int have_cat = j_str(&d, 0, "category", &cat) == 0 && cat.len;
    int want_text = 0;
    int wt = j_get(&d, 0, "full_text");
    if (wt >= 0 && j_is(&d, wt, "true"))
        want_text = 1;
    /* jsmn reports booleans as primitives, so check the raw token too */
    if (wt >= 0) {
        const char *p;
        size_t plen;
        if (j_raw(&d, wt, &p, &plen) == 0 && plen == 4 &&
            memcmp(p, "true", 4) == 0)
            want_text = 1;
    }

    buf_attach(&body, body_store, sizeof body_store);
    buf_appends(&body, "{\"query\":");
    buf_append_json_str(&body, sv.data);
    buf_appendf(&body, ",\"numResults\":%ld", num);
    buf_appends(&body, ",\"type\":");
    buf_append_json_str(&body, have_type ? type.data : "auto");
    if (have_cat) {
        buf_appends(&body, ",\"category\":");
        buf_append_json_str(&body, cat.data);
    }
    buf_appendf(&body, ",\"contents\":{\"highlights\":true%s}",
                want_text ? ",\"text\":true" : "");
    buf_appends(&body, "}");
    if (body.overflow) {
        buf_appends(out, "exa: query too long");
        return -1;
    }

    buf_attach(&hdrs, hdr_store, sizeof hdr_store);
    buf_appendf(&hdrs, "Authorization: Bearer %s\r\n", key);
    if (hdrs.overflow) {
        buf_appends(out, "exa: api key too long");
        return -1;
    }

    buf_attach(&resp, resp_store, sizeof resp_store);
    int status = 0;
    char err[256] = "";
    int rc = plugin_https_post(EXA_HOST, EXA_PATH, hdrs.data, body.data, &resp,
                               &status, err, sizeof err);
    if (rc != 0) {
        buf_appendf(out, "exa request failed: %s",
                    err[0] ? err : "interrupted");
        return -1;
    }
    if (status != 200) {
        buf_appendf(out, "exa http %d: ", status);
        /* surface the api's own message when there is one */
        static char msg_store[1024];
        buf msg;
        buf_attach(&msg, msg_store, sizeof msg_store);
        if (jdoc_parse(&d, resp.data, resp.len) == 0 &&
            (j_str(&d, 0, "error", &msg) == 0 ||
             j_str(&d, 0, "message", &msg) == 0) &&
            msg.len)
            buf_appends(out, msg.data);
        else
            buf_append(out, resp.data, resp.len < 300 ? resp.len : 300);
        return -1;
    }
    return format_results(resp.data, resp.len, out);
}

static const lambda_tool exa_search = {
    .name = "exa_search",
    .description =
        "Search the web with Exa, a search engine built for AI agents. "
        "Returns titles, URLs and short relevant excerpts. Use it for current "
        "information, documentation, or anything outside your training data. "
        "Prefer a descriptive natural-language query over keywords.",
    .schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"query\":{\"type\":\"string\",\"description\":\"What to search "
        "for, phrased as a description of the desired page\"},"
        "\"num_results\":{\"type\":\"integer\",\"description\":\"How many "
        "results to return (1-25, default 5)\"},"
        "\"type\":{\"type\":\"string\",\"enum\":[\"auto\",\"instant\","
        "\"fast\",\"deep-lite\",\"deep\",\"deep-reasoning\"],"
        "\"description\":\"Search mode; 'auto' is usually right\"},"
        "\"category\":{\"type\":\"string\",\"enum\":[\"company\",\"people\","
        "\"publication\",\"news\",\"personal site\",\"financial report\"],"
        "\"description\":\"Restrict results to a category\"},"
        "\"full_text\":{\"type\":\"boolean\",\"description\":\"Return whole "
        "page text instead of excerpts. Expensive in tokens; default false\"}"
        "},\"required\":[\"query\"]}",
    .run = exa_run,
    .available = exa_available,
    .label = exa_label,
};

LAMBDA_TOOL_REGISTER(exa_search)
