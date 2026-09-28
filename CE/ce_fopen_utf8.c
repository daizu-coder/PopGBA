/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * fopen() -> ce_fopen_utf8() redirect target - see compat/stdio.h for why
 * this exists (the CRT's narrow fopen() converts its char* path to
 * wchar_t* via CP_ACP internally, and this device's CP_ACP can't
 * represent Japanese at all; this wrapper decodes the CP_UTF8 this port
 * builds its narrow paths as instead and calls the real _wfopen()).
 *
 * mode is always a short plain-ASCII literal ("rb", "wb", ...) from
 * every call site in this codebase (core included), so CP_ACP is fine
 * for that half - it only matters for the path half, which can hold a
 * Japanese folder/file name.
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

FILE *ce_fopen_utf8(const char *utf8path, const char *mode)
{
    wchar_t wpath[MAX_PATH];
    wchar_t wmode[8];

    MultiByteToWideChar(CP_UTF8, 0, utf8path, -1, wpath, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, mode, -1, wmode, 8);

    return _wfopen(wpath, wmode);
}
