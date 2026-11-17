/* Exercise the real worker/pipe/player lifecycle with a fake synthesizer
 * and a local executable, independent of audio hardware and credentials. */
#include <assert.h>
#include "../src/voice.c"

volatile sig_atomic_t http_interrupted;
static char audio_path[4096], requests_path[4096];
static int synth_mode;

int tts_load_voice(char *err, size_t cap) { (void)err; (void)cap; return 0; }
int tts_configured(char *err, size_t cap) { (void)err; (void)cap; return 0; }

int tts_stream(const char *text, int (*audio)(const char *, size_t, void *),
               void *ud, char *err, size_t cap)
{
    size_t len = strlen(text);
    assert(len && len <= TTS_TEXT_MAX);
    for (size_t i = 0; i < len;) {
        size_t n = utf8_span(text + i, len - i);
        assert(n); i += n;
    }
    int fd = open(requests_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    assert(fd >= 0 && write(fd, text, len) == (ssize_t)len);
    close(fd);
    if (synth_mode == 1) {
        snprintf(err, cap, "voice: simulated synthesis failure");
        return -1;
    }
    while (synth_mode == 2 && !http_interrupted) {
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    if (http_interrupted) return -1;
    return audio(text, len, ud);
}

static long millis(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static void tick(void)
{
    voice_poll();
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
}

static void finished(void)
{
    long until = millis() + 5000;
    while (voice_poll() && millis() < until) tick();
    assert(!worker_pid);
}

static size_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (size_t)st.st_size : 0;
}

static void wait_bytes(const char *path, size_t n)
{
    long until = millis() + 3000;
    while (file_size(path) < n && millis() < until) tick();
    assert(file_size(path) == n);
}

static void reset_files(void)
{
    unlink(audio_path); unlink(requests_path);
    http_interrupted = 0;
    assert(!voice_error());
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char tmp[] = "/tmp/lambda-voice-XXXXXX", executable[4096], err[256];
    assert(mkdtemp(tmp));
    snprintf(audio_path, sizeof audio_path, "%s/audio", tmp);
    snprintf(requests_path, sizeof requests_path, "%s/requests", tmp);
    snprintf(executable, sizeof executable, "%s/mpv", tmp);
    FILE *f = fopen(executable, "w");
    assert(f);
    fputs("#!/bin/sh\n[ -z \"${INWORLD_API_KEY+x}\" ] || exit 9\n"
          "exec /bin/cat >> \"$LAMBDA_TEST_AUDIO\"\n", f);
    fclose(f);
    assert(chmod(executable, 0700) == 0);
    setenv("LAMBDA_TEST_AUDIO", audio_path, 1);
    setenv("INWORLD_API_KEY", "fixture-key-not-for-player", 1);
    setenv("PATH", "/no-such-player-directory", 1);
    assert(voice_enable(1, err, sizeof err) < 0 && strstr(err, "install"));
    setenv("PATH", tmp, 1);
    assert(voice_enable(1, err, sizeof err) == 0);
    assert(voice_enabled());

    /* No sentence boundary and no EOF: speech still begins while generating. */
    voice_begin();
    voice_text("stream-me");
    wait_bytes(audio_path, 9);
    assert(worker_pid && !ending);
    voice_text(" now.");
    voice_end();
    finished();
    assert(file_size(audio_path) == 13);
    assert(!voice_error());
    reset_files();

    /* A continuous stream without punctuation must not defer until EOF. */
    voice_begin();
    for (int i = 0; i < 35; i++) {
        voice_text("word ");
        struct timespec pause = {0, 20000000};
        nanosleep(&pause, NULL);
    }
    assert(file_size(audio_path) > 0 && worker_pid && !ending);
    voice_end(); finished();
    reset_files();

    /* Large UTF-8 reply, no spaces: request caps cannot split an emoji. */
    static char long_text[16004], got[16004];
    memcpy(long_text, "abc", 3); /* pipe reads split through UTF-8 scalars */
    for (size_t i = 3; i < 16003; i += 4) memcpy(long_text + i, "😀", 4);
    voice_begin();
    voice_text(long_text);
    voice_end();
    finished();
    assert(file_size(audio_path) == 16003);
    f = fopen(audio_path, "r"); assert(f);
    assert(fread(got, 1, sizeof got, f) == 16003);
    fclose(f);
    assert(!memcmp(got, long_text, 16003));
    reset_files();

    synth_mode = 1;
    voice_begin(); voice_text("Fail."); voice_end(); finished();
    const char *message = voice_error();
    assert(message && strstr(message, "simulated"));
    assert(!file_size(audio_path));
    reset_files();

    synth_mode = 2;
    voice_begin(); voice_text("Wait. ");
    wait_bytes(requests_path, 5);
    pid_t stopped = worker_pid;
    long before = millis();
    voice_stop();
    assert(millis() - before < 1000 && !voice_poll());
    assert(waitpid(stopped, NULL, WNOHANG) < 0 && errno == ECHILD);
    reset_files();

    synth_mode = 0;
    voice_begin(); voice_text("Playing. ");
    wait_bytes(audio_path, 8);
    http_interrupted = 1;
    assert(!voice_poll());
    reset_files();

    voice_begin();
    static char overfull[sizeof queued + 2];
    memset(overfull, 'x', sizeof overfull - 1);
    voice_text(overfull);
    assert(!voice_poll());
    assert(strstr(voice_error(), "queue full"));

    /* A player can exec successfully but fail to open the audio device. */
    reset_files();
    f = fopen(executable, "w"); assert(f);
    fputs("#!/bin/sh\nexit 7\n", f); fclose(f);
    voice_begin(); voice_text("Player failure."); voice_end(); finished();
    message = voice_error();
    assert(message && strstr(message, "player"));

    assert(voice_enable(0, err, sizeof err) == 0 && !voice_enabled());
    voice_begin(); voice_text("disabled"); voice_end();
    assert(!voice_poll());
    unlink(audio_path); unlink(requests_path); unlink(executable); rmdir(tmp);
    puts("voice: early playback, UTF-8 segmentation, queue bounds, errors, cancellation, reaping, secret isolation: ok");
    return 0;
}
