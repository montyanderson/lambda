#ifndef LAMBDA_TABLE_H
#define LAMBDA_TABLE_H

/* GitHub-style markdown tables, pre-formatted into box-drawn rows.
 *
 * Tables cannot be styled a line at a time the way the rest of the markdown
 * is: column widths depend on every row. So a whole table block is laid out
 * up front and handed back one formatted row at a time, each with the
 * parallel md.h style flags the renderer already knows how to draw.
 * MD_TBORDER marks the box-drawing glyphs. */

#define TABLE_MAX_COLS 16

/* One formatted row. `text` and `flags` are the same length and are only
 * valid until the next call. */
typedef void (*table_row_fn)(void *ud, const char *text,
                             const unsigned char *flags, int len);

/* If `src` starts a table (a row of cells, then a |---|:--:| delimiter),
 * lay it out to fit `maxcols` columns, emit its rows through `emit`, and
 * return the number of source bytes consumed — that is, the offset of the
 * line after the table. Returns 0 if there is no table here, in which case
 * nothing is emitted. */
int table_render(const char *src, int srclen, int maxcols, table_row_fn emit,
                 void *ud);

#endif
