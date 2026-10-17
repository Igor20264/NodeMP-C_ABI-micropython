#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "py/gc.h"
#include "py/mpstate.h"
#include "shared/runtime/gchelper.h"
#include "node_int.h"

static char g_line[512];
static size_t g_line_len;

static void flush_line(void) {
    if (g_line_len == 0) return;
    g_line[g_line_len] = '\0';
    nodemp_log(g_line);
    g_line_len = 0;
}

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    size_t i;
    if (!str) return;
    for (i = 0; i < len; i++) {
        char ch = str[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            flush_line();
            continue;
        }
        if (g_line_len + 1 >= sizeof g_line) flush_line();
        g_line[g_line_len++] = ch;
    }
}

#if MICROPY_ENABLE_GC
void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}
#endif

void nlr_jump_fail(void *val) {
    (void)val;
    nodemp_log_error("micropython exception escaped every handler");
    for (;;) {
#if defined(_WIN32)
        /* Keep the process up; the server is not ours to abort. */
        Sleep(1000);
#else
        sleep(1);
#endif
    }
}
