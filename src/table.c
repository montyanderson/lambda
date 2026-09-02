#include "table.h"

#include "md.h"
#include "term.h"

#include <stdint.h>
#include <string.h>

/* Styling markers inside a cell are kept in the output and marked MD_HIDE,
 * exactly as md.c does for ordinary lines, so the renderer skips them. Cell
 * flags are computed once per cell into this scratch; a cell longer than the
 * scratch keeps its text but loses styling past the cut, which only a
 * pathological cell reaches. */
#define CELL_FLAGS_MAX 2048
#define ROW_MAX 4096 /* one formatted row, including hidden markers */

#define AL_LEFT 0
#define AL_RIGHT 1
#define AL_CENTER 2

typedef struct {
    const char *p;
    int len;
    const unsigned char *f;
    int flen; /* bytes of `f` that are valid; past it, flags are 0 */
} cell;

static unsigned char g_cf[TABLE_MAX_COLS][CELL_FLAGS_MAX];
static char g_row[ROW_MAX];
static unsigned char g_rowf[ROW_MAX];
static int g_rn;

static int is_space(char c) { return c == ' ' || c == '\t'; }

static unsigned char cflag(const cell *c, int i)
{
    return i < c->flen ? c->f[i] : 0;
}

/* bytes up to the next newline (or the end) */
static int line_len(const char *p, int len)
{
    int i = 0;
    while (i < len && p[i] != '\n')
        i++;
    return i;
}

static int has_pipe(const char *p, int len)
{
    for (int i = 0; i < len; i++)
        if (p[i] == '|')
            return 1;
    return 0;
}

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

/* Split one source line into trimmed cells. Outer pipes are optional; \|
 * does not split. Returns the cell count, which may exceed `maxc` (only the
 * first `maxc` are stored) so the caller can reject oversized tables. */
static int split_row(const char *p, int len, cell *cells, int maxc)
{
    int i = 0, n = 0;
    while (i < len && is_space(p[i]))
        i++;
    if (i < len && p[i] == '|')
        i++;
    while (len > i && is_space(p[len - 1]))
        len--;
    if (len > i && p[len - 1] == '|' && (len - 2 < i || p[len - 2] != '\\'))
        len--;

    while (i <= len) {
        int s = i;
        while (i < len && !(p[i] == '|' && (i == s || p[i - 1] != '\\')))
            i++;
        int e = i;
        while (s < e && is_space(p[s]))
            s++;
        while (e > s && is_space(p[e - 1]))
            e--;
        if (n < maxc) {
            cells[n].p = p + s;
            cells[n].len = e - s;
            cells[n].f = NULL;
            cells[n].flen = 0;
        }
        n++;
        if (i >= len)
            break;
        i++; /* past the pipe */
    }
    return n;
}

/* a delimiter cell is -+, optionally colon-anchored on either end */
static int delim_align(const char *p, int len, int *align)
{
    int i = 0, dash = 0, left = 0, right = 0;
    if (i < len && p[i] == ':') {
        left = 1;
        i++;
    }
    while (i < len && p[i] == '-') {
        dash++;
        i++;
    }
    if (i < len && p[i] == ':') {
        right = 1;
        i++;
    }
    if (i != len || dash == 0)
        return 0;
    *align = right ? (left ? AL_CENTER : AL_RIGHT) : AL_LEFT;
    return 1;
}

/* Attach style flags to a cell, computed into slot `col`. */
static void cell_style(cell *c, int col)
{
    int n = c->len < CELL_FLAGS_MAX ? c->len : CELL_FLAGS_MAX;
    md_state st;
    st.in_fence = 0;
    md_style(&st, c->p, n, g_cf[col], 0);
    /* \| is an escaped pipe: drop the backslash, keep the bar */
    for (int i = 0; i + 1 < n; i++)
        if (c->p[i] == '\\' && c->p[i + 1] == '|')
            g_cf[col][i] |= MD_HIDE;
    c->f = g_cf[col];
    c->flen = n;
}

/* Longest run from `off` whose display width fits `w`, breaking on a space
 * where it can. `next` is where the following segment starts. */
static int seg_end(const cell *c, int off, int w, int *next, int *width)
{
    int i = off, col = 0, last_space = -1, space_col = 0;
    while (i < c->len) {
        int at = i;
        unsigned char f = cflag(c, i);
        uint32_t cp = utf8_cp(c->p, c->len, &i);
        if (f & MD_HIDE)
            continue; /* a marker takes no column */
        int cw = term_char_width(cp);
        if (col + cw > w) {
            if (last_space > off) {
                *next = last_space + 1;
                *width = space_col;
                return last_space;
            }
            if (at == off) { /* one glyph wider than the column: keep it */
                *next = i;
                *width = cw;
                return i;
            }
            *next = at;
            *width = col;
            return at;
        }
        if (cp == ' ') {
            last_space = at;
            space_col = col;
        }
        col += cw;
    }
    *next = c->len;
    *width = col;
    return c->len;
}

/* visible width of a whole cell */
static int cell_width(const cell *c)
{
    int i = 0, col = 0;
    while (i < c->len) {
        unsigned char f = cflag(c, i);
        uint32_t cp = utf8_cp(c->p, c->len, &i);
        if (!(f & MD_HIDE))
            col += term_char_width(cp);
    }
    return col;
}

/* ---- row assembly ------------------------------------------------------ */
static void put_cp(uint32_t cp, unsigned char fl)
{
    char u[4];
    int n = 0;
    if (cp < 0x80) {
        u[n++] = (char)cp;
    } else if (cp < 0x800) {
        u[n++] = (char)(0xC0 | (cp >> 6));
        u[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        u[n++] = (char)(0xE0 | (cp >> 12));
        u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[n++] = (char)(0x80 | (cp & 0x3F));
    }
    if (g_rn + n > ROW_MAX)
        return;
    for (int k = 0; k < n; k++) {
        g_row[g_rn] = u[k];
        g_rowf[g_rn++] = fl;
    }
}

static void put_seg(const cell *c, int s, int e, unsigned char extra)
{
    for (int i = s; i < e && g_rn < ROW_MAX; i++) {
        g_row[g_rn] = c->p[i];
        g_rowf[g_rn++] = (unsigned char)(cflag(c, i) | extra);
    }
}

static void emit_border(const int *w, int nc, uint32_t l, uint32_t mid,
                        uint32_t r, table_row_fn emit, void *ud)
{
    g_rn = 0;
    put_cp(l, MD_TBORDER);
    for (int c = 0; c < nc; c++) {
        for (int k = 0; k < w[c] + 2; k++)
            put_cp(0x2500, MD_TBORDER); /* ─ */
        put_cp(c + 1 < nc ? mid : r, MD_TBORDER);
    }
    emit(ud, g_row, g_rowf, g_rn);
}

/* one logical row, wrapped across as many screen rows as its cells need */
static void emit_row(cell *cells, int nc, const int *w, const int *align,
                     unsigned char extra, table_row_fn emit, void *ud)
{
    int off[TABLE_MAX_COLS];
    for (int c = 0; c < nc; c++)
        off[c] = 0;
    for (;;) {
        g_rn = 0;
        put_cp(0x2502, MD_TBORDER); /* │ */
        int more = 0;
        for (int c = 0; c < nc; c++) {
            int next = 0, width = 0;
            int e = seg_end(&cells[c], off[c], w[c], &next, &width);
            int pad = w[c] - width;
            if (pad < 0)
                pad = 0;
            int lead = align[c] == AL_RIGHT    ? pad
                       : align[c] == AL_CENTER ? pad / 2
                                               : 0;
            put_cp(' ', 0);
            for (int k = 0; k < lead; k++)
                put_cp(' ', 0);
            put_seg(&cells[c], off[c], e, extra);
            for (int k = 0; k < pad - lead; k++)
                put_cp(' ', 0);
            put_cp(' ', 0);
            put_cp(0x2502, MD_TBORDER);
            off[c] = next;
            if (off[c] < cells[c].len)
                more = 1;
        }
        emit(ud, g_row, g_rowf, g_rn);
        if (!more)
            return;
    }
}

int table_render(const char *src, int srclen, int maxcols, table_row_fn emit,
                 void *ud)
{
    cell head[TABLE_MAX_COLS], body[TABLE_MAX_COLS], dl[TABLE_MAX_COLS];
    int align[TABLE_MAX_COLS], w[TABLE_MAX_COLS];

    int hlen = line_len(src, srclen);
    if (!has_pipe(src, hlen))
        return 0;
    int dstart = hlen + 1;
    if (dstart >= srclen)
        return 0; /* no delimiter line yet: not a table (or still streaming) */
    int dlen = line_len(src + dstart, srclen - dstart);
    int nc = split_row(src, hlen, head, TABLE_MAX_COLS);
    int nd = split_row(src + dstart, dlen, dl, TABLE_MAX_COLS);
    if (nc < 1 || nc > TABLE_MAX_COLS || nd != nc)
        return 0;
    for (int c = 0; c < nc; c++)
        if (!delim_align(dl[c].p, dl[c].len, &align[c]))
            return 0;

    /* the block runs to the first blank or pipe-less line */
    int dend = dstart + dlen;
    int consumed = dend < srclen ? dend + 1 : srclen;
    int first = consumed;
    for (int q = first; q < srclen;) {
        int l = line_len(src + q, srclen - q);
        if (l == 0 || !has_pipe(src + q, l))
            break;
        q += l;
        if (q < srclen)
            q++; /* the newline */
        consumed = q;
    }

    /* natural column widths, then squeezed to fit */
    for (int c = 0; c < nc; c++) {
        cell_style(&head[c], c);
        w[c] = cell_width(&head[c]);
    }
    for (int q = first; q < consumed;) {
        int l = line_len(src + q, srclen - q);
        int n = split_row(src + q, l, body, TABLE_MAX_COLS);
        if (n > nc)
            n = nc;
        for (int c = 0; c < n; c++) {
            cell_style(&body[c], c);
            int cw = cell_width(&body[c]);
            if (cw > w[c])
                w[c] = cw;
        }
        q += l + 1;
    }

    int overhead = 3 * nc + 1; /* "│ " per cell, plus the closing "│" */
    if (maxcols < overhead + nc)
        return 0; /* too narrow to draw: leave the source text alone */
    int avail = maxcols - overhead, sum = 0;
    for (int c = 0; c < nc; c++) {
        if (w[c] < 1)
            w[c] = 1;
        sum += w[c];
    }
    while (sum > avail) { /* shave the widest column until it fits */
        int m = 0;
        for (int c = 1; c < nc; c++)
            if (w[c] > w[m])
                m = c;
        if (w[m] <= 1)
            break;
        w[m]--;
        sum--;
    }

    emit_border(w, nc, 0x256D, 0x252C, 0x256E, emit, ud); /* ╭ ┬ ╮ */
    for (int c = 0; c < nc; c++)
        cell_style(&head[c], c);
    emit_row(head, nc, w, align, MD_BOLD | MD_HEADING, emit, ud);
    emit_border(w, nc, 0x251C, 0x253C, 0x2524, emit, ud); /* ├ ┼ ┤ */
    for (int q = first; q < consumed;) {
        int l = line_len(src + q, srclen - q);
        int n = split_row(src + q, l, body, TABLE_MAX_COLS);
        if (n > nc)
            n = nc;
        for (int c = 0; c < n; c++)
            cell_style(&body[c], c);
        for (int c = n; c < nc; c++) { /* short row: pad with empty cells */
            body[c].p = src + q;
            body[c].len = 0;
            body[c].f = NULL;
            body[c].flen = 0;
        }
        emit_row(body, nc, w, align, 0, emit, ud);
        q += l + 1;
    }
    emit_border(w, nc, 0x2570, 0x2534, 0x256F, emit, ud); /* ╰ ┴ ╯ */
    return consumed;
}
