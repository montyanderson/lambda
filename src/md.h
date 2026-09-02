#ifndef LAMBDA_MD_H
#define LAMBDA_MD_H

/* Very small inline-markdown styler. Given one already-wrapped visual line,
 * produces a parallel array of per-byte style flags and tells the renderer
 * which bytes to hide (the markers themselves).
 *
 * Supported: # headings, **bold**, *italic* / _italic_, `code`,
 * ``` fenced blocks, - / 1. list bullets, > quotes.
 * Tables are laid out separately, in table.c, which reuses these flags. */

#define MD_BOLD 0x01
#define MD_ITALIC 0x02
#define MD_CODE 0x04
#define MD_HEADING 0x08
#define MD_HIDE 0x10   /* marker byte: don't draw */
#define MD_BULLET 0x20 /* list marker: draw in accent */
#define MD_QUOTE 0x40
#define MD_TBORDER 0x80 /* table box-drawing glyph (see table.h) */

/* Block state carried across lines of one item (fenced code blocks). */
typedef struct {
    int in_fence;
} md_state;

/* Style `len` bytes at `p` into `flags` (must be >= len bytes).
 * `line_start` is 1 when p is the start of a source line (so leading
 * markers like "# " or "- " count). */
void md_style(md_state *st, const char *p, int len, unsigned char *flags,
              int line_start);

/* 1 if the line is a ``` fence marker */
int md_is_fence(const char *p, int len);

#endif
