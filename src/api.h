#ifndef LAMBDA_API_H
#define LAMBDA_API_H

#include <stddef.h>

#define LAMBDA_DEFAULT_MODEL "claude-fable-5"

typedef struct chat chat;

chat *chat_get(void); /* the single static chat instance */

void chat_init(chat *c);
void chat_clear(chat *c); /* drop conversation history */

const char *chat_model(const chat *c);
void chat_set_model(chat *c, const char *model);

/* Models offered by /model. Any other id can still be set by hand — this is
 * the shortlist the picker shows, not a whitelist. */
typedef struct {
    const char *id;
    const char *note; /* context window · price per mtok */
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

/* extra text placed at the very front of the system prompt (project context) */
void chat_set_prelude(chat *c, const char *text);

/* Run one full exchange: send history + user_text, stream the reply,
 * execute any bash tool calls and loop until the model stops.
 * Returns 0 on success, -1 on error. */
int chat_send(chat *c, const char *user_text);

#endif
