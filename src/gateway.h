#ifndef LAMBDA_GATEWAY_H
#define LAMBDA_GATEWAY_H

#include "api.h"
#include "http.h"
#include "util.h"
#include "reasoning.h"

/* A LiteLLM base URL, optionally ending in /v1. No credentials in URLs. */
int gateway_configure(const char *url, char *err, size_t errsz);
/* Optional $XDG_CONFIG_HOME/lambda/config.json (or ~/.config/lambda). */
int gateway_load_config(char *err, size_t errsz);
int gateway_enabled(void);
const char *gateway_url(void);
int gateway_headers(buf *headers, char *err, size_t errsz);
/* Configure host, port, TLS and path; the path buffer must outlive req. */
int gateway_route(http_request *req, const char *endpoint, buf *path);
int gateway_refresh(char *err, size_t errsz);
int gateway_model_count(void);
const model_info *gateway_model_at(int i);
/* Lazily fetch metadata once when an explicit -m skipped model discovery. */
const model_info *gateway_model_info(const char *id);
/* Optional rich metadata; unknown capabilities leave the API default usable. */
void gateway_reasoning(const char *id, reasoning_options *out);

#endif
