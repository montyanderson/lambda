#ifndef LAMBDA_HTTP_H
#define LAMBDA_HTTP_H

#include <signal.h>
#include <stddef.h>

/* set from a SIGINT handler to abort an in-flight request */
extern volatile sig_atomic_t http_interrupted;

typedef struct {
    const char *host;          /* e.g. "api.anthropic.com" */
    const char *port;          /* e.g. "443" */
    const char *path;          /* e.g. "/v1/messages" */
    const char *extra_headers; /* preformatted "K: V\r\n" lines, may be NULL */
    const char *body;
    size_t body_len;

    /* headers are parsed before any on_body call; *status is valid inside it */
    int *status;
    void (*on_body)(const char *data, size_t len, void *ud);
    void *body_ud;

    /* Called while waiting for data, and — when watch_input is set — the
     * moment a keystroke arrives, so the ui stays responsive even while the
     * response is streaming flat out. May be NULL.
     * watch_input must only be set when stdin is a terminal; on a pipe at
     * EOF it would spin. */
    void (*on_idle)(void *ud);
    void *idle_ud;
    int watch_input;
} http_request;

#define HTTP_OK 0
#define HTTP_ERR (-1)         /* err filled */
#define HTTP_INTERRUPTED (-2) /* aborted via http_interrupted */

int http_post(const http_request *req, char *err, size_t errsz);

#endif
