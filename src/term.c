/* Minimal terminal layer — raw mode, alternate screen, diffed cell output,
 * escape-sequence key decoding. Replaces a full curses/termbox dependency
 * with the ~5% of it lambda actually uses. Static storage throughout. */

#include "term.h"

#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_COLS 512
#define MAX_ROWS 256
#define OUTBUF (1u << 20)

/* second cell of a double-width glyph: occupied, never drawn */
#define CELL_CONT 0xFFFFFFFFu

typedef struct {
    uint32_t ch;
    int16_t fg, bg;
    uint8_t attr;
} cell;

static cell g_back[MAX_ROWS][MAX_COLS];
static cell g_front[MAX_ROWS][MAX_COLS];
static int g_w, g_h;
static int g_active;
static int g_dirty_all = 1;
static struct termios g_orig;
static int g_cursor_x = -1, g_cursor_y = -1;

static char g_out[OUTBUF];
static size_t g_outlen;

static volatile sig_atomic_t g_resized;
static int g_dropped;

static void on_winch(int sig)
{
    (void)sig;
    g_resized = 1;
}

/* ---- output buffering --------------------------------------------------- */

/* Write everything, or record that we could not.
 *
 * Never discards: term_present has already recorded these cells in the front
 * buffer, so dropping bytes here desynchronises the buffer from the screen
 * permanently and leaves stale glyphs that no later diff will repaint. */
static void flush_out(void)
{
    size_t off = 0;
    while (off < g_outlen) {
        ssize_t n = write(STDOUT_FILENO, g_out + off, g_outlen - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* a non-blocking tty with a full kernel buffer — common on a
                 * saturated ssh link. wait for room rather than drop. */
                fd_set wf;
                FD_ZERO(&wf);
                FD_SET(STDOUT_FILENO, &wf);
                if (select(STDOUT_FILENO + 1, NULL, &wf, NULL, NULL) < 0 &&
                    errno != EINTR) {
                    g_dropped = 1;
                    break;
                }
                continue;
            }
            g_dropped = 1; /* unrecoverable; resync on the next present */
            break;
        }
        off += (size_t)n;
    }
    g_outlen = 0;
}

/* Buffer output, flushing early when full.
 *
 * A full repaint of a large terminal can emit more than the buffer holds, so
 * overflow must not mean "drop" — that used to force a resync, which emitted
 * an even larger repaint, which dropped again. Flushing mid-present is safe:
 * the escape sequences are self-contained and strictly ordered. */
static void out(const char *s, size_t n)
{
    if (g_outlen + n > sizeof g_out)
        flush_out();
    if (n > sizeof g_out) { /* single chunk larger than the whole buffer */
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(STDOUT_FILENO, s + off, n - off);
            if (w < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                g_dropped = 1;
                return;
            }
            off += (size_t)w;
        }
        return;
    }
    memcpy(g_out + g_outlen, s, n);
    g_outlen += n;
}

static void outs(const char *s) { out(s, strlen(s)); }

static void outf(const char *fmt, ...)
{
    va_list ap;
    char tmp[128];
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        out(tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

/* ---- size --------------------------------------------------------------- */
static void update_size(void)
{
    struct winsize ws;
    int w = 80, h = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
        ws.ws_row > 0) {
        w = ws.ws_col;
        h = ws.ws_row;
    }
    if (w > MAX_COLS)
        w = MAX_COLS;
    if (h > MAX_ROWS)
        h = MAX_ROWS;
    if (w != g_w || h != g_h) {
        g_w = w;
        g_h = h;
        g_dirty_all = 1;
    }
}

int term_width(void) { return g_w; }
int term_height(void) { return g_h; }
int term_active(void) { return g_active; }

/* ---- init / shutdown ---------------------------------------------------- */
int term_init(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return -1;
    if (tcgetattr(STDIN_FILENO, &g_orig) < 0)
        return -1;

    struct termios raw = g_orig;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) < 0)
        return -1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);

    g_active = 1;
    update_size();
    outs("\x1b[?1049h"); /* alternate screen */
    outs("\x1b[?25l");   /* hide cursor */
    outs("\x1b[?1000h\x1b[?1006h"); /* mouse: button events, SGR encoding */
    outs("\x1b[2J");
    flush_out();
    term_clear();
    memset(g_front, 0, sizeof g_front);
    g_dirty_all = 1;
    return 0;
}

void term_shutdown(void)
{
    if (!g_active)
        return;
    outs("\x1b[?1006l\x1b[?1000l");
    outs("\x1b[0m");
    outs("\x1b[?25h");
    outs("\x1b[?1049l"); /* leave alternate screen */
    flush_out();
    tcsetattr(STDIN_FILENO, TCSADRAIN, &g_orig);
    g_active = 0;
}

/* ---- drawing ------------------------------------------------------------ */
void term_clear(void)
{
    for (int y = 0; y < MAX_ROWS; y++)
        for (int x = 0; x < MAX_COLS; x++) {
            g_back[y][x].ch = ' ';
            g_back[y][x].fg = TCOL_DEFAULT;
            g_back[y][x].bg = TCOL_DEFAULT;
            g_back[y][x].attr = 0;
        }
}

void term_set(int x, int y, uint32_t cp, int fg, int bg, int attr)
{
    if (x < 0 || y < 0 || x >= g_w || y >= g_h)
        return;
    if (cp == 0)
        cp = ' ';
    int w = term_char_width(cp);
    if (w == 0)
        return; /* combining mark: nothing to place on its own cell */
    if (w == 2 && x + 1 >= g_w)
        cp = ' '; /* a wide glyph would run off the edge */
    cell *c = &g_back[y][x];
    c->ch = cp;
    c->fg = (int16_t)fg;
    c->bg = (int16_t)bg;
    c->attr = (uint8_t)attr;
    if (w == 2 && x + 1 < g_w) {
        /* the terminal's cursor lands two columns on, so claim the next cell
         * as a continuation and never draw into it */
        cell *n = &g_back[y][x + 1];
        n->ch = CELL_CONT;
        n->fg = (int16_t)fg;
        n->bg = (int16_t)bg;
        n->attr = (uint8_t)attr;
    }
}

/* Terminal display width. The cell grid has to agree with what the terminal
 * actually does with a codepoint, or every later column on the line is offset
 * and the diff leaves stale glyphs behind. */
int term_char_width(uint32_t cp)
{
    /* combining marks, variation selectors, zero-width spaces */
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) ||
        (cp >= 0x0591 && cp <= 0x05BD) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x00AD || cp == 0x2060)
        return 0;
    if (cp < 0x1100)
        return 1;
    /* east asian wide and fullwidth */
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    /* emoji planes */
    if ((cp >= 0x1F300 && cp <= 0x1F64F) ||
        (cp >= 0x1F680 && cp <= 0x1F6FF) ||
        (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x1FA70 && cp <= 0x1FAFF) ||
        (cp >= 0x1F000 && cp <= 0x1F0FF))
        return 2;
    /* Symbols with emoji presentation. Deliberately enumerated rather than
     * taking the whole 2600..27BF block, which also holds narrow glyphs the
     * ui itself draws (❯ U+276F, ✻ U+273B). */
    if (cp == 0x231A || cp == 0x231B || (cp >= 0x23E9 && cp <= 0x23EC) ||
        cp == 0x23F0 || cp == 0x23F3 || cp == 0x25FD || cp == 0x25FE ||
        cp == 0x2614 || cp == 0x2615 || (cp >= 0x2648 && cp <= 0x2653) ||
        cp == 0x267F || cp == 0x2693 || cp == 0x26A1 || cp == 0x26AA ||
        cp == 0x26AB || cp == 0x26BD || cp == 0x26BE || cp == 0x26C4 ||
        cp == 0x26C5 || cp == 0x26CE || cp == 0x26D4 || cp == 0x26EA ||
        cp == 0x26F2 || cp == 0x26F3 || cp == 0x26F5 || cp == 0x26FA ||
        cp == 0x26FD || cp == 0x2705 || cp == 0x270A || cp == 0x270B ||
        cp == 0x2728 || cp == 0x274C || cp == 0x274E ||
        (cp >= 0x2753 && cp <= 0x2755) || cp == 0x2757 ||
        (cp >= 0x2795 && cp <= 0x2797) || cp == 0x27B0 || cp == 0x27BF ||
        cp == 0x2B1B || cp == 0x2B1C || cp == 0x2B50 || cp == 0x2B55)
        return 2;
    return 1;
}

static int utf8_decode(const char *s, uint32_t *cp)
{
    unsigned char c = (unsigned char)s[0];
    int n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    uint32_t v = n == 1 ? c : (uint32_t)(c & (0xff >> (n + 1)));
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80)
            return 1; /* malformed: consume one byte */
        v = (v << 6) | ((unsigned char)s[i] & 0x3f);
    }
    *cp = v;
    return n;
}

int term_print(int x, int y, const char *s, int fg, int bg, int attr)
{
    int cols = 0;
    for (const char *p = s; *p;) {
        uint32_t cp;
        int n = utf8_decode(p, &cp);
        p += n;
        if (cp == '\n')
            break;
        int w = term_char_width(cp);
        if (w == 0)
            continue;
        term_set(x + cols, y, cp, fg, bg, attr);
        cols += w;
        if (x + cols >= g_w)
            break;
    }
    return cols;
}

static void encode_utf8(uint32_t cp, char *b, int *n)
{
    if (cp < 0x80) {
        b[0] = (char)cp;
        *n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        *n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        *n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        *n = 4;
    }
}

static void emit_sgr(int fg, int bg, int attr)
{
    outs("\x1b[0");
    if (attr & TATTR_BOLD)
        outs(";1");
    if (attr & TATTR_DIM)
        outs(";2");
    if (attr & TATTR_ITALIC)
        outs(";3");
    if (attr & TATTR_REVERSE)
        outs(";7");
    if (fg >= 0)
        outf(";38;5;%d", fg);
    if (bg >= 0)
        outf(";48;5;%d", bg);
    outs("m");
}

void term_present(void)
{
    if (!g_active)
        return;
    if (g_resized) {
        g_resized = 0;
        update_size();
    }
    if (g_dirty_all || g_dropped) {
        g_dropped = 0;
        outs("\x1b[0m\x1b[2J");
        memset(g_front, 0xff, sizeof g_front); /* force full repaint */
    }
    outs("\x1b[?25l");

    int cur_fg = -2, cur_bg = -2, cur_attr = -1;
    for (int y = 0; y < g_h; y++) {
        int x = 0;
        while (x < g_w) {
            cell *b = &g_back[y][x], *f = &g_front[y][x];
            if (b->ch == f->ch && b->fg == f->fg && b->bg == f->bg &&
                b->attr == f->attr) {
                x++;
                continue;
            }
            outf("\x1b[%d;%dH", y + 1, x + 1);
            /* write the whole differing run */
            while (x < g_w) {
                b = &g_back[y][x];
                f = &g_front[y][x];
                if (b->ch == f->ch && b->fg == f->fg && b->bg == f->bg &&
                    b->attr == f->attr)
                    break;
                if (b->ch == CELL_CONT) {
                    /* Owned by a wide glyph that is no longer there. Emit a
                     * space so the stale second half is cleared. */
                    if (cur_fg != -2 || cur_bg != -2 || cur_attr != 0) {
                        emit_sgr(TCOL_DEFAULT, TCOL_DEFAULT, 0);
                        cur_fg = cur_bg = -1;
                        cur_attr = 0;
                    }
                    out(" ", 1);
                    *f = *b;
                    x++;
                    continue;
                }
                if (b->fg != cur_fg || b->bg != cur_bg ||
                    b->attr != cur_attr) {
                    emit_sgr(b->fg, b->bg, b->attr);
                    cur_fg = b->fg;
                    cur_bg = b->bg;
                    cur_attr = b->attr;
                }
                char u[4];
                int n;
                encode_utf8(b->ch, u, &n);
                out(u, (size_t)n);
                *f = *b;
                x++;
                /* a wide glyph moved the cursor two columns: take the
                 * continuation cell with it so we stay aligned */
                if (term_char_width(b->ch) == 2 && x < g_w) {
                    g_front[y][x] = g_back[y][x];
                    x++;
                }
            }
        }
    }
    outs("\x1b[0m");
    if (g_cursor_x >= 0) {
        outf("\x1b[%d;%dH", g_cursor_y + 1, g_cursor_x + 1);
        outs("\x1b[?25h");
    }
    g_dirty_all = 0;
    flush_out();
}

void term_show_cursor(int x, int y)
{
    g_cursor_x = x;
    g_cursor_y = y;
}

void term_hide_cursor(void)
{
    g_cursor_x = g_cursor_y = -1;
}

/* ---- input -------------------------------------------------------------- */

static char g_in[256];
static size_t g_inlen;

/* read more bytes; returns 0 on timeout, -1 on EOF/error, 1 on data */
static int fill(int timeout_ms)
{
    fd_set rf;
    struct timeval tv, *ptv = NULL;
    FD_ZERO(&rf);
    FD_SET(STDIN_FILENO, &rf);
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int r = select(STDIN_FILENO + 1, &rf, NULL, NULL, ptv);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
        return 0;
    if (g_inlen >= sizeof g_in)
        g_inlen = 0; /* runaway sequence: resync */
    ssize_t n = read(STDIN_FILENO, g_in + g_inlen, sizeof g_in - g_inlen);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    if (n == 0)
        return -1;
    g_inlen += (size_t)n;
    return 1;
}

static void consume(size_t n)
{
    if (n >= g_inlen) {
        g_inlen = 0;
        return;
    }
    memmove(g_in, g_in + n, g_inlen - n);
    g_inlen -= n;
}

static term_event ev_make(int type, uint32_t ch)
{
    term_event e;
    e.type = type;
    e.ch = ch;
    return e;
}

/* try to decode one event from the buffer; K_NONE if incomplete */
static term_event decode(void)
{
    if (g_inlen == 0)
        return ev_make(K_NONE, 0);
    unsigned char c = (unsigned char)g_in[0];

    if (c == 0x1b) {
        if (g_inlen == 1)
            return ev_make(K_NONE, 0); /* wait for the rest */
        if (g_in[1] == '[' || g_in[1] == 'O') {
            /* CSI / SS3 */
            size_t i = 2;
            /* SGR mouse: \x1b[<b;x;yM|m */
            if (g_inlen > 2 && g_in[2] == '<') {
                size_t j = 3;
                int b = 0;
                while (j < g_inlen && g_in[j] >= '0' && g_in[j] <= '9')
                    b = b * 10 + (g_in[j++] - '0');
                while (j < g_inlen && g_in[j] != 'M' && g_in[j] != 'm')
                    j++;
                if (j >= g_inlen)
                    return ev_make(K_NONE, 0);
                consume(j + 1);
                if (b == 64)
                    return ev_make(K_WHEEL_UP, 0);
                if (b == 65)
                    return ev_make(K_WHEEL_DOWN, 0);
                return ev_make(K_NONE, 1); /* other mouse: ignore */
            }
            while (i < g_inlen && g_in[i] >= '0' && g_in[i] <= '9')
                i++;
            while (i < g_inlen && g_in[i] == ';') {
                i++;
                while (i < g_inlen && g_in[i] >= '0' && g_in[i] <= '9')
                    i++;
            }
            if (i >= g_inlen)
                return ev_make(K_NONE, 0);
            char final = g_in[i];
            char first = g_in[2];
            consume(i + 1);
            switch (final) {
            case 'A': return ev_make(K_UP, 0);
            case 'B': return ev_make(K_DOWN, 0);
            case 'C': return ev_make(K_RIGHT, 0);
            case 'D': return ev_make(K_LEFT, 0);
            case 'H': return ev_make(K_HOME, 0);
            case 'F': return ev_make(K_END, 0);
            case '~':
                switch (first) {
                case '1': case '7': return ev_make(K_HOME, 0);
                case '4': case '8': return ev_make(K_END, 0);
                case '3': return ev_make(K_DELETE, 0);
                case '5': return ev_make(K_PGUP, 0);
                case '6': return ev_make(K_PGDN, 0);
                default: return ev_make(K_NONE, 1);
                }
            default:
                return ev_make(K_NONE, 1);
            }
        }
        consume(1);
        return ev_make(K_ESC, 0);
    }

    if (c == '\r' || c == '\n') {
        consume(1);
        return ev_make(K_ENTER, 0);
    }
    if (c == 0x7f || c == 0x08) {
        consume(1);
        return ev_make(K_BACKSPACE, 0);
    }
    if (c == '\t') {
        consume(1);
        return ev_make(K_TAB, 0);
    }
    if (c < 0x20) { /* ctrl-<letter> */
        consume(1);
        return ev_make(K_CTRL, (uint32_t)('a' + c - 1));
    }

    /* utf-8 character; wait if the sequence is incomplete */
    int need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    if ((size_t)need > g_inlen)
        return ev_make(K_NONE, 0);
    uint32_t cp;
    int n = utf8_decode(g_in, &cp);
    consume((size_t)n);
    return ev_make(K_CHAR, cp);
}

/* A lone 0x1b is ambiguous: it is either the escape key or the first byte of
 * a sequence still in flight. Waiting this long settles it — long enough for
 * the rest of a sequence to arrive even over ssh, short enough that the key
 * still feels immediate. */
#define ESC_WAIT_MS 30
static long g_esc_ms; /* when the pending lone escape showed up */

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

term_event term_poll(int timeout_ms)
{
    if (g_resized) {
        g_resized = 0;
        update_size();
        return ev_make(K_RESIZE, 0);
    }
    for (;;) {
        term_event e = decode();
        if (e.type != K_NONE)
            return e;
        if (e.ch == 1) /* decoded-and-ignored (e.g. mouse move): keep going */
            continue;

        int r;
        if (g_inlen == 1 && g_in[0] == 0x1b) {
            long now = mono_ms();
            if (!g_esc_ms)
                g_esc_ms = now;
            long left = ESC_WAIT_MS - (now - g_esc_ms);
            if (left <= 0) {
                g_esc_ms = 0;
                consume(1);
                return ev_make(K_ESC, 0);
            }
            /* never wait past what the caller asked for */
            r = fill(timeout_ms >= 0 && timeout_ms < left ? timeout_ms
                                                          : (int)left);
        } else {
            g_esc_ms = 0;
            r = fill(timeout_ms);
        }

        if (r < 0)
            return ev_make(K_EOF, 0);
        if (g_resized) {
            g_resized = 0;
            update_size();
            return ev_make(K_RESIZE, 0);
        }
        if (r == 0) {
            /* the escape settled: go round once more and emit it */
            if (g_esc_ms && mono_ms() - g_esc_ms >= ESC_WAIT_MS)
                continue;
            return ev_make(K_NONE, 0);
        }
    }
}
