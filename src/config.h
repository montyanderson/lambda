#ifndef LAMBDA_CONFIG_H
#define LAMBDA_CONFIG_H

/* Conversation history, request/reply buffers and scrollback grow. The
 * remaining protocol, tool and UI scratch buffers have explicit limits. */

#define LAMBDA_SUMMARY_CHUNK_MAX (4u << 20) /* max source bytes per summary request */
#define LAMBDA_SYSTEM_MAX 8192           /* system prompt */
#define LAMBDA_MODEL_ID_MAX 512
#define LAMBDA_MODELS_MAX 1024
#define LAMBDA_MODELS_BODY_MAX (2u << 20)
#define LAMBDA_SUMMARY_MAX (32u << 10)
#define LAMBDA_UNKNOWN_CONTEXT 32768

#define LAMBDA_SSE_PENDING_MAX (64u << 10) /* unsplit sse stream bytes */
#define LAMBDA_SSE_DATA_MAX (64u << 10)    /* one sse event payload */
#define LAMBDA_ERRBODY_MAX (16u << 10)     /* non-200 response body */
#define LAMBDA_JSON_TOKENS 32768           /* includes large model catalogs */

#define LAMBDA_REQ_HDRS_MAX 8192        /* extra request headers */
#define LAMBDA_RESP_HDR_MAX (32u << 10) /* response header block */
#define LAMBDA_RESP_HDR_COUNT 64        /* parsed response headers */

#define LAMBDA_TA_MAX 512            /* trust anchors */
#define LAMBDA_TA_POOL (1u << 20)    /* dn + public key bytes */

#define LAMBDA_LINE_MAX 8192 /* input line */
#define LAMBDA_HIST_MAX 64   /* input history entries */
#define LAMBDA_STDIN_MAX (1u << 20) /* piped stdin prompt */

#define LAMBDA_PATH_MAX 4096
#define LAMBDA_CONTEXT_MAX (256u << 10) /* AGENTS.md / CLAUDE.md text */
#define LAMBDA_CONTEXT_FILES 16         /* how many such files to load */
#define LAMBDA_SESSION_QUEUE (1u << 20) /* buffered session-log bytes */

#ifndef LAMBDA_MAX_ITEMS
#define LAMBDA_MAX_ITEMS 64                /* initial transcript item capacity */
#endif
#ifndef LAMBDA_TABLE_ARENA
#define LAMBDA_TABLE_ARENA (256u << 10)    /* formatted markdown tables */
#endif

#define LAMBDA_PLUGINS_MAX 32          /* registered plugin tools */
#define LAMBDA_PLUGIN_OUT_MAX (512u << 10) /* raw plugin http response */
#define LAMBDA_TOOL_CALLS_MAX 8        /* parallel tool_use blocks per turn */
#define LAMBDA_TOOL_INPUT_MAX 16384    /* raw tool input json */
#define LAMBDA_TOOL_OUTPUT_MAX 65536   /* captured command output (to model) */
/* One serialised api message. JSON escaping can expand a byte up to 6x
 * (\u00XX for control characters), so this must clear the worst case for a
 * full tool result or the message cannot be stored at all. */
#define LAMBDA_MSG_MAX (LAMBDA_TOOL_OUTPUT_MAX * 6 + 16384)
#define LAMBDA_TOOL_TURNS_MAX 64       /* agentic loop iterations per prompt */
#define LAMBDA_THINKING_MAX (256u << 10) /* replayed thinking blocks / turn */

#endif
