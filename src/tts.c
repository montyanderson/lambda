/* Inworld HTTP streaming: NDJSON -> independently decoded MP3 chunks. */
#include "tts.h"
#include "http.h"
#include "jsonx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define TTS_LINE_MAX (2u << 20)
static char line_store[TTS_LINE_MAX], encoded[TTS_LINE_MAX];
static char decoded[TTS_LINE_MAX * 3 / 4];
static char saved_voice[256];

const char *tts_voice_id(void)
{
    const char *id = getenv("INWORLD_VOICE_ID");
    return id ? id : *saved_voice ? saved_voice : "Sarah";
}

int tts_load_voice(char *err, size_t cap)
{
    if (getenv("INWORLD_VOICE_ID")) return 0;
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char path[4096], data[4096];
    int n;
    if (xdg && *xdg)
        n = snprintf(path, sizeof path, "%s/lambda/voice.json", xdg);
    else if (home && *home)
        n = snprintf(path, sizeof path, "%s/.config/lambda/voice.json", home);
    else return 0;
    if (n < 0 || (size_t)n >= sizeof path) goto bad;
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) { saved_voice[0] = '\0'; return 0; }
        goto bad;
    }
    size_t len = fread(data, 1, sizeof data, f);
    int failed = ferror(f);
    fclose(f);
    jdoc d;
    buf id;
    buf_attach(&id, saved_voice, sizeof saved_voice);
    if (failed || len == sizeof data || jdoc_parse(&d, data, len) != 0 ||
        j_str(&d, 0, "voice_id", &id) != 0 || id.overflow || !id.len || strlen(id.data) != id.len)
        goto bad;
    return 0;
bad:
    snprintf(err, cap, "voice: cannot read voice.json (expected a voice_id string)");
    return -1;
}

typedef struct {
    buf line;
    int status, failed, received;
    int (*audio)(const char *, size_t, void *);
    void *ud;
    char *err;
    size_t cap;
} synthesis;

int tts_configured(char *err, size_t cap)
{
    const char *id = tts_voice_id();
    if (!*id || strlen(id) > 255) {
        snprintf(err, cap, "voice: INWORLD_VOICE_ID must be 1–255 bytes");
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        if (*p < 32 || *p == 127) {
            snprintf(err, cap, "voice: voice ID contains control characters");
            return -1;
        }
    }
    const char *key = getenv("INWORLD_API_KEY");
    if (!key || !*key) {
        snprintf(err, cap, "voice: set INWORLD_API_KEY in lambda's environment");
        return -1;
    }
    if (strlen(key) > 7000) {
        snprintf(err, cap, "voice: INWORLD_API_KEY is too long");
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        if (*p < 33 || *p > 126) {
            snprintf(err, cap, "voice: INWORLD_API_KEY contains invalid characters");
            return -1;
        }
    }
    return 0;
}

static void fail(synthesis *s, const char *message)
{
    if (!s->failed)
        snprintf(s->err, s->cap, "voice: %s", message);
    s->failed = 1;
    http_interrupted = 1; /* worker-local; never interrupts the model */
}

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int decode_audio(const char *p, size_t n, size_t *len)
{
    *len = 0;
    if (n % 4) return -1;
    for (size_t i = 0; i < n; i += 4) {
        int a = b64_value(p[i]), b = b64_value(p[i + 1]);
        int c = p[i + 2] == '=' ? 0 : b64_value(p[i + 2]);
        int d = p[i + 3] == '=' ? 0 : b64_value(p[i + 3]);
        int pad = p[i + 2] == '=' ? 2 : p[i + 3] == '=' ? 1 : 0;
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (pad && i + 4 != n) || (pad == 2 && p[i + 3] != '=') ||
            (pad == 2 && (b & 15)) || (pad == 1 && (c & 3)))
            return -1;
        decoded[(*len)++] = (char)((a << 2) | (b >> 4));
        if (pad < 2) decoded[(*len)++] = (char)((b << 4) | (c >> 2));
        if (!pad) decoded[(*len)++] = (char)((c << 6) | d);
    }
    return 0;
}

static void consume_line(synthesis *s)
{
    const char *p = s->line.data;
    size_t n = s->line.len;
    while (n && (*p == ' ' || *p == '\r' || *p == '\t')) { p++; n--; }
    if (!n) return;
    jdoc d;
    if (jdoc_parse(&d, p, n) != 0 || d.toks[0].type != JSMN_OBJECT) {
        fail(s, "invalid Inworld stream JSON");
        return;
    }
    /* Never print upstream bodies: they can echo credentials or input. */
    if (j_get(&d, 0, "error") >= 0 || j_get(&d, 0, "code") >= 0) {
        fail(s, "Inworld synthesis failed");
        return;
    }
    int result = j_get(&d, 0, "result");
    if (result < 0 || d.toks[result].type != JSMN_OBJECT) {
        fail(s, "missing result in Inworld stream");
        return;
    }
    int token = j_get(&d, result, "audioContent");
    if (token < 0) return; /* usage/timestamps can follow the last audio */
    buf audio;
    buf_attach(&audio, encoded, sizeof encoded);
    size_t len;
    if (j_str_tok(&d, token, &audio) != 0 || audio.overflow ||
        decode_audio(audio.data, audio.len, &len) != 0) {
        fail(s, "invalid base64 audio in Inworld stream");
        return;
    }
    if (!len) return;
    if (s->audio(decoded, len, s->ud) != 0) {
        fail(s, "audio player stopped; check your audio output device");
        return;
    }
    s->received = 1;
}

static void receive(const char *p, size_t n, void *ud)
{
    synthesis *s = ud;
    if (s->status != 200) {
        s->failed = 1;
        snprintf(s->err, s->cap, "voice: Inworld HTTP %d%s", s->status,
                 s->status == 401 || s->status == 403 ? " (check INWORLD_API_KEY)" : "");
        http_interrupted = 1;
        return;
    }
    while (n && !s->failed) {
        const char *nl = memchr(p, '\n', n);
        size_t take = nl ? (size_t)(nl - p) : n;
        if (buf_append(&s->line, p, take) != 0) {
            fail(s, "Inworld stream line exceeds 2 MiB");
            return;
        }
        p += take; n -= take;
        if (nl) {
            consume_line(s);
            buf_reset(&s->line);
            p++; n--;
        }
    }
}

int tts_stream(const char *text, int (*audio)(const char *, size_t, void *),
               void *ud, char *err, size_t cap)
{
    if (tts_configured(err, cap) != 0) return -1;
    if (!*text || strlen(text) > TTS_TEXT_MAX) {
        snprintf(err, cap, "voice: invalid speech text length");
        return -1;
    }
    char headers[8192], body_store[TTS_TEXT_MAX * 6 + 2048], transport[256];
    snprintf(headers, sizeof headers, "Authorization: Basic %s\r\n", getenv("INWORLD_API_KEY"));
    buf body;
    buf_attach(&body, body_store, sizeof body_store);
    buf_appends(&body, "{\"text\":");
    buf_append_json_str(&body, text);
    buf_appends(&body, ",\"voice_id\":");
    buf_append_json_str(&body, tts_voice_id());
    buf_appends(&body, ",\"model_id\":\"inworld-tts-2\","
                "\"audio_config\":{\"audio_encoding\":\"MP3\",\"speaking_rate\":1},"
                "\"delivery_mode\":\"BALANCED\",\"language\":\"AUTO\"}");
    synthesis s = {.audio = audio, .ud = ud, .err = err, .cap = cap};
    buf_attach(&s.line, line_store, sizeof line_store);
    http_request req = {
        .host = "api.inworld.ai", .port = "443", .path = "/tts/v1/voice:stream",
        .extra_headers = headers, .body = body.data, .body_len = body.len,
        .status = &s.status, .on_body = receive, .body_ud = &s,
    };
    int rc = http_post(&req, transport, sizeof transport);
    if (s.failed) return -1;
    if (rc != HTTP_OK) {
        snprintf(err, cap, "voice: Inworld connection failed or timed out");
        return -1;
    }
    if (s.status != 200) {
        snprintf(err, cap, "voice: Inworld HTTP %d", s.status);
        return -1;
    }
    if (s.line.len) consume_line(&s);
    if (!s.failed && !s.received) fail(&s, "Inworld returned no audio");
    return s.failed ? -1 : 0;
}
