/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "ce_log.h"
#include "ce_app_config.h"

/* Video Config's "Enable Debug Log" - off by default (see ce_log.h).
 * CeVideoInit() calls CeLogSetEnabled() with the persisted flag right
 * after CeConfigLoad(); until then s_enabled is 0, so the earliest
 * few lines are suppressed on a fresh config. */
static int s_enabled = 0;

void CeLogSetEnabled(int enabled)
{
    s_enabled = enabled ? 1 : 0;
}

int CeLogIsEnabled(void)
{
    return s_enabled;
}

void CeLog(const char *fmt, ...)
{
    wchar_t exePath[MAX_PATH];
    wchar_t logPath[MAX_PATH];
    wchar_t *slash;
    char msg[512];
    va_list ap;
    FILE *f;

    if (!s_enabled)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);

    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    slash = wcsrchr(exePath, L'\\');
    if (slash)
        *slash = L'\0';
    _snwprintf(logPath, MAX_PATH, L"%s\\" CE_APP_LOG_FILENAME, exePath);

    f = _wfopen(logPath, L"a");
    if (!f)
        return;
    /* TEMPORARY: millisecond timestamp prefix (round: "load a second ROM
     * while one is already running" hang investigation) - CeLog() never
     * recorded *when* a line was written, so a gap between two lines in
     * the log could be a multi-second hang or a sub-millisecond no-op
     * and there was no way to tell them apart. GetTickCount() wraps
     * every ~49.7 days, which doesn't matter for diffing two nearby
     * lines within one test run. Remove (or keep - harmless either way)
     * once the hang's root cause is confirmed. */
    fprintf(f, "[%lu] ", (unsigned long)GetTickCount());
    fputs(msg, f);
    fputc('\n', f);
    fclose(f);
}
