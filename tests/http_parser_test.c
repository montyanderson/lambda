#include <assert.h>
#include "../src/http.c"

static char output[64];
static buf captured;
static void capture(const char *data, size_t len, void *ud)
{ (void)ud; buf_append(&captured, data, len); }
static void cancel(void *ud) { (void)ud; http_interrupted = 1; }

static void check(const char *wire, const char *body, int expected_status)
{
    for (size_t split = 0; split <= strlen(wire); split++) {
        resp r;
        memset(&r, 0, sizeof r);
        char header[2048], a[2048], b[2048], err[128] = "";
        int status = 0;
        http_request req = {.status = &status, .on_body = capture};
        r.req = &req;
        buf_attach(&r.hdr, header, sizeof header);
        buf_attach(&captured, output, sizeof output);
        memcpy(a, wire, split);
        memcpy(b, wire + split, strlen(wire) - split);
        assert(feed(&r, a, split, err, sizeof err) == 0);
        assert(feed(&r, b, strlen(wire) - split, err, sizeof err) == 0);
        assert(status == expected_status && !strcmp(output, body));
        assert(r.body_done || r.framing == F_CLOSE);
    }
}

int main(void)
{
    check("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", "hello", 200);
    check("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n", "hello", 200);
    check("HTTP/1.1 429 Busy\r\nConnection: close\r\n\r\nerror", "error", 429);
    check("HTTP/1.1 204 Empty\r\nContent-Length: 0\r\n\r\n", "", 204);
    /* Both descriptors are ready: cancellation wins over the final byte. */
    int sockets[2], input[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(pipe(input) == 0);
    int original = dup(STDIN_FILENO);
    assert(original >= 0 && dup2(input[0], STDIN_FILENO) >= 0);
    assert(write(input[1], "c", 1) == 1 && write(sockets[1], "x", 1) == 1);
    ioctx io = {.fd=sockets[0], .on_idle=cancel, .watch_input=1};
    unsigned char byte;
    assert(sock_read(&io, &byte, 1) == -1 && io.interrupted);
    assert(read(sockets[0], &byte, 1) == 1 && byte == 'x');
    dup2(original, STDIN_FILENO);
    close(original); close(input[0]); close(input[1]); close(sockets[0]); close(sockets[1]);
    puts("HTTP: every framing split, chunked/length/close, simultaneous input/socket cancellation: ok");
    return 0;
}
