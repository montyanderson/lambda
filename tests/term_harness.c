/* Drives term.c's diff renderer with random content and dumps, to a file,
 * what term.c believes the screen holds. Its escape output goes to stdout,
 * so an independent emulator can be fed the same bytes and compared. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/term.c"

static unsigned long rs;
static unsigned rnd(unsigned n) { rs = rs * 6364136223846793005ULL + 1; return (unsigned)((rs >> 33) % n); }

static uint32_t rand_cp(void)
{
    switch (rnd(10)) {
    case 0: case 1: return 0x4E00 + rnd(200);        /* cjk (wide) */
    case 2: return 0x1F600 + rnd(60);                /* emoji (wide) */
    case 3: return 0xFF21 + rnd(20);                 /* fullwidth (wide) */
    case 4: return 0x2705;                           /* ✅ (wide) */
    case 5: return 0x276F;                           /* ❯ (narrow) */
    default: return 33 + rnd(90);                    /* ascii */
    }
}

int main(int argc, char **argv)
{
    int W = atoi(argv[1]), H = atoi(argv[2]), rounds = atoi(argv[3]);
    rs = (unsigned long)atoi(argv[4]);
    const char *dumpfile = argv[5];

    g_active = 1;
    g_w = W; g_h = H;
    term_clear();
    memset(g_front, 0, sizeof g_front);
    g_dirty_all = 1;

    for (int r = 0; r < rounds; r++) {
        /* mutate a random subset of rows, like a scroll or a new message */
        int nrows = 1 + rnd((unsigned)H);
        for (int k = 0; k < nrows; k++) {
            int y = (int)rnd((unsigned)H);
            for (int x = 0; x < W; x++)
                term_set(x, y, ' ', TCOL_DEFAULT, TCOL_DEFAULT, 0);
            int x = 0;
            while (x < W) {
                if (rnd(6) == 0) { x++; continue; }   /* leave a gap */
                uint32_t cp = rand_cp();
                int fg = rnd(3) ? TCOL_DEFAULT : (int)rnd(256);
                int bg = rnd(8) ? TCOL_DEFAULT : (int)rnd(256);
                int at = rnd(4) ? 0 : (int)rnd(8);
                term_set(x, y, cp, fg, bg, at);
                x += term_char_width(cp);
            }
        }
        term_present();
    }
    flush_out();

    FILE *f = fopen(dumpfile, "w");
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            cell *c = &g_back[y][x];
            if (c->ch == CELL_CONT) continue;         /* owned by a wide glyph */
            if (c->ch == ' ') continue;               /* blank */
            fprintf(f, "%d %d %u\n", y, x, c->ch);
        }
    }
    fclose(f);
    return 0;
}
