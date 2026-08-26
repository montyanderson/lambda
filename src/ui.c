/* Fixed-frame terminal UI: a title bar, an internally-scrolling transcript,
 * and a bottom input line — built on termbox2. Falls back to plain streamed
 * stdout when not attached to a tty. No dynamic allocation: transcript text
 * lives in a static arena that compacts oldest-first when full. */

#include "ui.h"
#include "config.h"
#include "md.h"
#include "http.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "term.h"

#include <stdint.h>
#include <time.h>

/* ---- colors ------------------------------------------------------------ */
#define COL_ACCENT 208
#define COL_GREY 245
#define COL_RED 203
#define COL_TEXT TCOL_DEFAULT
#define COL_CODE 174 /* inline code / fenced blocks */
#define COL_CODE_BG 236
#define COL_TITLE_BG 208
#define COL_TITLE_FG 232

static int wants_gap(int prev_kind, int kind);
static int input_area_rows(void);
static uint32_t utf8_cp(const char *p, int len, int *i);

/* display width of the codepoint at p[i]; advances i past it */
static int adv_width(const char *p, int len, int *i)
{
    uint32_t cp = utf8_cp(p, len, i);
    return term_char_width(cp);
}

/* line-editor result codes; edit_key is shared by ui_readline and ui_pump */
#define EDIT_NONE 0
#define EDIT_SUBMIT 1
#define EDIT_QUIT 2
static int edit_key(term_event ev, int busy);
void ui_render_force(void);
static void in_set(const char *s);

static int g_tty;
static int g_active; /* termbox running */
static char g_model[128];
static char g_status[256];

/* ---- transcript store -------------------------------------------------- */
typedef struct {
    int kind;
    size_t off, len; /* into g_arena */
} item;

static char g_arena[LAMBDA_TRANSCRIPT_ARENA];
static size_t g_used;
static item g_items[LAMBDA_MAX_ITEMS];
static int g_nitems;
static int g_streaming;      /* last item is an open stream */
static int g_stream_kind = -1;

static int g_scroll;   /* top visible visual-line index */
static int g_follow = 1; /* stick to bottom */
static unsigned g_gen;   /* bumped whenever transcript content changes */

/* Renders are coalesced: streamed tokens arrive far faster than a terminal
 * (or an ssh link) can usefully repaint, so a redraw is rate-limited unless
 * the caller needs it now — typing always forces one, so it stays crisp. */
#define RENDER_MIN_MS 25
static long g_last_render_ms;
static int g_dirty;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int ui_is_tty(void)
{
    return g_tty;
}

/* drop oldest items until `need` bytes are free at the arena tail */
/* Drop the oldest `n` items and reclaim their bytes. Items are stored
 * contiguously from offset 0, so dropping a prefix frees exactly
 * g_items[n].off bytes. */
static void drop_oldest(int n)
{
    if (n <= 0)
        return;
    if (n > g_nitems)
        n = g_nitems;
    size_t freed = (n < g_nitems) ? g_items[n].off : g_used;
    memmove(g_arena, g_arena + freed, g_used - freed);
    g_used -= freed;
    for (int i = n; i < g_nitems; i++) {
        g_items[i - n] = g_items[i];
        g_items[i - n].off -= freed;
    }
    g_nitems -= n;
    g_gen++;
}

/* Free at least `need` bytes by discarding the oldest items. The newest item
 * is never dropped: it may be an open stream currently being appended to. */
static void arena_make_room(size_t need)
{
    size_t avail = sizeof g_arena - g_used;
    if (avail >= need)
        return;
    size_t deficit = need - avail;
    int drop = 0;
    while (drop < g_nitems - 1 && g_items[drop].off < deficit)
        drop++;
    drop_oldest(drop);
}

/* returns the number of bytes actually stored, which is less than `len` if
 * the arena could not be made to fit them */
static size_t arena_append(const char *s, size_t len)
{
    if (len > sizeof g_arena)
        len = sizeof g_arena;
    if (sizeof g_arena - g_used < len)
        arena_make_room(len);
    if (sizeof g_arena - g_used < len)
        len = sizeof g_arena - g_used;
    memcpy(g_arena + g_used, s, len);
    g_used += len;
    return len;
}

/* ---- headless (non-tty) backend --------------------------------------- */
static const char *H_RESET = "", *H_DIM = "", *H_ACCENT = "", *H_ERR = "",
                  *H_GREY = "";
static int g_head_bol = 1; /* at beginning of line on stdout */

static int g_head_last = -1; /* last item kind printed */

static void head_prefix_lines(const char *pfx, const char *text,
                              const char *color)
{
    /* print text with `pfx` on the first line, spaces after (color applied) */
    printf("%s%s", color, pfx);
    for (const char *p = text; *p; p++) {
        putchar(*p);
        if (*p == '\n')
            printf("  ");
    }
    printf("%s\n", H_RESET);
}

/* ---- public: init / shutdown ------------------------------------------ */
void ui_init(const char *model)
{
    snprintf(g_model, sizeof g_model, "%s", model ? model : "");
    g_tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (!g_tty) {
        if (!getenv("NO_COLOR")) {
            H_RESET = "\x1b[0m";
            H_DIM = "\x1b[2m";
            H_GREY = "\x1b[38;5;245m";
            H_ACCENT = "\x1b[38;5;208m";
            H_ERR = "\x1b[31m";
        }
        return;
    }
    if (term_init() != 0) {
        g_tty = 0;
        return;
    }
    g_active = 1;
}

void ui_shutdown(void)
{
    if (g_active) {
        term_shutdown();
        g_active = 0;
    }
}

void ui_set_model(const char *model)
{
    snprintf(g_model, sizeof g_model, "%s", model ? model : "");
}

/* ---- items ------------------------------------------------------------- */
static void item_open(int kind)
{
    if (g_nitems == LAMBDA_MAX_ITEMS)
        drop_oldest(1);
    g_items[g_nitems].kind = kind;
    g_items[g_nitems].off = g_used;
    g_items[g_nitems].len = 0;
    g_nitems++;
    g_gen++;
}

static void item_extend(const char *s, size_t len)
{
    if (g_nitems == 0)
        return;
    /* Use the count arena_append reports. Comparing g_used before and after
     * is wrong: a compaction moves g_used backwards, and the difference
     * underflows into a huge length. */
    g_items[g_nitems - 1].len += arena_append(s, len);
    g_gen++;
}

void ui_add(int kind, const char *text)
{
    if (!g_tty) {
        const char *pfx = kind == UI_USER       ? "❯ "
                          : kind == UI_ASSISTANT ? "● "
                          : kind == UI_TOOL_CMD  ? "$ "
                          : kind == UI_THINKING  ? "✻ "
                          : kind == UI_ERROR     ? "✗ "
                                                 : "  ";
        const char *col = kind == UI_ERROR ? H_ERR
                          : kind == UI_TOOL_CMD || kind == UI_TOOL_OUT ||
                                    kind == UI_THINKING
                              ? H_GREY
                          : kind == UI_NOTICE ? H_DIM
                          : kind == UI_USER   ? H_ACCENT
                                              : "";
        if (g_head_last >= 0 && wants_gap(g_head_last, kind))
            putchar('\n');
        head_prefix_lines(pfx, text, col);
        g_head_last = kind;
        g_head_bol = 1;
        return;
    }
    item_open(kind);
    item_extend(text, strlen(text));
    g_follow = 1;
    ui_render();
}

void ui_stream_begin(int kind)
{
    g_stream_kind = kind;
    if (!g_tty) {
        if (g_head_last >= 0)
            putchar('\n');
        if (kind == UI_THINKING)
            printf("%s✻ %s%s", H_GREY, H_RESET, H_GREY);
        else
            printf("%s● %s", H_ACCENT, H_RESET);
        g_head_last = kind;
        g_head_bol = 0;
        return;
    }
    item_open(kind);
    g_streaming = 1;
    g_follow = 1;
}

void ui_stream_delta(const char *text)
{
    if (!g_tty) {
        fputs(text, stdout);
        fflush(stdout);
        size_t n = strlen(text);
        if (n)
            g_head_bol = text[n - 1] == '\n';
        return;
    }
    item_extend(text, strlen(text));
    g_follow = 1;
    ui_render();
}

void ui_stream_end(void)
{
    int kind = g_stream_kind;
    g_stream_kind = -1;
    if (!g_tty) {
        if (kind == UI_THINKING)
            fputs(H_RESET, stdout);
        if (!g_head_bol)
            putchar('\n');
        g_head_bol = 1;
        return;
    }
    g_streaming = 0;
    /* drop an empty stream (e.g. the reply was only tool calls) */
    if (g_nitems > 0 && g_items[g_nitems - 1].kind == kind &&
        g_items[g_nitems - 1].len == 0)
        g_nitems--;
    g_gen++;
    ui_render_force(); /* end of a stream: make sure the tail is painted */
}

/* transient status (spinner) — never printed in headless mode */
void ui_status(const char *text)
{
    snprintf(g_status, sizeof g_status, "%s", text ? text : "");
    if (g_tty)
        ui_render();
}

/* durable per-turn note (token counts) */
void ui_status_final(const char *text)
{
    ui_status(text);
    if (!g_tty && text && *text)
        printf("%s  %s%s\n", H_DIM, text, H_RESET);
}

/* ---- word wrap into visual lines --------------------------------------- */
typedef struct {
    int kind;
    const char *p;
    int len;
    unsigned char first; /* first visual line of its item (draw gutter) */
    unsigned char sol;   /* starts a source line (leading md markers apply) */
    unsigned char fence; /* inside a ``` block */
} vline;

static vline g_vl[LAMBDA_WRAP_LINES];
static int g_nvl;
static int g_first_pending;
static int g_sol_pending;
static int g_fence_cur;

static int utf8_len(unsigned char c)
{
    if (c >= 0xf0)
        return 4;
    if (c >= 0xe0)
        return 3;
    if (c >= 0xc0)
        return 2;
    return 1;
}

static void push_vl(int kind, const char *p, int len)
{
    if (g_nvl >= LAMBDA_WRAP_LINES)
        return;
    g_vl[g_nvl].kind = kind;
    g_vl[g_nvl].p = p;
    g_vl[g_nvl].len = len;
    g_vl[g_nvl].first = (unsigned char)g_first_pending;
    g_vl[g_nvl].sol = (unsigned char)g_sol_pending;
    g_vl[g_nvl].fence = (unsigned char)g_fence_cur;
    g_first_pending = 0;
    g_sol_pending = 0;
    g_nvl++;
}

static void wrap_para(int kind, const char *p, int len, int maxcols)
{
    if (len == 0) {
        push_vl(kind, p, 0);
        return;
    }
    int start = 0, i = 0, col = 0, last_space = -1;
    while (i < len) {
        if (p[i] == ' ')
            last_space = i;
        int at = i;
        int w = adv_width(p, len, &i);
        col += w;
        /* break once this glyph would overflow the column budget */
        if (col > maxcols && at > start) {
            int brk = (last_space > start) ? last_space : at;
            push_vl(kind, p + start, brk - start);
            start = (last_space > start) ? brk + 1 : brk;
            i = start;
            col = 0;
            last_space = -1;
        }
    }
    if (start < len || start == 0)
        push_vl(kind, p + start, len - start);
}

/* blank spacer line between items — tool output stays attached to the
 * command that produced it */
static int wants_gap(int prev_kind, int kind)
{
    if (prev_kind == UI_TOOL_CMD && kind == UI_TOOL_OUT)
        return 0;
    return 1;
}

static unsigned g_vl_gen = (unsigned)-1;
static int g_vl_cols = -1;

static void build_vlines(int maxcols)
{
    if (maxcols < 4)
        maxcols = 4;
    if (g_vl_gen == g_gen && g_vl_cols == maxcols)
        return; /* nothing changed: keep the existing wrap */
    g_vl_gen = g_gen;
    g_vl_cols = maxcols;
    g_nvl = 0;
    for (int it = 0; it < g_nitems; it++) {
        const char *base = g_arena + g_items[it].off;
        int len = (int)g_items[it].len;
        if (it > 0 && wants_gap(g_items[it - 1].kind, g_items[it].kind)) {
            g_first_pending = 0;
            push_vl(UI_NOTICE, "", 0);
        }
        g_first_pending = 1;
        g_fence_cur = 0;
        int md = g_items[it].kind == UI_ASSISTANT;
        int start = 0;
        for (int i = 0; i <= len; i++) {
            if (i == len || base[i] == '\n') {
                int llen = i - start;
                g_sol_pending = 1;
                /* a fence marker line is consumed, not shown */
                if (md && md_is_fence(base + start, llen)) {
                    g_fence_cur = !g_fence_cur;
                    g_first_pending = 0;
                    start = i + 1;
                    continue;
                }
                wrap_para(g_items[it].kind, base + start, llen, maxcols);
                start = i + 1;
            }
        }
    }
}

/* ---- rendering --------------------------------------------------------- */
static void gutter_for(int kind, const char **sym, int *fg)
{
    switch (kind) {
    case UI_USER:      *sym = "❯ "; *fg = COL_ACCENT; break;
    case UI_ASSISTANT: *sym = "● "; *fg = COL_ACCENT; break;
    case UI_THINKING:  *sym = "✻ "; *fg = COL_GREY;   break;
    case UI_TOOL_CMD:  *sym = "$ "; *fg = COL_GREY;   break;
    case UI_ERROR:     *sym = "✗ "; *fg = COL_RED;    break;
    default:           *sym = "  "; *fg = COL_GREY;   break;
    }
}

static int text_fg(int kind)
{
    switch (kind) {
    case UI_TOOL_CMD:
    case UI_TOOL_OUT:
    case UI_THINKING:
    case UI_NOTICE: return COL_GREY;
    case UI_ERROR:  return COL_RED;
    default:        return COL_TEXT;
    }
}

static int text_attr(int kind)
{
    if (kind == UI_THINKING)
        return TATTR_DIM | TATTR_ITALIC;
    return kind == UI_NOTICE ? TATTR_DIM : 0;
}

/* byte length of the first `cols` codepoints of p (never splits a sequence) */
static int clamp_cols(const char *p, int len, int cols)
{
    int i = 0, n = 0;
    while (i < len) {
        int at = i;
        int w = adv_width(p, len, &i);
        if (n + w > cols) {
            i = at;
            break;
        }
        n += w;
    }
    return i < len ? i : len;
}

static void draw_span(int x, int y, int fg, int attr, const char *p, int len)
{
    char tmp[4096];
    if (len > (int)sizeof tmp - 1)
        len = (int)sizeof tmp - 1;
    memcpy(tmp, p, (size_t)len);
    tmp[len] = '\0';
    term_print(x, y, tmp, fg, TCOL_DEFAULT, attr);
}

/* decode one utf-8 codepoint; advances *i */
static uint32_t utf8_cp(const char *p, int len, int *i)
{
    unsigned char c = (unsigned char)p[*i];
    int n = utf8_len(c);
    if (*i + n > len)
        n = 1;
    uint32_t cp;
    switch (n) {
    case 2: cp = c & 0x1Fu; break;
    case 3: cp = c & 0x0Fu; break;
    case 4: cp = c & 0x07u; break;
    default: cp = c; break;
    }
    for (int k = 1; k < n; k++)
        cp = (cp << 6) | ((unsigned char)p[*i + k] & 0x3Fu);
    *i += n;
    return cp;
}

/* draw a visual line with basic markdown styling applied */
static void draw_markdown(int x, int y, const vline *v, int maxx)
{
    unsigned char flags[4096];
    int len = v->len;
    if (len > (int)sizeof flags)
        len = (int)sizeof flags;
    md_state st;
    st.in_fence = v->fence;
    md_style(&st, v->p, len, flags, v->sol);

    int i = 0;
    while (i < len && x < maxx) {
        unsigned char f = flags[i];
        int at = i;
        uint32_t cp = utf8_cp(v->p, len, &i);
        if (f & MD_HIDE)
            continue;

        int fg = COL_TEXT, bg = TCOL_DEFAULT, attr = 0;
        if (f & MD_CODE) {
            fg = COL_CODE;
            bg = COL_CODE_BG;
        } else if (f & MD_HEADING) {
            fg = COL_ACCENT;
        } else if (f & MD_QUOTE) {
            fg = COL_GREY;
            attr |= TATTR_ITALIC;
        }
        if (f & MD_BOLD)
            attr |= TATTR_BOLD;
        if (f & MD_ITALIC)
            attr |= TATTR_ITALIC;
        if (f & MD_BULLET) {
            fg = COL_ACCENT;
            if (v->p[at] == '-' || v->p[at] == '*' || v->p[at] == '+')
                cp = 0x2022; /* • */
        }
        int w = term_char_width(cp);
        if (w == 0)
            continue; /* combining mark: no cell of its own */
        if (x + w > maxx)
            break;
        term_set(x, y, cp, fg, bg, attr);
        x += w;
    }
}

/* ---- frame ------------------------------------------------------------- */

/* input editor state (persists for cursor placement across renders) */
static char g_inbuf[LAMBDA_LINE_MAX];
static size_t g_inlen, g_inpos;
static const char *g_inprompt = "";
static int g_input_active;
static char g_badge[128]; /* right-hand top-border indicators */

/* prompts typed while a turn was running, waiting their turn */
#define QUEUE_MAX 16
#define QUEUE_SHOWN 3
static char g_queue[QUEUE_MAX][LAMBDA_LINE_MAX];
static int g_qn;

int ui_queued_count(void) { return g_qn; }

int ui_take_queued(char *out, size_t cap)
{
    if (g_qn == 0)
        return 0;
    snprintf(out, cap, "%s", g_queue[0]);
    memmove(g_queue[0], g_queue[1], sizeof(g_queue[0]) * (size_t)(g_qn - 1));
    g_qn--;
    if (g_active)
        ui_render();
    return 1;
}

static void queue_push(const char *s)
{
    if (!*s)
        return;
    if (g_qn >= QUEUE_MAX)
        return;
    snprintf(g_queue[g_qn++], LAMBDA_LINE_MAX, "%s", s);
}

#define IN_MAX_ROWS 6  /* input box grows to this many lines, then scrolls */
#define PAD 1          /* blank columns just inside the frame */
#define GUTTER 2       /* "● " etc. */

/* byte offset of each wrapped input row */
static size_t g_row_off[1024];
static int g_row_n;

/* wrap the input buffer to `iw` columns; reports where the cursor lands */
static void layout_input(int iw, int *cur_row, int *cur_col)
{
    g_row_n = 0;
    g_row_off[g_row_n++] = 0;
    int col = 0;
    *cur_row = 0;
    *cur_col = 0;
    int seen_cursor = 0;
    for (size_t i = 0; i <= g_inlen;) {
        if (i == g_inpos && !seen_cursor) {
            *cur_row = g_row_n - 1;
            *cur_col = col;
            seen_cursor = 1;
        }
        if (i == g_inlen)
            break;
        int ii = (int)i;
        int w = adv_width(g_inbuf, (int)g_inlen, &ii);
        i = (size_t)ii;
        col += w;
        if (col >= iw) {
            if (g_row_n < (int)(sizeof g_row_off / sizeof g_row_off[0]))
                g_row_off[g_row_n++] = i;
            col = 0;
        }
    }
}

static void hline(int y, int x0, int x1, int fg)
{
    for (int x = x0; x <= x1; x++)
        term_set(x, y, 0x2500, fg, TCOL_DEFAULT, 0); /* ─ */
}

/* top border with the title inlaid: ╭─ λ lambda · model ───────╮ */
static void draw_top(int W)
{
    int fg = COL_GREY;
    term_set(0, 0, 0x256D, fg, TCOL_DEFAULT, 0);      /* ╭ */
    term_set(W - 1, 0, 0x256E, fg, TCOL_DEFAULT, 0);  /* ╮ */
    hline(0, 1, W - 2, fg);

    int x = 2;
    x += term_print(x, 0, " λ ", COL_ACCENT, TCOL_DEFAULT, TATTR_BOLD);
    x += term_print(x, 0, "lambda ", COL_TEXT, TCOL_DEFAULT, TATTR_BOLD);
    x += term_print(x, 0, " · ", COL_GREY, TCOL_DEFAULT, TATTR_DIM);
    x += term_print(x, 0, g_model, COL_ACCENT, TCOL_DEFAULT, 0);
    term_set(x, 0, ' ', fg, TCOL_DEFAULT, 0);

    /* right-hand badges, when they differ from the defaults */
    if (g_badge[0]) {
        int wlen = 0;
        for (const char *p = g_badge; *p; p += utf8_len((unsigned char)*p))
            wlen++;
        int bx = W - 3 - wlen;
        if (bx > x + 2) {
            term_set(bx - 1, 0, ' ', fg, TCOL_DEFAULT, 0);
            term_print(bx, 0, g_badge, COL_GREY, TCOL_DEFAULT, TATTR_DIM);
            term_set(bx + wlen, 0, ' ', fg, TCOL_DEFAULT, 0);
        }
    }
}

void ui_badge(const char *text)
{
    snprintf(g_badge, sizeof g_badge, "%s", text ? text : "");
    if (g_active)
        ui_render();
}

/* paint now */
static void render_now(void)
{
    g_last_render_ms = now_ms();
    g_dirty = 0;
    int W = term_width(), H = term_height();
    if (W < 28 || H < 10) { /* too small to frame: bail out cleanly */
        term_clear();
        term_present();
        return;
    }
    term_clear();

    /* text column: inside the frame, past the padding and the gutter,
     * leaving a column for the scrollbar */
    int tx = 1 + PAD + GUTTER;
    int maxcols = W - 1 - PAD - tx;
    if (maxcols < 8)
        maxcols = 8;

    /* input box height depends only on the width, so lay it out first */
    int iw = W - 1 - PAD - tx + 1;
    if (iw < 8)
        iw = 8;
    int cur_row = 0, cur_col = 0;
    layout_input(iw, &cur_row, &cur_col);
    int ih = g_row_n < IN_MAX_ROWS ? g_row_n : IN_MAX_ROWS;
    if (ih < 1)
        ih = 1;
    int qrows = input_area_rows() - ih;

    int hint_y = H - 1;      /* outside the frame */
    int bottom_y = H - 2;    /* ╰────╯ */
    int in_y0 = bottom_y - ih;
    int q_y0 = in_y0 - qrows;
    int div_y = q_y0 - 1;    /* ├────┤ */
    int th = div_y - 1;      /* transcript rows: 1 .. div_y-1 */

    build_vlines(maxcols);

    if (g_follow)
        g_scroll = g_nvl > th ? g_nvl - th : 0;
    if (g_scroll > g_nvl - th)
        g_scroll = g_nvl > th ? g_nvl - th : 0;
    if (g_scroll < 0)
        g_scroll = 0;

    draw_top(W);

    /* frame sides for every row between the borders */
    for (int y = 1; y < bottom_y; y++) {
        term_set(0, y, 0x2502, COL_GREY, TCOL_DEFAULT, 0);      /* │ */
        term_set(W - 1, y, 0x2502, COL_GREY, TCOL_DEFAULT, 0);
    }

    /* transcript */
    for (int row = 0; row < th; row++) {
        int vi = g_scroll + row;
        if (vi >= g_nvl)
            break;
        vline *v = &g_vl[vi];
        const char *sym;
        int gfg;
        gutter_for(v->kind, &sym, &gfg);
        if (v->first)
            term_print(1 + PAD, 1 + row, sym, gfg, TCOL_DEFAULT, TATTR_BOLD);
        if (v->kind == UI_ASSISTANT)
            draw_markdown(tx, 1 + row, v, tx + maxcols);
        else
            draw_span(tx, 1 + row, text_fg(v->kind), text_attr(v->kind), v->p,
                      v->len);
    }

    /* scroll thumb, drawn onto the right frame edge so it costs no columns */
    if (g_nvl > th) {
        int barh = th * th / g_nvl;
        if (barh < 1)
            barh = 1;
        int bary = g_scroll * th / g_nvl;
        for (int i = bary; i < bary + barh && i < th; i++)
            term_set(W - 1, 1 + i, 0x2588, COL_ACCENT, TCOL_DEFAULT, 0);
    }

    /* divider above the input box */
    term_set(0, div_y, 0x251C, COL_GREY, TCOL_DEFAULT, 0);      /* ├ */
    term_set(W - 1, div_y, 0x2524, COL_GREY, TCOL_DEFAULT, 0);  /* ┤ */
    hline(div_y, 1, W - 2, COL_GREY);

    /* bottom border */
    term_set(0, bottom_y, 0x2570, COL_GREY, TCOL_DEFAULT, 0);     /* ╰ */
    term_set(W - 1, bottom_y, 0x256F, COL_GREY, TCOL_DEFAULT, 0); /* ╯ */
    hline(bottom_y, 1, W - 2, COL_GREY);

    /* queued prompts, above the line being edited */
    for (int i = 0; i < qrows; i++) {
        int y = q_y0 + i;
        if (g_qn > QUEUE_SHOWN && i == qrows - 1) {
            char more[64];
            snprintf(more, sizeof more, "+%d more queued",
                     g_qn - QUEUE_SHOWN);
            term_print(tx, y, more, COL_GREY, TCOL_DEFAULT, TATTR_DIM);
            break;
        }
        /* single-width glyph: the cell grid assumes one column per
         * codepoint, so no double-width characters in the chrome */
        term_print(1 + PAD, y, "» ", COL_GREY, TCOL_DEFAULT, TATTR_DIM);
        draw_span(tx, y, COL_GREY, TATTR_DIM, g_queue[i],
                  clamp_cols(g_queue[i], (int)strlen(g_queue[i]), maxcols));
    }

    /* input box contents */
    if (g_input_active) {
        int first = 0;
        if (g_row_n > ih)
            first = cur_row >= ih ? cur_row - ih + 1 : 0;
        if (first > g_row_n - ih)
            first = g_row_n - ih;
        if (first < 0)
            first = 0;

        for (int r = 0; r < ih; r++) {
            int row = first + r;
            if (row >= g_row_n)
                break;
            if (r == 0 || row == 0)
                term_print(1 + PAD, in_y0 + r, g_inprompt, COL_ACCENT,
                           TCOL_DEFAULT, TATTR_BOLD);
            size_t s0 = g_row_off[row];
            size_t s1 = (row + 1 < g_row_n) ? g_row_off[row + 1] : g_inlen;
            draw_span(tx, in_y0 + r, COL_TEXT, 0, g_inbuf + s0, (int)(s1 - s0));
        }
        term_show_cursor(tx + cur_col, in_y0 + (cur_row - first));
    }

    /* hint / status line, below the frame */
    char hint[512];
    if (g_status[0])
        snprintf(hint, sizeof hint, "%s", g_status);
    else
        snprintf(hint, sizeof hint,
                 "enter send · pgup/pgdn or wheel scroll · ctrl-c interrupt "
                 "· ctrl-d quit");
    term_print(1 + PAD, hint_y, hint, COL_GREY, TCOL_DEFAULT, TATTR_DIM);

    term_present();
}

void ui_render_force(void)
{
    if (g_active)
        render_now();
}

void ui_render(void)
{
    if (!g_active)
        return;
    if (now_ms() - g_last_render_ms < RENDER_MIN_MS) {
        g_dirty = 1; /* a later call, or ui_pump, will pick this up */
        return;
    }
    render_now();
}

/* ---- event helpers ----------------------------------------------------- */
/* rows the input box occupies: queued prompts plus the line being edited */
static int input_area_rows(void)
{
    int q = g_qn < QUEUE_SHOWN ? g_qn : QUEUE_SHOWN;
    if (g_qn > QUEUE_SHOWN)
        q++; /* "+N more" line */
    int ih = g_row_n < IN_MAX_ROWS ? g_row_n : IN_MAX_ROWS;
    if (ih < 1)
        ih = 1;
    return q + ih;
}

/* transcript rows and text width, matching ui_render's layout */
static int view_rows(void)
{
    return term_height() - 4 - input_area_rows();
}

static int view_cols(void)
{
    int tx = 1 + PAD + GUTTER;
    int c = term_width() - 1 - PAD - tx;
    return c < 8 ? 8 : c;
}

static void scroll_by(int delta)
{
    int th = view_rows();
    build_vlines(view_cols());
    g_scroll += delta;
    if (g_scroll < 0)
        g_scroll = 0;
    if (g_scroll >= g_nvl - th) {
        g_scroll = g_nvl > th ? g_nvl - th : 0;
        g_follow = 1;
    } else {
        g_follow = 0;
    }
}

/* handle scroll/resize; 1 if consumed */
static int handle_view_event(term_event ev)
{
    int th = view_rows();
    switch (ev.type) {
    case K_RESIZE:
        ui_render_force();
        return 1;
    case K_WHEEL_UP:
        scroll_by(-3);
        ui_render_force();
        return 1;
    case K_WHEEL_DOWN:
        scroll_by(3);
        ui_render_force();
        return 1;
    case K_PGUP:
        scroll_by(-(th - 1));
        ui_render_force();
        return 1;
    case K_PGDN:
        scroll_by(th - 1);
        ui_render_force();
        return 1;
    default:
        return 0;
    }
}

int ui_pump(void)
{
    if (!g_active)
        return 0;
    int interrupt = 0, edited = 0;
    for (;;) {
        term_event ev = term_poll(0);
        if (ev.type == K_NONE || ev.type == K_EOF)
            break;
        if (ev.type == K_CTRL && ev.ch == 'c') {
            /* while busy, ctrl-c means "stop what you're doing" */
            interrupt = 1;
            http_interrupted = 1;
            continue;
        }
        if (handle_view_event(ev))
            continue;
        /* keep the input box live: enter queues rather than sends */
        int r = edit_key(ev, 1);
        if (r == EDIT_SUBMIT) {
            queue_push(g_inbuf);
            in_set("");
        }
        edited = 1;
    }
    if (edited)
        ui_render_force(); /* keystrokes must feel instant */
    else if (g_dirty)
        ui_render();
    return interrupt;
}

/* ---- line editor ------------------------------------------------------- */
static char g_hist[LAMBDA_HIST_MAX][LAMBDA_LINE_MAX];
static int g_hist_n;

static void hist_add(const char *s)
{
    if (!*s)
        return;
    if (g_hist_n > 0 && strcmp(g_hist[g_hist_n - 1], s) == 0)
        return;
    if (g_hist_n == LAMBDA_HIST_MAX) {
        memmove(g_hist[0], g_hist[1],
                sizeof(g_hist[0]) * (LAMBDA_HIST_MAX - 1));
        g_hist_n--;
    }
    snprintf(g_hist[g_hist_n++], LAMBDA_LINE_MAX, "%s", s);
}

static size_t prev_cp(const char *s, size_t pos)
{
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xc0) == 0x80)
        pos--;
    return pos;
}
static size_t next_cp(const char *s, size_t len, size_t pos)
{
    if (pos >= len)
        return len;
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xc0) == 0x80)
        pos++;
    return pos;
}

static int g_hix;                     /* history cursor */
static char g_saved[LAMBDA_LINE_MAX]; /* edit stashed while browsing */

static void in_set(const char *s)
{
    snprintf(g_inbuf, sizeof g_inbuf, "%s", s);
    g_inlen = strlen(g_inbuf);
    g_inpos = g_inlen;
}

/* headless line read */
static int head_readline(char *out, size_t cap)
{
    if (!fgets(out, (int)cap, stdin))
        return 0;
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';
    return 1;
}


/* One key of line editing. `busy` is set when a turn is in flight, which only
 * changes what ctrl-d does (never quit mid-turn). */
static int edit_key(term_event ev, int busy)
{
    switch (ev.type) {
    case K_ENTER:
        return EDIT_SUBMIT;
    case K_BACKSPACE:
        if (g_inpos > 0) {
            size_t p = prev_cp(g_inbuf, g_inpos);
            memmove(g_inbuf + p, g_inbuf + g_inpos, g_inlen - g_inpos + 1);
            g_inlen -= g_inpos - p;
            g_inpos = p;
        }
        break;
    case K_DELETE:
        if (g_inpos < g_inlen) {
            size_t nx = next_cp(g_inbuf, g_inlen, g_inpos);
            memmove(g_inbuf + g_inpos, g_inbuf + nx, g_inlen - nx + 1);
            g_inlen -= nx - g_inpos;
        }
        break;
    case K_LEFT:
        g_inpos = prev_cp(g_inbuf, g_inpos);
        break;
    case K_RIGHT:
        g_inpos = next_cp(g_inbuf, g_inlen, g_inpos);
        break;
    case K_HOME:
        g_inpos = 0;
        break;
    case K_END:
        g_inpos = g_inlen;
        break;
    case K_UP:
        if (g_hix > 0) {
            if (g_hix == g_hist_n)
                snprintf(g_saved, sizeof g_saved, "%s", g_inbuf);
            in_set(g_hist[--g_hix]);
        }
        break;
    case K_DOWN:
        if (g_hix < g_hist_n) {
            g_hix++;
            in_set(g_hix == g_hist_n ? g_saved : g_hist[g_hix]);
        }
        break;
    case K_CTRL:
        switch (ev.ch) {
        case 'c':
            in_set("");
            break;
        case 'd':
            if (g_inlen == 0 && !busy)
                return EDIT_QUIT;
            break;
        case 'a':
            g_inpos = 0;
            break;
        case 'e':
            g_inpos = g_inlen;
            break;
        case 'k':
            g_inlen = g_inpos;
            g_inbuf[g_inlen] = '\0';
            break;
        case 'u':
            memmove(g_inbuf, g_inbuf + g_inpos, g_inlen - g_inpos + 1);
            g_inlen -= g_inpos;
            g_inpos = 0;
            break;
        case 'w': {
            size_t p = g_inpos;
            while (p > 0 && g_inbuf[p - 1] == ' ')
                p--;
            while (p > 0 && g_inbuf[p - 1] != ' ')
                p--;
            memmove(g_inbuf + p, g_inbuf + g_inpos, g_inlen - g_inpos + 1);
            g_inlen -= g_inpos - p;
            g_inpos = p;
            break;
        }
        default:
            break;
        }
        break;
    case K_CHAR: {
        char u[4];
        int n = 0;
        uint32_t cp = ev.ch;
        if (cp < 0x80) {
            u[n++] = (char)cp;
        } else if (cp < 0x800) {
            u[n++] = (char)(0xC0 | (cp >> 6));
            u[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            u[n++] = (char)(0xE0 | (cp >> 12));
            u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            u[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            u[n++] = (char)(0xF0 | (cp >> 18));
            u[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            u[n++] = (char)(0x80 | (cp & 0x3F));
        }
        if (g_inlen + (size_t)n < sizeof g_inbuf) {
            memmove(g_inbuf + g_inpos + n, g_inbuf + g_inpos,
                    g_inlen - g_inpos + 1);
            memcpy(g_inbuf + g_inpos, u, (size_t)n);
            g_inlen += (size_t)n;
            g_inpos += (size_t)n;
        }
        break;
    }
    default:
        break;
    }
    return EDIT_NONE;
}

int ui_readline(const char *prompt, char *out, size_t cap)
{
    if (!g_tty)
        return head_readline(out, cap);

    /* a queued prompt is served without waiting for a keystroke */
    if (ui_take_queued(out, cap))
        return 1;

    g_inprompt = prompt;
    g_input_active = 1;
    g_hix = g_hist_n;
    /* forced, not coalesced: we are about to block, so anything still
     * pending (a resumed transcript, startup notices) must be on screen */
    ui_render_force();

    int rc = 0;
    for (;;) {
        /* A coalesced repaint may still be pending. Blocking indefinitely
         * with one outstanding would leave a stale frame on screen until the
         * next keystroke, which looks like the display is stuck mid-scroll. */
        term_event ev = term_poll(g_dirty ? RENDER_MIN_MS : -1);
        if (ev.type == K_NONE) {
            if (g_dirty)
                ui_render_force();
            continue;
        }
        if (ev.type == K_EOF) {
            rc = 0;
            break;
        }
        if (handle_view_event(ev))
            continue;
        int r = edit_key(ev, 0);
        if (r == EDIT_SUBMIT) {
            snprintf(out, cap, "%s", g_inbuf);
            hist_add(g_inbuf);
            in_set("");
            ui_render_force(); /* the line clears immediately */
            rc = 1;
            break;
        }
        if (r == EDIT_QUIT) {
            rc = 0;
            break;
        }
        ui_render_force();
    }
    ui_render();
    return rc;
}
