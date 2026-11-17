#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
volatile sig_atomic_t http_interrupted;
#include "../src/ui.c"

int main(void)
{
    g_tty = 1; /* keep notices in the in-memory transcript */
    char text[64], got[LAMBDA_LINE_MAX];
    for (int cycle = 0; cycle < 200; cycle++) {
        assert(!ui_take_queued(got, sizeof got));
        for (int i = 0; i < QUEUE_MAX; i++) {
            snprintf(text, sizeof text, "cycle %d item %d ✓", cycle, i);
            assert(queue_push(text));
        }
        assert(!queue_push("must retain draft"));
        assert(ui_queued_count() == QUEUE_MAX);
        assert(!ui_take_queued(got, 3)); /* never pop a truncated message */
        assert(ui_queued_count() == QUEUE_MAX);
        ui_pause_queued();
        assert(!ui_take_queued(got, sizeof got));
        assert(ui_queued_count() == QUEUE_MAX);
        g_qpaused = 0;
        for (int i = 0; i < QUEUE_MAX; i++) {
            snprintf(text, sizeof text, "cycle %d item %d ✓", cycle, i);
            assert(ui_take_queued(got, sizeof got));
            assert(!strcmp(got, text));
        }
    }
    puts("queue: 3,200 FIFO deliveries, full/paused/undersized reads lose nothing: ok");
    return 0;
}
