/* Minimal HTTPS/1.1 client: getaddrinfo + BearSSL + picohttpparser for
 * response headers and chunked framing. One connection per request
 * ("Connection: close"). No dynamic allocation. */

#include "http.h"
#include "config.h"
#include "ta.h"
#include "util.h"

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <bearssl.h>
#include "picohttpparser.h"

volatile sig_atomic_t http_interrupted = 0;

typedef struct {
    int fd;
    void (*on_idle)(void *);
    void *idle_ud;
    int watch_input;
    int interrupted;
    int io_errno;
} ioctx;

static int sock_read(void *ctx, unsigned char *b, size_t len)
{
    ioctx *io = ctx;
    for (;;) {
        if (http_interrupted) {
            io->interrupted = 1;
            return -1;
        }
        fd_set rf;
        struct timeval tv;
        int maxfd = io->fd;
        FD_ZERO(&rf);
        FD_SET(io->fd, &rf);
        /* Watch the keyboard alongside the socket. Without this, keystrokes
         * are only serviced when the stream goes quiet — which during a busy
         * response is never, so typing appears frozen. */
        if (io->watch_input) {
            FD_SET(STDIN_FILENO, &rf);
            if (STDIN_FILENO > maxfd)
                maxfd = STDIN_FILENO;
        }
        tv.tv_sec = 0;
        tv.tv_usec = 120 * 1000;
        int r = select(maxfd + 1, &rf, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            io->io_errno = errno;
            return -1;
        }
        if (r == 0) {
            if (io->on_idle)
                io->on_idle(io->idle_ud);
            continue;
        }
        if (io->watch_input && FD_ISSET(STDIN_FILENO, &rf)) {
            if (io->on_idle)
                io->on_idle(io->idle_ud); /* drains and echoes the keystroke */
            if (!FD_ISSET(io->fd, &rf))
                continue;
        }
        ssize_t n = read(io->fd, b, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            io->io_errno = errno;
            return -1;
        }
        if (n == 0)
            return -1; /* EOF */
        return (int)n;
    }
}

static int sock_write(void *ctx, const unsigned char *b, size_t len)
{
    ioctx *io = ctx;
    for (;;) {
        if (http_interrupted) {
            io->interrupted = 1;
            return -1;
        }
        ssize_t n = write(io->fd, b, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            io->io_errno = errno;
            return -1;
        }
        return (int)n;
    }
}

static int tcp_connect(const char *host, const char *port, char *err,
                       size_t errsz)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        snprintf(err, errsz, "cannot resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        snprintf(err, errsz, "cannot connect to %s:%s: %s", host, port,
                 strerror(errno));
    return fd;
}

/* ---- response parsing ------------------------------------------------- */

enum framing { F_CHUNKED, F_LENGTH, F_CLOSE };

typedef struct {
    const http_request *req;

    buf hdr;         /* accumulated bytes until headers parsed */
    size_t prev_len; /* picohttpparser incremental-parse cursor */
    int headers_done;
    enum framing framing;
    uint64_t remaining; /* content-length countdown */
    struct phr_chunked_decoder cdec;
    int body_done;
} resp;

static void emit(resp *r, const char *data, size_t len)
{
    if (len > 0 && r->req->on_body)
        r->req->on_body(data, len, r->req->body_ud);
}

/* body bytes after headers; data must be mutable (chunked decode-in-place) */
static int feed_body(resp *r, char *data, size_t len, char *err, size_t errsz)
{
    switch (r->framing) {
    case F_CHUNKED: {
        size_t sz = len;
        ssize_t pr = phr_decode_chunked(&r->cdec, data, &sz);
        if (pr == -1) {
            snprintf(err, errsz, "bad chunked encoding");
            return -1;
        }
        emit(r, data, sz);
        if (pr >= 0)
            r->body_done = 1;
        return 0;
    }
    case F_LENGTH: {
        size_t take = len;
        if ((uint64_t)take > r->remaining)
            take = (size_t)r->remaining;
        emit(r, data, take);
        r->remaining -= take;
        if (r->remaining == 0)
            r->body_done = 1;
        return 0;
    }
    default:
        emit(r, data, len);
        return 0;
    }
}

static int hdr_is(const struct phr_header *h, const char *name)
{
    return h->name_len == strlen(name) &&
           strncasecmp(h->name, name, h->name_len) == 0;
}

static int mem_contains(const char *hay, size_t haylen, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen > haylen)
        return 0;
    for (size_t i = 0; i + nlen <= haylen; i++)
        if (strncasecmp(hay + i, needle, nlen) == 0)
            return 1;
    return 0;
}

static int feed(resp *r, char *data, size_t len, char *err, size_t errsz)
{
    if (r->headers_done)
        return feed_body(r, data, len, err, errsz);

    if (buf_append(&r->hdr, data, len) != 0) {
        snprintf(err, errsz, "HTTP response headers too large");
        return -1;
    }
    int minor, status;
    const char *msg;
    size_t msg_len;
    struct phr_header headers[LAMBDA_RESP_HDR_COUNT];
    size_t nh = LAMBDA_RESP_HDR_COUNT;
    int pret = phr_parse_response(r->hdr.data, r->hdr.len, &minor, &status,
                                  &msg, &msg_len, headers, &nh, r->prev_len);
    r->prev_len = r->hdr.len;
    if (pret == -1) {
        snprintf(err, errsz, "malformed HTTP response");
        return -1;
    }
    if (pret == -2)
        return 0; /* need more bytes */

    *r->req->status = status;
    r->headers_done = 1;
    r->framing = F_CLOSE;
    for (size_t i = 0; i < nh; i++) {
        if (hdr_is(&headers[i], "transfer-encoding") &&
            mem_contains(headers[i].value, headers[i].value_len, "chunked")) {
            r->framing = F_CHUNKED;
            memset(&r->cdec, 0, sizeof r->cdec);
            r->cdec.consume_trailer = 1;
        } else if (hdr_is(&headers[i], "content-length") &&
                   r->framing != F_CHUNKED) {
            r->framing = F_LENGTH;
            r->remaining = strtoull(headers[i].value, NULL, 10);
            if (r->remaining == 0)
                r->body_done = 1;
        }
    }
    if (r->hdr.len > (size_t)pret)
        return feed_body(r, r->hdr.data + pret, r->hdr.len - (size_t)pret,
                         err, errsz);
    return 0;
}

/* ---- request ----------------------------------------------------------- */

int http_post(const http_request *req, char *err, size_t errsz)
{
    if (ta_load(err, errsz) != 0)
        return HTTP_ERR;

    int fd = tcp_connect(req->host, req->port, err, errsz);
    if (fd < 0)
        return http_interrupted ? HTTP_INTERRUPTED : HTTP_ERR;

    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];

    br_ssl_client_init_full(&sc, &xc, ta_anchors(), ta_count());
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);

    /* x509 "minimal" has no clock of its own */
    time_t now = time(NULL);
    br_x509_minimal_set_time(&xc, (uint32_t)(now / 86400 + 719528),
                             (uint32_t)(now % 86400));

    br_ssl_client_reset(&sc, req->host, 0);

    ioctx io = {fd, req->on_idle, req->idle_ud, req->watch_input, 0, 0};
    br_sslio_init(&ioc, &sc.eng, sock_read, &io, sock_write, &io);

    static char head_store[LAMBDA_REQ_HDRS_MAX + 1024];
    buf head;
    buf_attach(&head, head_store, sizeof head_store);
    buf_appendf(&head,
                "POST %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "Connection: close\r\n"
                "Accept: application/json, text/event-stream\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n",
                req->path, req->host, req->body_len);
    if (req->extra_headers)
        buf_appends(&head, req->extra_headers);
    buf_appends(&head, "\r\n");
    if (head.overflow) {
        snprintf(err, errsz, "request headers too large");
        close(fd);
        return HTTP_ERR;
    }

    int ret = HTTP_ERR;
    resp r;
    static char hdr_store[LAMBDA_RESP_HDR_MAX];
    memset(&r, 0, sizeof r);
    r.req = req;
    buf_attach(&r.hdr, hdr_store, sizeof hdr_store);

    if (br_sslio_write_all(&ioc, head.data, head.len) != 0 ||
        br_sslio_write_all(&ioc, req->body, req->body_len) != 0 ||
        br_sslio_flush(&ioc) != 0) {
        goto tls_fail;
    }

    for (;;) {
        unsigned char rb[8192];
        int n = br_sslio_read(&ioc, rb, sizeof rb);
        if (n < 0) {
            int e = br_ssl_engine_last_error(&sc.eng);
            if (io.interrupted) {
                ret = HTTP_INTERRUPTED;
                goto done;
            }
            if (e == BR_ERR_OK || (e == BR_ERR_IO && r.headers_done)) {
                /* clean close (or abrupt close mid-body) */
                if (r.headers_done &&
                    (r.body_done || r.framing == F_CLOSE)) {
                    ret = HTTP_OK;
                } else {
                    snprintf(err, errsz, "connection closed prematurely");
                }
                goto done;
            }
            goto tls_fail;
        }
        if (feed(&r, (char *)rb, (size_t)n, err, errsz) != 0)
            goto done;
        if (r.body_done) {
            ret = HTTP_OK;
            goto done;
        }
    }

tls_fail: {
    int e = br_ssl_engine_last_error(&sc.eng);
    if (io.interrupted)
        ret = HTTP_INTERRUPTED;
    else if (e == BR_ERR_X509_NOT_TRUSTED)
        snprintf(err, errsz, "TLS: certificate not trusted");
    else if (e == BR_ERR_IO || e == BR_ERR_OK)
        snprintf(err, errsz, "network error: %s",
                 io.io_errno ? strerror(io.io_errno) : "connection closed");
    else
        snprintf(err, errsz, "TLS error %d", e);
}
done:
    close(fd);
    return ret;
}
