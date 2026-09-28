/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
#ifndef CE_LOG_H
#define CE_LOG_H

/* Crash-safe diagnostic logger: opens/appends/closes
 * "<exe-dir>\" CE_APP_LOG_FILENAME (see ce_app_config.h) on every call.
 * There is no debugger on this device, so this (plus Windows CE's own
 * BER crash log) is the only way to see what happened after a
 * real-hardware run. */
void CeLog(const char *fmt, ...);

/* Video Config's "Enable Debug Log" checkbox (user request, ported from
 * the sister PopGB / PopSNES ports) - lets this device's
 * occasional multi-hundred-ms flash-I/O latency on log file open be
 * avoided entirely when a log isn't being collected. Disabled by
 * default; the persisted flag ("DebugLogEnabled") is applied once at
 * startup from ce_video.c's CeVideoInit(). While disabled, CeLog() is a
 * no-op and no log file is created - so the handful of lines written
 * before CeVideoInit() runs (CeConfigLoad()'s summary etc.) are
 * suppressed too on a fresh config; enable the checkbox to capture
 * startup diagnostics. */
void CeLogSetEnabled(int enabled);
int  CeLogIsEnabled(void);

#endif
