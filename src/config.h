#ifndef LAMBDA_CONFIG_H
#define LAMBDA_CONFIG_H

/* lambda does no dynamic allocation: everything lives in fixed static
 * arenas sized here. BSS is lazily paged in, so unused capacity costs
 * virtual address space only, not resident memory. */

#define LAMBDA_MAX_MSGS 512              /* turns kept in history */
#define LAMBDA_HISTORY_ARENA (8u << 20)  /* history text arena */
#define LAMBDA_BODY_MAX (16u << 20)      /* request body build buffer */
#define LAMBDA_REPLY_MAX (1u << 20)      /* one streamed reply */
#define LAMBDA_SYSTEM_MAX 8192           /* system prompt */

#define LAMBDA_SSE_PENDING_MAX (64u << 10) /* unsplit sse stream bytes */
#define LAMBDA_SSE_DATA_MAX (64u << 10)    /* one sse event payload */
#define LAMBDA_ERRBODY_MAX (16u << 10)     /* non-200 response body */
#define LAMBDA_JSON_TOKENS 8192            /* jsmn tokens per payload */

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

#define LAMBDA_TRANSCRIPT_ARENA (4u << 20) /* on-screen transcript text */
#define LAMBDA_MAX_ITEMS 4096              /* transcript items */
#define LAMBDA_WRAP_LINES 65536            /* wrapped visual lines index */

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
