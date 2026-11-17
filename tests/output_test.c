#include <assert.h>
#include <signal.h>
volatile sig_atomic_t http_interrupted;
#include "../src/ui.c"

static char spoken[256];
static int ticks;
static void on_text(const char *text)
{
    assert(strlen(spoken) + strlen(text) < sizeof spoken);
    strcat(spoken, text);
}
static int on_tick(void) { ticks++; return 1; }

int main(void)
{
    g_tty = 1;
    ui_set_output_hooks(on_text, on_tick, NULL);
    ui_add(UI_ASSISTANT, "restored history");
    ui_add(UI_TOOL_OUT, "private tool output");
    ui_stream_begin(UI_THINKING);
    ui_stream_delta("private reasoning");
    ui_stream_end();
    ui_stream_begin(UI_ASSISTANT);
    ui_stream_delta("live ");
    ui_stream_delta("reply");
    ui_stream_end();
    assert(!strcmp(spoken, "live reply"));
    ui_pump();
    assert(ticks == 1);
    ui_set_output_hooks(NULL, NULL, NULL);
    ui_stream_begin(UI_ASSISTANT);
    ui_stream_delta("disabled");
    ui_stream_end();
    assert(!strcmp(spoken, "live reply"));
    puts("output: only live assistant deltas reach speech; reasoning, tools and restored history excluded: ok");
    return 0;
}
