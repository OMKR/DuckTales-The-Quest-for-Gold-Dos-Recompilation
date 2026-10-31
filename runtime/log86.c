/* Log file (settings.log): one file per run, logs/ducktales_YYYYMMDD_HHMMSS.log
 * beside the program. Everything the runtime writes to stderr goes there
 * (fatal errors, the Amiga sound's errors, ...), plus the start, the scene
 * changes, the exit, and on a crash (an exception or signal) its code and
 * the recompiled game's state (crash_dump, core86.c). */
#include <SDL.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "log86.h"

void crash_dump(void);              /* core86.c */

static int log_on;
static uint32_t log_start;

int log_active(void) { return log_on; }

void log_printf(const char *fmt, ...)
{
    if (!log_on) return;
    uint32_t ms = SDL_GetTicks() - log_start;
    fprintf(stderr, "[%02u:%02u.%03u] ", ms / 60000, ms / 1000 % 60, ms % 1000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void crash(const char *what, unsigned long code, const void *addr)
{
    static int once;
    if (once++) return;
    log_printf("CRASH: %s (code %08lX, address %p)", what, code, addr);
    crash_dump();
    fflush(stderr);
}

static void on_signal(int sig)
{
    crash(sig == SIGSEGV ? "segmentation fault" : sig == SIGFPE ? "arithmetic error" :
          sig == SIGILL ? "illegal instruction" : sig == SIGABRT ? "abort" : "signal",
          (unsigned long)sig, NULL);
    signal(sig, SIG_DFL);
    raise(sig);
}

#ifdef _WIN32
static LONG WINAPI on_exception(EXCEPTION_POINTERS *e)
{
    crash("exception", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* heap corruption (C0000374) and a stack buffer overrun (C0000409) end the
 * program without reaching the unhandled-exception filter */
static LONG WINAPI on_fail_fast(EXCEPTION_POINTERS *e)
{
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c == 0xC0000374 || c == 0xC0000409) crash("fail fast", c, e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void log_at_exit(void)
{
    log_printf("exit");
    fflush(stderr);
}

void log_open(void)
{
    char dir[1200] = "logs";
    char *base = SDL_GetBasePath();
    if (base) { snprintf(dir, sizeof dir, "%slogs", base); SDL_free(base); }
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char path[1300];
    snprintf(path, sizeof path, "%s/ducktales_%04d%02d%02d_%02d%02d%02d.log", dir,
             t ? t->tm_year + 1900 : 0, t ? t->tm_mon + 1 : 0, t ? t->tm_mday : 0,
             t ? t->tm_hour : 0, t ? t->tm_min : 0, t ? t->tm_sec : 0);
    if (!freopen(path, "w", stderr)) return;
    setvbuf(stderr, NULL, _IONBF, 0);       /* every line reaches the file before a crash */
    log_on = 1;
    log_start = SDL_GetTicks();
    log_printf("DuckTales (DOS) log, started %04d-%02d-%02d %02d:%02d:%02d", t ? t->tm_year + 1900 : 0,
               t ? t->tm_mon + 1 : 0, t ? t->tm_mday : 0, t ? t->tm_hour : 0, t ? t->tm_min : 0,
               t ? t->tm_sec : 0);
    signal(SIGSEGV, on_signal);
    signal(SIGFPE, on_signal);
    signal(SIGILL, on_signal);
    signal(SIGABRT, on_signal);
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_exception);
    AddVectoredExceptionHandler(1, on_fail_fast);
#endif
    atexit(log_at_exit);
}
