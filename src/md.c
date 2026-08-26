#include "md.h"

#include <string.h>

static int is_space(char c) { return c == ' ' || c == '\t'; }

void md_style(md_state *st, const char *p, int len, unsigned char *flags,
              int line_start)
{
    memset(flags, 0, (size_t)len);
    int i = 0;

    /* ``` fence toggles a verbatim block */
    if (line_start) {
        int j = 0;
        while (j < len && is_space(p[j]))
            j++;
        if (len - j >= 3 && strncmp(p + j, "```", 3) == 0) {
            st->in_fence = !st->in_fence;
            for (int k = 0; k < len; k++)
                flags[k] = MD_HIDE; /* the fence line itself isn't shown */
            return;
        }
    }
    if (st->in_fence) {
        for (int k = 0; k < len; k++)
            flags[k] = MD_CODE;
        return;
    }

    unsigned char line_style = 0;
    if (line_start) {
        int j = 0;
        while (j < len && is_space(p[j]))
            j++;
        if (j < len && p[j] == '#') {
            int h = j;
            while (h < len && p[h] == '#')
                h++;
            if (h < len && is_space(p[h])) {
                for (int k = j; k <= h; k++)
                    flags[k] = MD_HIDE;
                line_style = MD_HEADING | MD_BOLD;
                i = h + 1;
            }
        } else if (j < len && p[j] == '>') {
            flags[j] = MD_HIDE;
            line_style = MD_QUOTE;
            i = j + 1;
            if (i < len && p[i] == ' ')
                flags[i++] = MD_HIDE;
        } else if (j + 1 < len && (p[j] == '-' || p[j] == '*' || p[j] == '+') &&
                   is_space(p[j + 1])) {
            /* render "- " as a real bullet: hide the dash, keep alignment */
            flags[j] = MD_BULLET;
            i = j + 1;
        } else {
            /* ordered list: "12. " */
            int d = j;
            while (d < len && p[d] >= '0' && p[d] <= '9')
                d++;
            if (d > j && d + 1 < len && p[d] == '.' && is_space(p[d + 1])) {
                for (int k = j; k <= d; k++)
                    flags[k] |= MD_BULLET;
                i = d + 1;
            }
        }
    }

    /* inline spans */
    int bold = 0, italic = 0, code = 0;
    for (; i < len; i++) {
        char c = p[i];
        if (c == '`') {
            flags[i] = MD_HIDE;
            code = !code;
            continue;
        }
        if (!code && c == '*' && i + 1 < len && p[i + 1] == '*') {
            flags[i] = MD_HIDE;
            flags[i + 1] = MD_HIDE;
            bold = !bold;
            i++;
            continue;
        }
        if (!code && (c == '*' || c == '_')) {
            /* only treat as emphasis when it hugs a word */
            int opens = (i + 1 < len && !is_space(p[i + 1]));
            int closes = (i > 0 && !is_space(p[i - 1]));
            if ((!italic && opens) || (italic && closes)) {
                flags[i] = MD_HIDE;
                italic = !italic;
                continue;
            }
        }
        unsigned char f = line_style;
        if (bold)
            f |= MD_BOLD;
        if (italic)
            f |= MD_ITALIC;
        if (code)
            f |= MD_CODE;
        flags[i] |= f;
    }

    /* apply line style to any bullet/leading region too */
    if (line_style)
        for (int k = 0; k < len; k++)
            if (!(flags[k] & MD_HIDE))
                flags[k] |= line_style;
}

/* 1 if the line is a ``` fence marker (used while indexing lines) */
int md_is_fence(const char *p, int len)
{
    int j = 0;
    while (j < len && is_space(p[j]))
        j++;
    return len - j >= 3 && strncmp(p + j, "```", 3) == 0;
}
