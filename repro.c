/* Minimal repro for the TTF-save crash.
 * Logs to a file (unbuffered) so nothing is lost on crash.
 */
#include <stdio.h>
#include <string.h>

#define LOG(f, ...) do { \
    fprintf(g_log, f "\n", __VA_ARGS__); fflush(g_log); \
} while (0)

static FILE *g_log;

int main(int argc, char **argv)
{
    const char *font = "C:/Windows/Fonts/simsun.ttc";
    g_log = fopen("repro.log", "w");
    if (!g_log) return 9;

    LOG("start, font=%s", font);

    /* dllself_check.c style dynamic load through the import lib would be
     * simpler, but the smoke exe links the import library; do the same. */
    {
        extern int dll_repro_run(const char *font);
        LOG("calling dll_repro_run");
        LOG("rc=%d", dll_repro_run(font));
    }
    LOG("done");
    fclose(g_log);
    return 0;
}
