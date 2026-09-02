/* Markdown table layout.
 *
 * table.c is the one part of the renderer that has to look past the line it
 * is styling, so it owns its own parser. The cases here are the ones that
 * cost real bugs elsewhere: deciding what is and is not a table, keeping
 * every emitted row exactly as wide as the last, and always making forward
 * progress through a cell (a wrap that stalls hangs the whole ui). */

#include <stdio.h>
#include <string.h>

#include "../src/table.h"
#include "../src/term.h"

static int failures;

static void fail(const char *what)
{
    printf("  FAIL %s\n", what);
    failures++;
}

/* what one run collected */
#define MAX_ROWS 256
static struct {
    int n;
    int cols[MAX_ROWS]; /* display width of each row */
    char first[512];
    int too_many;
} g;

static int row_cols(const char *t, int len)
{
    /* the same accounting the renderer does: markers are hidden, everything
     * else costs its terminal width */
    int i = 0, n = 0;
    while (i < len) {
        unsigned char c = (unsigned char)t[i];
        int adv = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (i + adv > len)
            adv = 1;
        unsigned cp = adv == 1 ? c : (unsigned)(c & (0xff >> (adv + 1)));
        for (int k = 1; k < adv; k++)
            cp = (cp << 6) | ((unsigned char)t[i + k] & 0x3fu);
        n += term_char_width(cp);
        i += adv;
    }
    return n;
}

static void collect(void *ud, const char *text, const unsigned char *flags,
                    int len)
{
    (void)ud;
    if (g.n == 0)
        snprintf(g.first, sizeof g.first, "%.*s", len, text);
    if (g.n >= MAX_ROWS) {
        g.too_many = 1;
        return;
    }
    /* hidden markers cost no columns */
    int cols = 0, i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        int adv = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (i + adv > len)
            adv = 1;
        if (!(flags[i] & 0x10)) /* MD_HIDE */
            cols += row_cols(text + i, adv);
        i += adv;
    }
    g.cols[g.n++] = cols;
}

static int run(const char *src, int maxcols)
{
    memset(&g, 0, sizeof g);
    return table_render(src, (int)strlen(src), maxcols, collect, NULL);
}

/* every row of a table must be the same width, and never wider than asked */
static void check_rect(const char *label, int maxcols)
{
    if (g.n < 4) {
        fail(label);
        printf("       only %d rows emitted\n", g.n);
        return;
    }
    for (int i = 1; i < g.n; i++) {
        if (g.cols[i] != g.cols[0]) {
            fail(label);
            printf("       row %d is %d cols, row 0 is %d\n", i, g.cols[i],
                   g.cols[0]);
            return;
        }
    }
    if (g.cols[0] > maxcols) {
        fail(label);
        printf("       %d cols wide, only %d available\n", g.cols[0], maxcols);
    }
}

int main(void)
{
    /* --- what counts as a table ------------------------------------- */
    struct {
        const char *src;
        const char *why;
    } no[] = {
        {"just prose\nmore prose\n", "no pipes at all"},
        {"a | b\nmore prose\n", "pipes but no delimiter row"},
        {"a | b\n-----\n", "a horizontal rule is not a delimiter row"},
        {"a | b | c\n--- | ---\n", "delimiter row column count differs"},
        {"| a | b |\n", "header with no line after it (still streaming)"},
        {"| a | b |\n| :: | -- |\n", "delimiter cell without a dash"},
    };
    for (unsigned i = 0; i < sizeof no / sizeof no[0]; i++) {
        if (run(no[i].src, 60) != 0) {
            fail("detected a table where there is none");
            printf("       %s\n", no[i].why);
        }
        if (g.n != 0) {
            fail("emitted rows for a non-table");
            printf("       %s\n", no[i].why);
        }
    }

    /* --- the ordinary case ------------------------------------------ */
    const char *t = "| model | ctx | price |\n"
                    "|---|:--:|------:|\n"
                    "| `claude-opus-5` | 1m | $5/$25 |\n"
                    "| claude-haiku-4-5 | 200k | $1/$5 |\n"
                    "after the table\n";
    int used = run(t, 60);
    if (used != (int)(strlen(t) - strlen("after the table\n")))
        fail("table block did not stop at the first non-table line");
    if (g.n != 6) /* top, header, rule, 2 rows, bottom */
        fail("wrong row count for a 2-row table");
    check_rect("plain table", 60);

    /* --- widths are squeezed to fit, at every width ------------------ */
    for (int w = 13; w <= 120; w++) {
        run(t, w);
        if (g.n == 0)
            continue; /* too narrow to draw: rendered as source instead */
        check_rect("squeezed table", w);
    }

    /* --- cells that have to wrap ------------------------------------- */
    const char *wrap =
        "| flag | what it does |\n"
        "| --- | --- |\n"
        "| --no-tools | disable the bash tool, so the model only talks |\n";
    run(wrap, 40);
    check_rect("wrapped cells", 40);
    if (g.n < 6)
        fail("a long cell did not wrap onto extra rows");

    /* A cell with no spaces still has to advance: if the wrap ever stalls
     * the ui hangs, so this is a liveness check as much as a layout one. */
    run("| a | b |\n| - | - |\n| xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx | y |\n", 20);
    check_rect("unbreakable cell", 20);
    if (g.too_many)
        fail("wrapping an unbreakable cell did not terminate");

    /* --- ragged input ------------------------------------------------ */
    run("| a | b | c |\n| - | - | - |\n| only one |\n| 1 | 2 | 3 |\n", 40);
    check_rect("short row padded out", 40);

    run("a | b\n--- | ---\n1 | 2\n", 40); /* outer pipes are optional */
    check_rect("no outer pipes", 40);

    run("| a | b |\n| - | - |\n| x \\| y | z |\n", 40); /* escaped pipe */
    check_rect("escaped pipe", 40);
    if (g.n != 5)
        fail("an escaped pipe split a cell");

    /* --- wide characters -------------------------------------------- */
    run("| col | x |\n| - | - |\n| 日本語テキスト | y |\n", 40);
    check_rect("cjk cells", 40);

    /* --- degenerate widths ------------------------------------------- */
    for (int w = 0; w < 13; w++)
        if (run(t, w) != 0)
            fail("laid out a table into a width that cannot hold it");

    printf("table_test: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
