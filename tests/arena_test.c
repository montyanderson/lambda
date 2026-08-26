/* Transcript-arena invariants.
 *
 * ui.c's arena is static, so the test includes the translation unit directly
 * to reach it. Everything here runs without a terminal or the network.
 *
 * This exists because the arena only misbehaves once it is full, which in
 * normal use takes a very long session — the bugs it caught (a compaction
 * that discarded the whole transcript, and a length that underflowed to a
 * huge value when a compaction moved g_used backwards) were invisible until
 * then, and showed up as garbage on screen. */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ui.c references this; the real definition lives in http.c, which we do not
 * want to link here */
volatile sig_atomic_t http_interrupted = 0;

#include "../src/ui.c"

static int failures;

static void fail(const char *what, int step)
{
    printf("  FAIL [step %d] %s\n", step, what);
    failures++;
}

/* every invariant the renderer relies on */
static void check_invariants(int step)
{
    if (g_nitems < 0 || g_nitems > LAMBDA_MAX_ITEMS) {
        fail("item count out of range", step);
        return;
    }
    if (g_used > sizeof g_arena)
        fail("used exceeds arena", step);

    size_t total = 0, expect_off = 0;
    for (int i = 0; i < g_nitems; i++) {
        if (g_items[i].off != expect_off)
            fail("items are not contiguous from 0", step);
        if (g_items[i].len > sizeof g_arena)
            fail("item length larger than the whole arena", step);
        if (g_items[i].off + g_items[i].len > g_used)
            fail("item runs past the end of used space", step);
        expect_off = g_items[i].off + g_items[i].len;
        total += g_items[i].len;
    }
    if (total != g_used)
        fail("item lengths do not sum to used space", step);
}

int main(void)
{
    printf("arena: %u bytes, %d items max\n",
           (unsigned)sizeof g_arena, LAMBDA_MAX_ITEMS);

    /* 1. fill far past capacity with varied sizes */
    static char chunk[8192];
    for (int step = 0; step < 400; step++) {
        size_t n = (size_t)(37 + (step * 251) % 3000);
        memset(chunk, 'a' + (step % 26), n);
        chunk[n] = '\0';
        item_open(step % 5);
        item_extend(chunk, n);
        check_invariants(step);
        if (failures)
            break;
    }

    /* 2. streaming: many small appends to one item, across compactions */
    if (!failures) {
        item_open(UI_ASSISTANT);
        for (int step = 0; step < 2000; step++) {
            item_extend("token ", 6);
            check_invariants(1000 + step);
            if (failures)
                break;
        }
    }

    /* 3. the newest item's bytes must survive compaction intact */
    if (!failures) {
        const char *marker = "MARKER-CONTENT-MUST-SURVIVE";
        item_open(UI_USER);
        item_extend(marker, strlen(marker));
        check_invariants(9000);
        item *last = &g_items[g_nitems - 1];
        if (last->len != strlen(marker) ||
            memcmp(g_arena + last->off, marker, last->len) != 0)
            fail("newest item's content was corrupted", 9000);
    }

    /* 4. a single item larger than the arena must be truncated, not wrap */
    if (!failures) {
        static char huge[1 << 20];
        memset(huge, 'z', sizeof huge);
        item_open(UI_TOOL_OUT);
        item_extend(huge, sizeof huge);
        check_invariants(9100);
    }

    if (failures) {
        printf("arena_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("arena_test: ok\n");
    return 0;
}
