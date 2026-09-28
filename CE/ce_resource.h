/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
#ifndef CE_RESOURCE_H
#define CE_RESOURCE_H

#define IDI_MAIN 100

/* Touch-to-reveal "menu" dialog (CE/ce_res.rc's IDD_MAINMENU): plain
 * DialogBoxW + PUSHBUTTON controls, not a real HMENU - this coredll
 * genuinely does not export SetMenu (checked with nm; CreateMenu/
 * AppendMenuW/DrawMenuBar exist but SetMenu/GetMenu don't - a real
 * limitation of this CE profile, not a mistake), and the Pocket-PC-style
 * alternative (SHCreateMenuBar) is both a different visual paradigm
 * (bottom command bar, not a desktop-style dropdown) and, given this
 * device's gx.dll/aygshell.dll are bundled as C++-mangled-export
 * binaries, an unverified risk. DialogBox is proven on this exact
 * hardware already (GetOpenFileNameW, the previous CE project's own
 * settings screens).
 *
 * Layout matches an earlier prototype's IDD_MAINMENU control-for-
 * control (same rects, same 210x140 dialog size): Open ROM full width
 * (WS_GROUP - the one thing that makes Up/Down *wrap* at the first/last
 * button on this device, plain sequential move already works without
 * it), Save/Load State as a 2-up row, the three config dialogs as a
 * 3-up row (in the same left-to-right slot the earlier prototype's Misc/Sound/Keys
 * occupy - Video Cfg is this project's "Misc" analogue, it's where the
 * language toggle lives, same reasoning as IDC_VC_JAPANESE below), Exit
 * full width, and a footer hint line instead of a separate Resume
 * button/About button - resuming is just the physical Back key
 * (IDCANCEL), matching the earlier prototype exactly. */
#define IDD_MAINMENU      3000
#define IDC_MM_OPEN       3002
#define IDC_MM_EXIT       3003
#define IDC_MM_INPUT      3004
#define IDC_MM_SOUND      3005
#define IDC_MM_VIDEO      3006
#define IDC_MM_SAVESTATE  3008
#define IDC_MM_LOADSTATE  3009
#define IDC_MM_HINT       3010
#define IDC_MM_SCREENSHOT 3012 /* 3011 is IDB_MAINMENU */

/* Main-menu decoration bitmap (ce_res.rc's IDB_MAINMENU, from
 * CE/icon/popgba_mascot.bmp - popgba_mascot_C_osanpo.png alpha-composited
 * onto the menu's cream background #F0E1BC and saved as a 260x98 24bpp
 * BMP, since this device's GDI has no transparent blit). Drawn by
 * MainMenuDlgProc's WM_PAINT to the right of the IDC_MM_HINT text. */
#define IDB_MAINMENU      3011

/* Input Config dialog (CE/ce_res.rc's IDD_INPUTCONFIG): one PUSHBUTTON
 * per SNES joypad button, showing the currently-bound key - click it,
 * then press the physical key to bind (see ce_input.c). Native controls
 * again, same reasoning as IDD_MAINMENU above - a plain grid of
 * PUSHBUTTONs, so (like IDD_MAINMENU) it just needs WS_GROUP on the
 * first one for Up/Down wraparound, no per-control subclassing. No
 * Cancel button (see IDD_SOUNDCONFIG's comment on why) - only OK, and
 * the physical Back key (IDCANCEL) does the same thing OK does. No
 * Reset to Defaults button either (round 21, user request) - removed
 * along with its now-unused IDC_IC_RESET id. */
#define IDD_INPUTCONFIG   3100
#define IDC_IC_BTN_UP     3101
#define IDC_IC_BTN_DOWN   3102
#define IDC_IC_BTN_LEFT   3103
#define IDC_IC_BTN_RIGHT  3104
#define IDC_IC_BTN_SELECT 3105
#define IDC_IC_BTN_START  3106
#define IDC_IC_BTN_A      3107
#define IDC_IC_BTN_B      3108
/* 3109/3110 (IDC_IC_BTN_X/_Y) intentionally unused - GBA has no X/Y
 * buttons, unlike the sister Genesis/SNES CE ports. IDs left retired
 * rather than reused, matching this file's own precedent for
 * IDC_IC_RESET above. */
#define IDC_IC_BTN_L      3111
#define IDC_IC_BTN_R      3112

/* Diagonal D-pad combos (Up+Right/Right+Down/Down+Left/Left+Up) - one
 * physical key standing in for two D-pad directions at once, for users
 * who can't reliably hold two direction keys down together on this
 * device. Unbound by default (see ce_input.c's s_map[]). Ported from
 * an earlier prototype, which in turn ported the idea
 * from PopSG. IDs continue after IDC_IC_BTN_R rather than
 * reusing the retired 3109/3110 (IDC_IC_BTN_X/_Y), matching this file's
 * own precedent for IDC_IC_RESET above. */
#define IDC_IC_BTN_UPRIGHT   3113
#define IDC_IC_BTN_RIGHTDOWN 3114
#define IDC_IC_BTN_DOWNLEFT  3115
#define IDC_IC_BTN_LEFTUP    3116

/* Row captions on the left column (Up/Down/Left/Right/Select/Start) -
 * these were anonymous (-1) LTEXTs until the Japanese UI toggle needed
 * to address them individually via SetDlgItemTextW. */
#define IDC_IC_LBL_UP     3120
#define IDC_IC_LBL_DOWN   3121
#define IDC_IC_LBL_LEFT   3122
#define IDC_IC_LBL_RIGHT  3123
#define IDC_IC_LBL_SELECT 3124
#define IDC_IC_LBL_START  3125

/* A/B/L/R and the four diagonal-combo labels stay untranslated (same
 * "A"/"B"/"Up R"/... in both UI languages, same call the sister ports
 * made for their own single-letter button labels) but still need real
 * control IDs - not plain "-1" LTEXTs any more - because the Shinonome
 * bitmap-font repaint (ce_bmpfont.c's CeBmpFontPaintLabel(), called
 * from WM_PAINT) needs GetDlgItem() to find them. See ce_res.rc. */
#define IDC_IC_LBL_A         3126
#define IDC_IC_LBL_B         3127
#define IDC_IC_LBL_L         3128
#define IDC_IC_LBL_R         3129
#define IDC_IC_LBL_UPRIGHT   3130
#define IDC_IC_LBL_RIGHTDOWN 3131
#define IDC_IC_LBL_DOWNLEFT  3132
#define IDC_IC_LBL_LEFTUP    3133

/* Sound Config dialog (CE/ce_res.rc's IDD_SOUNDCONFIG) - see
 * ce_audio.c. Volume/Rate/Bits/Quality are all "-/value/+" spinners now
 * (ported from an earlier prototype's own Sound Settings dialog),
 * not a COMBOBOX + RADIOBUTTON pairs: the value lives on a WS_TABSTOP
 * PUSHBUTTON in the middle so it gets a native focus rectangle and
 * participates in the same physical-key Up/Down/Left/Right loop as
 * every other control here (see SoundConfigDlgProc's WM_GETDLGCODE
 * subclassing) - the "-"/"+" buttons on either side are touch-only
 * (no WS_TABSTOP), matching the earlier prototype's IDC_FRAMESKIP_MINUS/PLUS. No
 * Cancel button: this device has no meaningful "discard changes"
 * gesture, only "go back" - the physical Back key (IDCANCEL) commits
 * and closes exactly like OK does, same as every settings dialog in
 * the earlier prototype (see SoundConfigDlgProc's IDOK/IDCANCEL handling). */
#define IDD_SOUNDCONFIG      3200
#define IDC_SC_VOLUME_MINUS  3202
#define IDC_SC_VOLUME_VALUE  3203
#define IDC_SC_VOLUME_PLUS   3220
#define IDC_SC_RATE_MINUS    3221
#define IDC_SC_RATE_VALUE    3222
#define IDC_SC_RATE_PLUS     3223
#define IDC_SC_BITS_MINUS    3224
#define IDC_SC_BITS_VALUE    3225
#define IDC_SC_BITS_PLUS     3226
#define IDC_SC_QUALITY_MINUS 3227
#define IDC_SC_QUALITY_VALUE 3228
#define IDC_SC_QUALITY_PLUS  3229

/* Static captions - were anonymous (-1) LTEXTs, need real IDs for the
 * Japanese UI toggle to address them individually. */
#define IDC_SC_LBL_VOLUME    3210
#define IDC_SC_LBL_RATE      3211
#define IDC_SC_LBL_BITS      3212
#define IDC_SC_LBL_QUALITY   3213
#define IDC_SC_LBL_BUFFER    3214

/* Sound ring-buffer size "-/value/+" spinner (ported from the sister
 * PopGB / PopSNES ports' Sound Config, after audio-latency /
 * underrun reports). Steps ce_audio.c's runtime ring size through a
 * small set of choices; the value is held pending until OK/Back commits,
 * same as Rate/Bits, since the waveOut drain thread reads the size
 * live. */
#define IDC_SC_BUFFER_MINUS  3230
#define IDC_SC_BUFFER_VALUE  3231
#define IDC_SC_BUFFER_PLUS   3232

/* Video Config dialog (CE/ce_res.rc's IDD_VIDEOCONFIG) - see
 * ce_video.c. Scale mode (1:1 / Full Screen 1:1 / Expand half / Expand -
 * see CeScaleMode in ce_video.h) is consumed directly by ce_gapi.c's
 * blit; transparency + frame skip are forwarded to the core via the
 * standard libretro core-options environment calls
 * (snes9x2002_transparency / snes9x2002_frameskip*) rather than poking
 * Settings.* directly, keeping ce_video.c a frontend, not a core patch.
 * Scale is a "-/value/+" spinner (round 21, replacing a 2x2 radio-button
 * grid, user request) cycling through 4 states (displayed as
 * x1/x1.5/Wide/Full - see ce_video.c's kScaleOrder/kScaleLabels), same
 * "-/value/+" pattern as Sound Config's Volume/Rate/Bits/Quality and
 * this dialog's own Frame Skip row below - the value lives on a
 * WS_TABSTOP PUSHBUTTON so it gets a native focus rectangle and a place
 * in the physical-key Up/Down/Left/Right loop; "-"/"+" are touch-only
 * (no WS_TABSTOP). Frame Skip is the same pattern -
 * IDC_VC_FRAMESKIP_LABEL was an inert LTEXT before, now a WS_TABSTOP
 * PUSHBUTTON so it has a place in the physical-key focus loop. No
 * Cancel button - see IDD_SOUNDCONFIG's comment above. */
#define IDD_VIDEOCONFIG          3300
#define IDC_VC_SCALE_MINUS       3301
#define IDC_VC_SCALE_VALUE       3302
#define IDC_VC_SCALE_PLUS        3303
#define IDC_VC_COLORCORRECT      3305
#define IDC_VC_FRAMESKIP_LABEL   3306
#define IDC_VC_FRAMESKIP_DOWN    3307
#define IDC_VC_FRAMESKIP_UP      3308

/* Static "Frame Skip:" caption - was an anonymous (-1) LTEXT, distinct
 * from IDC_VC_FRAMESKIP_LABEL above (the live numeric readout). The
 * "Scale:" caption and the four scale-mode labels (x1/x1.5/Wide/Full)
 * are deliberately left untranslated even in Japanese mode - technical
 * mode names, same call the earlier prototype made for its own output-rate radios
 * (11KHz/22KHz/44KHz). */
#define IDC_VC_LBL_FRAMESKIP     3309

/* Japanese/English UI toggle (see ce_lang.h) - lives here rather than a
 * new dialog because this one already has exactly this UI pattern (a
 * row of display options), and is this project's closest analogue to
 * the earlier prototype's Misc dialog (which houses the same toggle plus Open Last
 * Folder below). Round 24 (user request) turned this from a CHECKBOX
 * captioned "English" into a third "-/value/+" spinner (same pattern
 * as Scale/Frame Skip above): the value readout (still IDC_VC_JAPANESE,
 * not renamed, for the same reason the earlier prototype kept its own control as
 * IDC_JAPAN even after relabelling it) now shows "GAIKOKU-English" or
 * "NIHON-Japanese" depending on which is actually on screen - romaji
 * rather than a translated caption, so it reads the same and needs no
 * jptahoma.ttc glyphs in *either* language (see ce_video.c's
 * UpdateLanguageLabel), unlike the old fixed "English" caption which
 * silently assumed the reader already knew English. IDC_VC_LANG_MINUS/
 * PLUS are the new "-"/"+" touch buttons flanking it; being a two-state
 * toggle, both simply flip it, same as Frame Skip's Up/Down at the 0/30
 * ends of its own range naturally clamping instead of wrapping. */
#define IDC_VC_JAPANESE          3310
#define IDC_VC_LANG_MINUS        3312
#define IDC_VC_LANG_PLUS         3313

/* 3311 was IDC_VC_OPENLASTFOLDER - checkbox removed in round 64 (the
 * picker always reopens the last folder now, see ce_fileopen.c). Keep
 * the number retired. */

/* "Scale:" caption - same reason as IDD_INPUTCONFIG's IDC_IC_LBL_A etc.
 * above: needs a real ID so the Shinonome bitmap-font repaint
 * (ce_bmpfont.c's CeBmpFontPaintLabel(), from WM_PAINT) can find it,
 * even though it's never translated. */
#define IDC_VC_LBL_SCALE         3314

/* "Enable Debug Log" checkbox (user request, ported from the sister
 * PopGB / PopSNES ports) - toggles ce_log.c's
 * CeLogSetEnabled(). Same BS_OWNERDRAW / direct-backing-data checkbox
 * handling as IDC_VC_COLORCORRECT (CeLogIsEnabled() is the state store,
 * not BM_GETCHECK - see ce_bmpfont.c). Persisted as "DebugLogEnabled". */
#define IDC_VC_DEBUGLOG          3315

/* Custom ROM picker (CE/ce_res.rc's IDD_FILEOPEN, CE/ce_fileopen.c) -
 * replaces GetOpenFileNameW(), which has no way to show Japanese folder/
 * file names (see ce_fileopen.c). Ported from an earlier prototype's
 * own IDD_FILEOPEN/DLGFileOpen. */
#define IDD_FILEOPEN      3400
#define IDC_FO_PATH       3401
#define IDC_FO_LIST       3402

/* Generic message box (CE/ce_res.rc's IDD_MSGBOX, ce_main.c's
 * CeShowMsgBox()/MsgBoxDlgProc) - draws its text with the Shinonome
 * bitmap font (ce_bmpfont.c) instead of relying on MessageBoxW()'s
 * system font, which has no Japanese glyphs on this device any more
 * (jptahoma.ttc removed - see ce_lang.h). Used for the Save/Load State
 * result messages only; the plain-ASCII MessageBoxW() calls left in
 * this port (ROM load failure, RegisterClassW/CreateWindowW startup
 * failures) don't need it. */
#define IDD_MSGBOX        3500
#define IDC_MB_TEXT       3501

/* Yes/No confirmation dialog (CE/ce_res.rc's IDD_CONFIRM, ce_main.c's
 * CeConfirm()) - user request: Save State asks "Save?" first. Same
 * Shinonome-bitmap-font self-drawn design as IDD_MSGBOX. Custom Yes/No
 * control IDs rather than IDYES/IDNO so nothing depends on those being
 * present in this toolchain's winuser.h. */
#define IDD_CONFIRM       3520
#define IDC_CF_TEXT       3521
#define IDC_CF_YES        3522
#define IDC_CF_NO         3523

#endif
