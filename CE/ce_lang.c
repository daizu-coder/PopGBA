/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * Japanese/English UI toggle - persisted flag only. The bundled
 * jptahoma.ttc TrueType font and its AddFontResourceW() background load
 * were removed; UI text is now drawn by ce_bmpfont.c (Shinonome bitmap
 * font baked into the binary). See ce_lang.h for the full history.
 */
#include "ce_lang.h"
#include "ce_config.h"

static int g_japanese = 1;

void CeLangInit(void)
{
    /* Default Japanese (1) - only matters for a fresh config file (no
     * UILanguageJapanese key yet); anyone who has already picked a
     * language via Video Config keeps that persisted choice. */
    g_japanese = CeConfigGetInt("UILanguageJapanese", 1);
}

int CeLangIsJapanese(void)
{
    return g_japanese;
}

void CeLangSetJapanese(int japanese)
{
    g_japanese = japanese ? 1 : 0;
    CeConfigSetInt("UILanguageJapanese", g_japanese);
}
