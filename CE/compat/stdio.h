/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for <stdio.h> on this cegcc toolchain - like compat/errno.h,
 * this exists purely to intercept one libc entry point without touching
 * the upstream gpSP core (common.h/gba_memory.c/cpu_threaded.c/etc) or any
 * frontend call site directly. A working toolchain <stdio.h> genuinely
 * exists further down the include chain, so this uses #include_next to
 * pull it in unchanged and only overrides fopen().
 *
 * gpSP's own file_open() macro (common.h, non-PSP branch) is a plain
 * `fopen(filename, mode)` - load_bios()/load_gamepak_raw()/
 * load_backup()/save_backup() (gba_memory.c) all go through it with a
 * narrow path this port builds from a wchar_t one (WidePathToNarrow()
 * below, called for both the BIOS system dir and the picked ROM path).
 * That narrow path used to go through GetShortPathNameW() (the FAT 8.3
 * short name, guaranteed pure ASCII) with a WideCharToMultiByte(CP_ACP,
 * ...) fallback, because a real-hardware log (2026-08-11) showed this
 * device's GetACP() reporting 1252 (Western/Latin-1) - a codepage with
 * no Japanese characters at all - and CP_ACP conversion of the identical
 * Japanese folder name behaving *non-deterministically* across launches
 * (sometimes literal '?' per character, sometimes other bytes that
 * happened to still resolve). Rather than hand the core a path that
 * might not round-trip, callers rejected non-ASCII paths outright
 * (IsAsciiPathW()/RejectNonAsciiPathAndExit(), ce_main.c).
 *
 * The actual cause is narrower than "can't reach a non-ASCII path" at
 * all: fopen() itself converts its char* argument to wchar_t* via
 * CP_ACP internally to call CreateFileW (the only file API this OS
 * has), and CP_ACP can't represent Japanese on this device no matter
 * how carefully the narrow string is built (GetShortPathNameW's 8.3
 * short name sidesteps this by staying pure ASCII, but is unexported on
 * this cegcc target - resolved dynamically via GetProcAddress - and
 * depends on 8.3 name generation being enabled on the filesystem, two
 * separate failure modes). CP_UTF8 has neither problem: it can always
 * represent Japanese regardless of GetACP(), and it's what the sister
 * PopGB and PopSG ports already fixed this exact
 * CP_ACP-vs-real-encoding mismatch with. Redirecting fopen() to a
 * wrapper (ce_fopen_utf8(), ce_fopen_utf8.c) that decodes CP_UTF8 to
 * wide with MultiByteToWideChar() and calls the real _wfopen() sidesteps
 * the CRT's lossy/non-deterministic CP_ACP conversion entirely - for
 * every fopen() call in the program, core included, since -Icompat
 * applies to the whole build (see this port's own Makefile) and this
 * header shadows the toolchain's unconditionally. ASCII paths (the
 * common case) are valid UTF-8 unchanged, so this is a strict
 * improvement with no behavior change for paths that already worked.
 * WidePathToNarrow() now builds CP_UTF8 directly (the GetShortPathNameW/
 * GetProcAddress machinery is gone), and IsAsciiPathW()/
 * RejectNonAsciiPathAndExit() and their call sites (ROM pick, BIOS pick,
 * BIOS dir resolution) are removed now that the cause they were guarding
 * against is actually fixed.
 */
#ifndef CE_COMPAT_STDIO_H
#define CE_COMPAT_STDIO_H

#include_next <stdio.h>

FILE *ce_fopen_utf8(const char *utf8path, const char *mode);
#define fopen ce_fopen_utf8

#endif
