/* Links and list wrapping must use the same visible widths as the renderer. */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

volatile sig_atomic_t http_interrupted;
#include "../src/ui.c"

static void visible(const char *p, int len, const md_flags *flags, char *out)
{
    int n = 0;
    for (int i = 0; i < len; i++)
        if (!(flags[i] & MD_HIDE))
            out[n++] = flags[i] & MD_LINK_SPACE ? ' ' : p[i];
    out[n] = '\0';
}

static void check(const char *source, const char *expected, int fence)
{
    md_flags flags[4096];
    char rendered[4096];
    md_state state = {fence};
    md_style(&state, source, (int)strlen(source), flags, 1);
    visible(source, (int)strlen(source), flags, rendered);
    if (strcmp(rendered, expected)) {
        fprintf(stderr, "expected %s\n     got %s\n", expected, rendered);
        assert(0);
    }
}

int main(void)
{
    check("- [**a link**](https://example.com/a_b?q=x_y)", "- a link (https://example.com/a_b?q=x_y)", 0);
    check("12) [one](https://e/a_(b)) and [two](https://e/c_d)", "12) one (https://e/a_(b)) and two (https://e/c_d)", 0);
    check("`[literal](https://e/a_b)`", "[literal](https://e/a_b)", 0);
    check("- [literal](https://e/a_b)", "- [literal](https://e/a_b)", 1);
    check("[unfinished](https://example.com", "[unfinished](https://example.com", 0);
    check("\\[literal\\]", "[literal]", 0);
    assert(md_list_indent("  - nested", 10) == 4);
    assert(md_list_indent("12) numbered", 12) == 4);
    assert(md_list_indent("normal", 6) == 0);

    const char *source = "- [**long label**](https://example.com/a_b_c_d_e_f) trailing words";
    for (int width = 8; width <= 60; width++) {
        g_nitems = 0;
        g_used = 0;
        g_gen++;
        item_open(UI_ASSISTANT);
        item_extend(source, strlen(source));
        build_vlines(width);
        char joined[4096] = "";
        for (int i = 0; i < g_nvl; i++) {
            vline *v = &g_vl[i];
            char text[4096];
            assert(v->flags);
            assert(v->indent == (i ? 2 : 0));
            visible(v->p, v->len, v->flags, text);
            assert((int)strlen(text) + v->indent <= width);
            /* Spaces at wrap points are intentionally discarded. */
            for (int j = 0; text[j]; j++)
                if (text[j] != ' ')
                    strncat(joined, text + j, 1);
            for (int j = 0; j < v->len; j++)
                if (v->p[j] == '_')
                    assert(!(v->flags[j] & MD_HIDE) && (v->flags[j] & MD_LINK));
        }
        assert(strcmp(joined, "-longlabel(https://example.com/a_b_c_d_e_f)trailingwords") == 0);
    }
    puts("markdown: links, URL punctuation, literal code, and hanging list wraps: ok");
    return 0;
}
