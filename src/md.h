#ifndef LAMBDA_MD_H
#define LAMBDA_MD_H

#include <stdint.h>
typedef uint16_t md_flags;

/* Very small inline-markdown styler. Given one source line before wrapping,
 * produces a parallel array of per-byte style flags and tells the renderer
 * which bytes to hide (the markers themselves).
 *
 * Supported: # headings, **bold**, *italic* / _italic_, `code`,
 * ``` fenced blocks, - / 1. / 1) list bullets, > quotes, [label](url) links.
 * Tables are laid out separately, in table.c, which reuses these flags. */

#define MD_BOLD 0x01
#define MD_ITALIC 0x02
#define MD_CODE 0x04
#define MD_HEADING 0x08
#define MD_HIDE 0x10   /* marker byte: don't draw */
#define MD_BULLET 0x20 /* list marker: draw in accent */
#define MD_QUOTE 0x40
#define MD_TBORDER 0x80 /* table box-drawing glyph (see table.h) */
#define MD_LINK 0x100
#define MD_LINK_SPACE 0x200 /* closing label bracket rendered as a space */

/* Block state carried across lines of one item (fenced code blocks). */
typedef struct {
    int in_fence;
} md_state;

/* Style `len` bytes at `p` into `flags` (must be >= len bytes).
 * `line_start` is 1 when p is the start of a source line (so leading
 * markers like "# " or "- " count). */
void md_style(md_state *st, const char *p, int len, md_flags *flags,
              int line_start);

/* Indentation of wrapped list text, or 0 for a non-list source line. */
int md_list_indent(const char *p, int len);

/* 1 if the line is a ``` fence marker */
int md_is_fence(const char *p, int len);

#endif
