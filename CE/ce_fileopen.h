/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Custom ROM picker (CE/ce_res.rc's IDD_FILEOPEN) - a directory-browsing
 * listbox dialog that replaces GetOpenFileNameW(). Ported from an
 * earlier prototype's own IDD_FILEOPEN/DLGFileOpen (CE.c there): the
 * standard common file-open dialog has no way to render Japanese folder/
 * file names on this device (its own font is glyph-less for them,
 * confirmed by the earlier prototype's dev notes, and unfixable via OFN_* flags), so
 * the earlier prototype replaced it outright with a self-drawn listbox using the bundled
 * Japanese font (see ce_lang.h) instead.
 */
#ifndef CE_FILEOPEN_H
#define CE_FILEOPEN_H

#include <windows.h>

/* Loads the "Open Last Folder" preference from the config file - call
 * once from WinMain, after CeConfigLoad(). */
void CeFileOpenInit(void);

/* Which set of files the picker lists (see IsRomExtension /
 * FileMatchesPickMode in ce_fileopen.c):
 *  - CE_FILEOPEN_ROM : .gba/.agb/.gbz (NOT .bin - a loose .bin next to
 *    your games is almost always gba_bios.bin; user request 2026-09-02).
 *  - CE_FILEOPEN_BIOS: gba_bios.bin only, for PromptAndSetupBios()'s
 *    one-shot BIOS pick (ce_main.c). */
enum {
    CE_FILEOPEN_ROM  = 0,
    CE_FILEOPEN_BIOS = 1
};

/* Shows the picker. mode is one of the CE_FILEOPEN_* values above.
 * Starts at the folder remembered from the previous successful pick
 * (falling back to "\Storage Card", then "\", if none/no longer exists)
 * - "Open Last Folder" is always on since round 64. On a
 * successful pick, fills outPath with the
 * chosen file's full path, remembers its folder for next time, and
 * returns 1; returns 0 if the user backed out at the root without
 * picking anything. */
int CeShowFileOpenDialog(HWND owner, wchar_t *outPath, size_t outPathCount, int mode);

/* "Open Last Folder" state - fixed at 1 since round 64 (the Video
 * Config checkbox was removed; see ce_fileopen.c). */
int  CeFileOpenGetRememberLast(void);

#endif
