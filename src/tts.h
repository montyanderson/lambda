#ifndef LAMBDA_TTS_H
#define LAMBDA_TTS_H

#include <stddef.h>

/* Byte limit also keeps every request below Inworld's 4,000 UTF-16 units. */
#define TTS_TEXT_MAX 1200

/* Blocking, streaming MP3 synthesis. Run outside the UI process: the HTTP
 * and JSON parsers use static storage. The sink provides backpressure. */
int tts_stream(const char *text, int (*audio)(const char *, size_t, void *),
               void *ud, char *err, size_t cap);
int tts_configured(char *err, size_t cap);
int tts_load_voice(char *err, size_t cap);
const char *tts_voice_id(void);

#endif
