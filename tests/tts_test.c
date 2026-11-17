/* No network or real credentials: exercise request framing and NDJSON. */
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/jsonx.c"
#include "../src/tts.c"

volatile sig_atomic_t http_interrupted;
static const char *response;
static size_t split;
static int response_status = 200, transport_rc, sink_failed;
static char output[128];
static size_t output_len;
static const char *expected_voice = "test-voice";

int http_post(const http_request *req, char *err, size_t cap)
{
    (void)err; (void)cap;
    assert(!strcmp(req->host, "api.inworld.ai"));
    assert(!strcmp(req->port, "443") && !req->plain_http);
    assert(!strcmp(req->path, "/tts/v1/voice:stream"));
    assert(!strcmp(req->extra_headers, "Authorization: Basic fixture-key\r\n"));
    jdoc d;
    assert(jdoc_parse(&d, req->body, req->body_len) == 0);
    assert(j_is(&d, j_get(&d, 0, "voice_id"), expected_voice));
    assert(j_is(&d, j_get(&d, 0, "model_id"), "inworld-tts-2"));
    assert(j_is(&d, j_get(&d, 0, "delivery_mode"), "BALANCED"));
    assert(j_is(&d, j_get(&d, 0, "language"), "AUTO"));
    int config = j_get(&d, 0, "audio_config");
    assert(j_is(&d, j_get(&d, config, "audio_encoding"), "MP3"));
    assert(j_long(&d, config, "speaking_rate", 0) == 1);
    assert(!strstr(req->body, "fixture-key"));
    *req->status = response_status;
    size_t n = strlen(response), first = split < n ? split : n;
    req->on_body(response, first, req->body_ud);
    req->on_body(response + first, n - first, req->body_ud);
    return transport_rc;
}

static int sink(const char *p, size_t n, void *ud)
{
    (void)ud;
    if (sink_failed) return -1;
    assert(output_len + n < sizeof output);
    memcpy(output + output_len, p, n);
    output_len += n;
    return 0;
}

static int run(char *err, size_t cap)
{
    http_interrupted = 0;
    output_len = 0;
    *err = '\0';
    return tts_stream("Hello \"world\". £ 中文 😀", sink, NULL, err, cap);
}

int main(void)
{
    char err[256];
    setenv("INWORLD_API_KEY", "fixture-key", 1);
    setenv("INWORLD_VOICE_ID", expected_voice, 1);
    response = "\r\n{\"result\":{\"audioContent\":\"AAEC\"}}\r\n"
               "{\"result\":{\"audioContent\":\"\\/\\/4=\"}}\n"
               "{\"result\":{\"usage\":{\"processedCharactersCount\":0}}}\n"
               "{\"result\":{\"audioContent\":\"QQ==\"}}";
    for (split = 0; split <= strlen(response); split++) {
        assert(run(err, sizeof err) == 0);
        assert(output_len == 6 && !memcmp(output, "\x00\x01\x02\xff\xfe" "A", 6));
    }
    response = "{\"result\":{\"audioContent\":\"QQ==\"}}\n"
               "{\"error\":{\"message\":\"fixture-key\"}}";
    assert(run(err, sizeof err) < 0 && output_len == 1);
    assert(!strstr(err, "fixture-key"));
    const char *bad[] = {
        "", "\n\r\n", "{", "[]", "{}", "{\"result\":null}",
        "{\"result\":{\"audioContent\":null}}",
        "{\"result\":{\"audioContent\":\"!AAA\"}}",
        "{\"result\":{\"audioContent\":\"Q===\"}}",
        "{\"result\":{\"audioContent\":\"QQ==QQ==\"}}",
        "{\"result\":{\"audioContent\":\"QQ\"}}",
        "{\"result\":{\"audioContent\":\"QR==\"}}",
        "{\"result\":{\"audioContent\":\"\"}}",
        "{\"code\":3,\"message\":\"fixture-key\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        response = bad[i];
        assert(run(err, sizeof err) < 0 && *err);
        assert(!strstr(err, "fixture-key"));
    }
    response_status = 401;
    response = "fixture-key";
    assert(run(err, sizeof err) < 0 && strstr(err, "401"));
    assert(!strstr(err, "fixture-key") && !output_len);
    response_status = 200;
    response = "{\"result\":{\"audioContent\":\"QQ==\"}}\n";
    sink_failed = 1;
    assert(run(err, sizeof err) < 0 && strstr(err, "player"));
    sink_failed = 0;
    transport_rc = HTTP_ERR;
    assert(run(err, sizeof err) < 0 && strstr(err, "connection"));
    transport_rc = HTTP_OK;

    /* A single line can span many HTTP reads; oversize is an explicit error. */
    synthesis s = {.status = 200, .err = err, .cap = sizeof err, .audio = sink};
    buf_attach(&s.line, line_store, sizeof line_store);
    char large[8192]; memset(large, 'x', sizeof large);
    for (size_t i = 0; i < TTS_LINE_MAX / sizeof large; i++)
        receive(large, sizeof large, &s);
    assert(s.failed && strstr(err, "2 MiB"));

    unsetenv("INWORLD_API_KEY");
    assert(tts_configured(err, sizeof err) < 0 && strstr(err, "set INWORLD_API_KEY"));
    setenv("INWORLD_API_KEY", "fixture\r\nBad: header", 1);
    assert(tts_configured(err, sizeof err) < 0 && !strstr(err, "fixture"));
    setenv("INWORLD_API_KEY", "fixture-key", 1);
    setenv("INWORLD_VOICE_ID", "", 1);
    assert(tts_configured(err, sizeof err) < 0);
    setenv("INWORLD_VOICE_ID", "invalid\nvoice", 1);
    assert(tts_configured(err, sizeof err) < 0);

    /* Saved configuration is portable; environment/CLI overrides win. */
    char tmp[] = "/tmp/lambda-tts-XXXXXX", directory[4096], path[4096];
    assert(mkdtemp(tmp));
    setenv("XDG_CONFIG_HOME", tmp, 1);
    unsetenv("INWORLD_VOICE_ID");
    assert(tts_load_voice(err, sizeof err) == 0 && !strcmp(tts_voice_id(), "Sarah"));
    snprintf(directory, sizeof directory, "%s/lambda", tmp);
    snprintf(path, sizeof path, "%s/lambda/voice.json", tmp);
    assert(mkdir(directory, 0700) == 0);
    FILE *f = fopen(path, "w"); assert(f);
    fputs("{\"voice_id\":\"saved-test-voice\"}", f); fclose(f);
    assert(tts_load_voice(err, sizeof err) == 0);
    expected_voice = "saved-test-voice";
    assert(run(err, sizeof err) == 0);
    setenv("INWORLD_VOICE_ID", "override", 1);
    assert(tts_load_voice(err, sizeof err) == 0);
    expected_voice = "override";
    assert(run(err, sizeof err) == 0);
    unsetenv("INWORLD_VOICE_ID");
    f = fopen(path, "w"); assert(f);
    fputs("{\"voice_id\":\"bad\\u0000id\"}", f); fclose(f);
    assert(tts_load_voice(err, sizeof err) < 0);
    unlink(path); rmdir(directory); rmdir(tmp);
    puts("tts: request settings, every stream split, binary audio, EOF, errors and credential isolation: ok");
    return 0;
}
