#ifndef LAMBDA_TERM_H
#define LAMBDA_TERM_H

#include <stdint.h>

/* Minimal terminal layer: raw mode, alternate screen, a double-buffered
 * cell grid with diffed output, and escape-sequence key decoding.
 * Everything is statically sized — no allocation. */

/* colors: 0..255 are xterm-256 indices, TCOL_DEFAULT is the terminal's own */
#define TCOL_DEFAULT (-1)

/* attribute bits */
#define TATTR_BOLD 0x01
#define TATTR_DIM 0x02
#define TATTR_ITALIC 0x04
#define TATTR_REVERSE 0x08

/* key event types */
enum {
    K_NONE = 0, /* timeout, nothing available */
    K_CHAR,     /* ev.ch holds a unicode codepoint */
    K_ENTER,
    K_BACKSPACE,
    K_DELETE,
    K_LEFT,
    K_RIGHT,
    K_UP,
    K_DOWN,
    K_HOME,
    K_END,
    K_PGUP,
    K_PGDN,
    K_TAB,
    K_ESC,
    K_CTRL, /* ev.ch holds the letter, e.g. 'a' for ctrl-a */
    K_WHEEL_UP,
    K_WHEEL_DOWN,
    K_RESIZE,
    K_EOF,
};

typedef struct {
    int type;
    uint32_t ch;
} term_event;

int term_init(void); /* 0 on success */
void term_shutdown(void);
int term_active(void);

/* terminal display width of a codepoint: 0 (combining), 1, or 2 (wide) */
int term_char_width(uint32_t cp);

int term_width(void);
int term_height(void);

void term_clear(void);
void term_set(int x, int y, uint32_t cp, int fg, int bg, int attr);
/* returns the number of columns written */
int term_print(int x, int y, const char *s, int fg, int bg, int attr);
void term_present(void);

void term_show_cursor(int x, int y);
void term_hide_cursor(void);

/* wait up to timeout_ms (<0 = forever) for an event */
term_event term_poll(int timeout_ms);

#endif
