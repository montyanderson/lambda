#ifndef LAMBDA_API_H
#define LAMBDA_API_H

#include <stddef.h>

#define LAMBDA_DEFAULT_MODEL "claude-fable-5"

typedef struct chat chat;

chat *chat_get(void); /* the single static chat instance */

void chat_init(chat *c);
void chat_destroy(chat *c);
void chat_clear(chat *c); /* drop conversation history */
/* Reserve for the selected model. Estimates bytes from token metadata, then
 * grows as needed; a token limit is not a byte or conversation-length limit. */
int chat_prepare_storage(chat *c);

const char *chat_model(const chat *c);
/* Reject empty or oversized ids instead of silently selecting another model. */
int chat_set_model(chat *c, const char *model);
void chat_set_max_tokens(chat *c, int n);
void chat_set_context_window(chat *c, long tokens); /* 0 = model metadata */
long chat_context_window(chat *c);
long chat_context_tokens(chat *c); /* conservative estimate + observed usage */
long chat_context_threshold(chat *c);
const char *chat_summary(const chat *c);
int chat_set_summary(chat *c, const char *text);
/* Summarise older history; 1 compacted, 0 nothing to compact, -1 failed. */
int chat_compact(chat *c, char *err, size_t errsz);

/* Models offered by /model. Any other id can still be set by hand — this is
 * the shortlist the picker shows, not a whitelist. */
typedef struct {
    const char *id;
    const char *note; /* context window · price per mtok */
    long context_tokens, output_tokens; /* 0 when unknown */
} model_info;

int chat_model_count(void);
const model_info *chat_model_at(int i);

void chat_set_system(chat *c, const char *system);
void chat_set_fallbacks(chat *c, int on);
/* output_config.effort: "low".."max", or NULL/"" to leave it to the api */
void chat_set_effort(chat *c, const char *effort);
const char *chat_effort(const chat *c);
/* show the model's summarised reasoning while it works */
void chat_set_show_thinking(chat *c, int on);
void chat_set_tools(chat *c, int on);
int chat_tools_enabled(const chat *c);

/* history access, for the session log */
int chat_msg_count(const chat *c);
const char *chat_msg(const chat *c, int i, size_t *len);
/* append a verbatim api message object (used when resuming) */
int chat_push_raw(chat *c, const char *json, size_t len);
/* Complete retained history, including messages summarised out of requests. */
size_t chat_history_count(const chat *c);
size_t chat_history_bytes(const chat *c);
size_t chat_history_capacity(const chat *c);
const char *chat_history_msg(const chat *c, size_t i, size_t *len);
/* Transcript replay operations; never call the model. Compaction changes only
 * the active request selection. A drop removes rolled-back tail messages. */
int chat_restore_compact(chat *c, int drop, int pin, const char *summary);
void chat_drop(chat *c, int n);

/* extra text placed at the very front of the system prompt (project context) */
void chat_set_prelude(chat *c, const char *text);

/* Run one full exchange: send history + user_text, stream the reply,
 * execute any bash tool calls and loop until the model stops.
 * Returns 0 on success, -1 on error. */
int chat_send(chat *c, const char *user_text);

#endif
