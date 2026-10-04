/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * Video Config dialog (IDD_VIDEOCONFIG in ce_res.rc) - see ce_video.h.
 *
 * Scale mode is a pure CE-frontend concern (ce_gapi.c's blit reads
 * CeVideoGetScaleMode() directly, no core involvement). Color correction,
 * frame skip, and the dynamic recompiler toggle are core features already
 * exposed by gpSP's libretro.c check_variables() through the standard
 * libretro core-options environment calls (gpsp_color_correction,
 * gpsp_frameskip, gpsp_drc - see that function and
 * libretro_core_options.h) - CE answers those environment queries from
 * ce_main.c's ce_environment() using the accessors below instead of
 * poking core globals directly, keeping this port a libretro *frontend*
 * (core is not patched - gpsp_drc's one dynarec platform tweak, the
 * icache flush, is the single deliberate exception in ../cpu_threaded.c,
 * not here; the translation caches are plain .bss static arrays). Frame
 * skip's
 * *cap* on consecutive skips (CeVideoFrameSkipShouldForceRender/
 * NotifyRendered) is CE-side, not a core option - see ce_video.h for why.
 */
#include "ce_video.h"
#include "ce_log.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_fileopen.h"
#include "ce_resource.h"

#include <string.h>

static CeScaleMode s_scaleMode  = CE_SCALE_X2; /* matches pre-Video-Config behaviour (always stretch-to-fill) */
/* "Color Correction" - maps to the core's gpsp_color_correction core
 * option (adjusts output colors to match real GBA hardware's LCD, off by
 * default matching the core's own built-in default). Reuses the same
 * dialog slot the sister PopSNES / PopSG ports used for their own
 * "Transparency Effects"/"No Sprite Limit" checkbox - see
 * CeVideoEnvGetVariable below. gpSP's other simple on/off option,
 * gpsp_frame_mixing (interframe blending), is intentionally left
 * unanswered for this first bring-up (core falls back to its own
 * "disabled" default) rather than adding a second checkbox control. */
static int         s_colorCorrection = 0;
/* Frame Skip (ported unchanged from the sister PopSG port, which
 * widened the range to 30 to match that core's own FRAMESKIP_MAX): 0 =
 * off (never skips); 1..30 = the maximum number of consecutive frames the
 * core is allowed to skip while it's actually falling behind
 * (gpsp_frameskip "auto" - the core's own audio-buffer-underrun signal,
 * see init_frameskip()/RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK
 * in libretro.c), enforced against the core's own fixed FRAMESKIP_MAX
 * (libretro.c, 30 - confirmed identical to PicoDrive's) by
 * CeVideoFrameSkipShouldForceRender()/NotifyRendered() below - see
 * ce_video.h and ce_main.c's WinMain loop. At 30 this dial's own cap and
 * the core's internal one coincide, so ShouldForceRender() never actually
 * needs to intervene - the core's own FRAMESKIP_MAX does the job on its
 * own at that setting. gpsp_frameskip_threshold/_interval (used only by
 * the "auto_threshold"/"fixed_interval" modes) are intentionally left
 * unanswered - this dial only ever requests plain "auto".
 *
 * Default is 4 (not 0): rounds 34/35's investigation established that on
 * this device the emulator can't hit 59.73fps at frameskip 0 and that
 * frameskip is the only practical speed lever, so new installs ship with
 * it already on. Existing PopGBA.cfg files keep whatever VideoFrameSkip
 * they saved; a user who wants 0 can still dial it down in Video Config
 * and that choice persists. */
static int         s_frameSkip  = 4;
static int         s_frameSkipConsecutive = 0; /* frames skipped in a row since the last real render - see CeVideoFrameSkipShouldForceRender/NotifyRendered */

/* Dynamic recompiler (JIT) toggle - maps to the core's gpsp_drc core
 * option (libretro_core_options.h, #if defined(HAVE_DYNAREC) - this
 * build now defines it, see CE/Makefile and the dev notes' dynarec round).
 * Default ON (round 59, restoring round 43's intent): the three JP ROMs
 * that crashed/hung under the JIT with the old CE-veneer core - Sweet
 * Cookie Pie (ABGJ), Tales of Phantasia (AN8J) and SMT Devil Children
 * Honoo no Sho (BDHJ) - are all confirmed working on hardware after the
 * move to the then-current libretro/gpsp core (upstream commit 8d268a6):
 * its THUMB translation + R11 reg_base far-call ABI + init_bios_hooks()
 * VBlankIntrWait handling fixes the systematic dynarec bug that repeated
 * patching of the older core could not (see the dev notes
 * rounds 54-59). Interpreter fallback stays available: set
 * "VideoDynarec=0" in PopGBA.cfg by hand (no Video Config dialog checkbox
 * - that dialog has no free row without a resize, see ce_res.rc).
 * CeConfigGetInt below uses this initializer only as the missing-key
 * default, so changing it later only affects a PopGBA.cfg without a
 * VideoDynarec line. CeVideoSaveConfig() writes that line whenever Video
 * Config is closed (OK or Back), so a released version's cfg may already
 * carry one, and it keeps its saved value. The translation caches are
 * plain .bss static arrays;
 * platform_cache_sync() (cpu_threaded.c, CacheSync(CACHE_SYNC_ALL) shim)
 * is the only platform-specific bit. */
static int         s_dynarec = 1;

static int s_dirty = 0; /* consumed by CeVideoConsumeDirty() - see ce_video.h */

void CeVideoInit(void)
{
    int savedScaleMode;

    savedScaleMode  = CeConfigGetInt("VideoScaleMode", (int)s_scaleMode);
    switch (savedScaleMode)
    {
    case CE_SCALE_1TO1:
    case CE_SCALE_X2:
        s_scaleMode = (CeScaleMode)savedScaleMode;
        break;
    default:
        /* Unrecognised value - either a corrupt config file, or a
         * config saved by an older build that still had the
         * Fullscreen/Halfstretch modes (values 2/3, retired once Scale
         * became a plain x1/x2 toggle). */
        s_scaleMode = CE_SCALE_X2;
        break;
    }
    s_colorCorrection  = CeConfigGetInt("VideoColorCorrection", s_colorCorrection);
    s_frameSkip     = CeConfigGetInt("VideoFrameSkip", s_frameSkip);
    s_dynarec       = CeConfigGetInt("VideoDynarec", s_dynarec);

    /* "Enable Debug Log" checkbox (default off) - applied here so it's in
     * effect before most CeLog() calls. WinMain calls CeVideoInit() right
     * after CeConfigLoad(). See ce_log.h. */
    CeLogSetEnabled(CeConfigGetInt("DebugLogEnabled", 0));

    CeLog("CeVideoInit: loaded scaleMode=%d colorCorrection=%d frameSkip=%d dynarec=%d debugLog=%d from config file",
          (int)s_scaleMode, s_colorCorrection, s_frameSkip, s_dynarec, CeLogIsEnabled());
}

/* Registry-based persistence (samDesired/RegFlushKey lessons of round
 * 3/8 - see the dev notes) didn't survive an actual power-off on this
 * device (round 9 user report) - now goes through ce_config.c's plain
 * config file instead, same as ce_input.c/ce_audio.c. */
static void CeVideoSaveConfig(void)
{
    CeConfigSetInt("VideoScaleMode", (int)s_scaleMode);
    CeConfigSetInt("VideoColorCorrection", s_colorCorrection);
    CeConfigSetInt("VideoFrameSkip", s_frameSkip);
    CeConfigSetInt("VideoDynarec", s_dynarec);
    CeConfigSetInt("DebugLogEnabled", CeLogIsEnabled());
    CeConfigSave();

    CeLog("CeVideoSaveConfig: saved scaleMode=%d colorCorrection=%d frameSkip=%d dynarec=%d debugLog=%d",
          (int)s_scaleMode, s_colorCorrection, s_frameSkip, s_dynarec, CeLogIsEnabled());
}

CeScaleMode CeVideoGetScaleMode(void)
{
    return s_scaleMode;
}

int CeVideoIsDynarecEnabled(void)
{
    return s_dynarec;
}

/* gpSP's own libretro core-option keys (see libretro_core_options.h and
 * check_variables() in libretro.c), plus "gpsp_save_method", which the
 * current core no longer has (see below) - gpSP has no region option (GBA has
 * no PAL/NTSC region concept the way Genesis/SNES do), so unlike the
 * sister PopSG port there is no third "*_region" key to answer
 * here. */
int CeVideoEnvGetVariable(const char *key, const char **outValue)
{
    if (strcmp(key, "gpsp_color_correction") == 0)
    {
        *outValue = s_colorCorrection ? "enabled" : "disabled";
        return 1;
    }

    if (strcmp(key, "gpsp_frameskip") == 0)
    {
        /* Plain "auto": skip on the core's own raw audio-underrun signal
         * (see init_frameskip()/retro_run() in libretro.c) -
         * CeVideoFrameSkipShouldForceRender()/NotifyRendered() (see
         * ce_video.h and ce_main.c's WinMain loop) cap how many of those
         * skips can happen in a row at s_frameSkip, since the core's own
         * internal cap (FRAMESKIP_MAX, 30) is fixed and not adjustable
         * here. */
        *outValue = (s_frameSkip > 0) ? "auto" : "disabled";
        return 1;
    }

    if (strcmp(key, "gpsp_drc") == 0)
    {
        /* See s_dynarec's declaration above for why this is config-file-
         * only (no Video Config checkbox yet) and defaults on. */
        *outValue = s_dynarec ? "enabled" : "disabled";
        return 1;
    }

    if (strcmp(key, "gpsp_save_method") == 0)
    {
        /* The current core has no "gpsp_save_method" option (nor a
         * use_libretro_save_method switch) and never asks for this key,
         * so this answer changes nothing. retro_get_memory_data/
         * size(RETRO_MEMORY_SAVE_RAM) in libretro.c always return the
         * cart's SRAM/Flash/EEPROM buffer (gamepak_backup, a fixed
         * 128KiB) directly, which is what ce_main.c's CeLoadSram/
         * CeSaveSram already expect - same generic RETRO_MEMORY_SAVE_RAM
         * path every sister CE port uses. */
        *outValue = "libretro";
        return 1;
    }

    return 0;
}

int CeVideoConsumeDirty(void)
{
    int wasDirty = s_dirty;
    s_dirty = 0;
    return wasDirty;
}

int CeVideoFrameSkipShouldForceRender(void)
{
    return (s_frameSkip > 0 && s_frameSkipConsecutive >= s_frameSkip) ? 1 : 0;
}

void CeVideoFrameSkipNotifyRendered(int rendered)
{
    s_frameSkipConsecutive = rendered ? 0 : (s_frameSkipConsecutive + 1);
}

/* ------------------------------------------------------------------ */
/* Dialog                                                              */
/* ------------------------------------------------------------------ */

/* Display order for the Scale spinner (round 21, user request; narrowed
 * from 4 states to a plain x1/x2 toggle in a later round - GBA is a
 * fixed 240x160 source and this device's 480x320 display is an exact
 * 2x of that on both axes, so the intermediate Fullscreen/Halfstretch
 * aspect-ratio compromises those sister ports' variable-resolution
 * sources needed never actually did anything different from a plain x2
 * stretch here). Left/Right just flip between the two, same wraparound
 * left/right-only interaction as Sound Config's Bits/Quality pairs. */
static const CeScaleMode kScaleOrder[2] = { CE_SCALE_1TO1, CE_SCALE_X2 };
static const wchar_t *kScaleLabels[2] = { L"x1", L"x2" };
#define CE_SCALE_CHOICE_COUNT 2

static int ScaleModeToIndex(CeScaleMode m)
{
    int i;
    for (i = 0; i < CE_SCALE_CHOICE_COUNT; i++)
        if (kScaleOrder[i] == m)
            return i;
    return 0; /* fallback: x1 */
}

static void UpdateScaleLabel(HWND hDlg)
{
    SetWindowTextW(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE), kScaleLabels[ScaleModeToIndex(s_scaleMode)]);
}

static void StepScaleMode(HWND hDlg, int delta)
{
    int idx = (ScaleModeToIndex(s_scaleMode) + delta + CE_SCALE_CHOICE_COUNT) % CE_SCALE_CHOICE_COUNT;
    s_scaleMode = kScaleOrder[idx];
    UpdateScaleLabel(hDlg);
}

/* IDC_VC_JAPANESE's own value readout (round 24, replacing a fixed
 * "English" CHECKBOX caption - see ce_resource.h) - romaji instead of a
 * translated string, so unlike every other label this function touches
 * *nothing* CeLangIsJapanese() gates: it reads the same "GAIKOKU-
 * English" / "NIHON-Japanese" pair whichever language is actually on
 * screen, and needs no jptahoma.ttc glyphs to render correctly if the
 * reader is currently looking at the Japanese UI. Same "leave it
 * alone in ApplyVideoConfigLanguage" treatment as the Scale/Frame Skip
 * value readouts (kScaleLabels, "Off"/a number) - see this dialog's own
 * ApplyVideoConfigLanguage. */
static void UpdateLanguageLabel(HWND hDlg)
{
    /* Since the switch to the Shinonome bitmap font (ce_bmpfont.c) this
     * can show the real language names - IDC_VC_JAPANESE is BS_OWNERDRAW
     * and CeBmpFontDrawOwnerButton() reads this text back with
     * GetWindowTextW() and renders it, Japanese included. No
     * jptahoma.ttc / font-load-failure fallback to reason about any
     * more. */
    SetWindowTextW(GetDlgItem(hDlg, IDC_VC_JAPANESE),
                   CeLangIsJapanese() ? L"\x65e5\x672c\x8a9e" /* 日本語 */ : L"English");
}

static void UpdateFrameSkipLabel(HWND hDlg)
{
    wchar_t text[8];
    if (s_frameSkip <= 0)
        _snwprintf(text, 8, L"Off");
    else
        _snwprintf(text, 8, L"%d", s_frameSkip);
    SetWindowTextW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_LABEL), text);
}

/* Auto-repeat for the Frame Skip -/+ buttons (user request: stepping
 * from 0 to 30 one tap at a time is tiring). Standard WinCE PUSHBUTTON
 * controls don't auto-repeat on their own, so both buttons are
 * subclassed in WM_INITDIALOG to watch WM_LBUTTONDOWN/UP directly: press
 * starts a one-shot timer at FRAMESKIP_REPEAT_INITIAL_MS, its first fire
 * switches the same timer to the faster FRAMESKIP_REPEAT_INTERVAL_MS
 * cadence (so a quick tap doesn't also trigger a repeat), and each
 * subsequent fire steps s_frameSkip once. WM_LBUTTONUP/WM_CAPTURECHANGED
 * (capture is held by the button itself once pressed, so these always
 * reach it even if a finger drags off the button on this touch device)
 * kill the timer. The button's own BN_CLICKED (handled below, unchanged)
 * still fires once on release for a normal click - if a hold happened to
 * land exactly on a repeat tick just before release, that's at most one
 * extra step out of 0..30, not worth suppressing. Both buttons are
 * standard BUTTON-class controls, so they share one original window
 * proc address; it's re-fetched from IDC_VC_FRAMESKIP_UP each time the
 * dialog opens rather than cached process-wide. Neither button is
 * WS_TABSTOP (ce_res.rc) - touch-only, like Sound Config's own -/+
 * pairs, so VideoCtrlProc below never subclasses these two; the value
 * readout between them (IDC_VC_FRAMESKIP_LABEL) is the WS_TABSTOP
 * stand-in for physical-key adjustment, same split the earlier prototype uses for its
 * own -/+ pairs. */
#define FRAMESKIP_REPEAT_TIMER_ID     1
#define FRAMESKIP_REPEAT_INITIAL_MS   500
#define FRAMESKIP_REPEAT_INTERVAL_MS  120

static WNDPROC s_origFrameSkipBtnProc = NULL;
static int     s_frameSkipRepeatDir   = 0; /* +1 = up, -1 = down, 0 = not held */
static int     s_frameSkipRepeatFast  = 0; /* 0 until the initial-delay timer has fired once */

static LRESULT CALLBACK FrameSkipButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        s_frameSkipRepeatDir  = (GetDlgCtrlID(hwnd) == IDC_VC_FRAMESKIP_UP) ? 1 : -1;
        s_frameSkipRepeatFast = 0;
        SetTimer(hwnd, FRAMESKIP_REPEAT_TIMER_ID, FRAMESKIP_REPEAT_INITIAL_MS, NULL);
        break;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        KillTimer(hwnd, FRAMESKIP_REPEAT_TIMER_ID);
        s_frameSkipRepeatDir = 0;
        break;

    case WM_TIMER:
        if (wParam == FRAMESKIP_REPEAT_TIMER_ID && s_frameSkipRepeatDir != 0)
        {
            if (!s_frameSkipRepeatFast)
            {
                s_frameSkipRepeatFast = 1;
                SetTimer(hwnd, FRAMESKIP_REPEAT_TIMER_ID, FRAMESKIP_REPEAT_INTERVAL_MS, NULL);
            }

            if (s_frameSkipRepeatDir > 0)
            {
                if (s_frameSkip < 30)
                    s_frameSkip++;
            }
            else
            {
                if (s_frameSkip > 0)
                    s_frameSkip--;
            }
            UpdateFrameSkipLabel(GetParent(hwnd));
        }
        break;
    }

    return CallWindowProc(s_origFrameSkipBtnProc, hwnd, msg, wParam, lParam);
}

/* Cached once per fresh dialog instance (WM_INITDIALOG, before
 * ApplyVideoConfigLanguage() ever has a chance to overwrite a control's
 * font with the Japanese one) - unlike Input/Sound Config, this
 * dialog's own IDC_VC_JAPANESE spinner can flip languages again while
 * still open, so re-querying IDOK's *current* font at that point would
 * return the Japanese font instead of the original template font. */
static void  RefreshVideoConfigLanguage(HWND hDlg);
static void  ToggleLanguage(HWND hDlg);

/* Labels repainted by WM_PAINT via CeBmpFontPaintLabel() - "Scale:" is
 * never translated but still needs a real ID + hide-and-repaint cycle
 * like ce_input.c's A/B labels (see ce_resource.h). Every PUSHBUTTON/
 * CHECKBOX in this dialog is BS_OWNERDRAW and redraws itself from
 * WM_DRAWITEM instead. */
static const int kVideoLabelIds[] = { IDC_VC_LBL_SCALE, IDC_VC_LBL_FRAMESKIP };
#define CE_VIDEO_LABEL_COUNT (sizeof(kVideoLabelIds) / sizeof(kVideoLabelIds[0]))

/* Physical-key focus chain for this dialog: Scale -> Transparency ->
 * Frame Skip -> language spinner -> Open Last Folder -> OK, wrapping
 * back to Scale - same technique and the same reason as an earlier
 * prototype's MiscNeighbor/MiscCtrlProc (see that prototype's source):
 * this device's dialog manager doesn't reliably move focus with the
 * arrow keys between dissimilar control types, and always routes decide
 * (Enter) to the DEFPUSHBUTTON (OK) regardless of what's actually
 * focused unless a control claims WANTALLKEYS and handles it itself -
 * OK included, so it can participate in the wrap instead of being an
 * arrow-key dead end. */
#define WM_SETVIDEOFOCUS (WM_APP + 202)

static int VideoNeighborDown(int id)
{
    switch (id)
    {
    case IDC_VC_SCALE_VALUE:       return IDC_VC_COLORCORRECT;
    case IDC_VC_COLORCORRECT:      return IDC_VC_FRAMESKIP_LABEL;
    case IDC_VC_FRAMESKIP_LABEL:   return IDC_VC_JAPANESE;
    case IDC_VC_JAPANESE:          return IDC_VC_DEBUGLOG;
    case IDC_VC_DEBUGLOG:          return IDOK;
    case IDOK:                     return IDC_VC_SCALE_VALUE;
    }
    return id;
}

static int VideoNeighborUp(int id)
{
    switch (id)
    {
    case IDC_VC_SCALE_VALUE:       return IDOK;
    case IDC_VC_COLORCORRECT:      return IDC_VC_SCALE_VALUE;
    case IDC_VC_FRAMESKIP_LABEL:   return IDC_VC_COLORCORRECT;
    case IDC_VC_JAPANESE:          return IDC_VC_FRAMESKIP_LABEL;
    case IDC_VC_DEBUGLOG:          return IDC_VC_JAPANESE;
    case IDOK:                     return IDC_VC_DEBUGLOG;
    }
    return id;
}

static WNDPROC s_pVideoOrigProc = NULL;

/* Subclasses the Scale value readout, Transparency, the Frame Skip value
 * readout, the language spinner, Open Last Folder and OK - claims every
 * key unconditionally (DLGC_WANTARROWS | DLGC_WANTALLKEYS), same blanket
 * approach as the earlier prototype's MiscCtrlProc/SoundCtrlProc for the reasons given
 * in VideoNeighborDown's comment above. Unclaimed keys still fall
 * through to the native BUTTON control via CallWindowProc at the
 * bottom. */
static LRESULT CALLBACK VideoCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);

    if (message == WM_GETDLGCODE)
    {
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
    }
    else if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_UP:
            SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborUp(id)));
            return 0;

        case VK_DOWN:
            SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborDown(id)));
            return 0;

        case VK_LEFT:
        case VK_RIGHT:
            if (id == IDC_VC_SCALE_VALUE)
            {
                StepScaleMode(GetParent(hWnd), (wParam == VK_LEFT) ? -1 : 1);
            }
            else if (id == IDC_VC_FRAMESKIP_LABEL)
            {
                if (wParam == VK_LEFT)
                {
                    if (s_frameSkip > 0) s_frameSkip--;
                }
                else
                {
                    if (s_frameSkip < 30) s_frameSkip++;
                }
                UpdateFrameSkipLabel(GetParent(hWnd));
            }
            else if (id == IDC_VC_JAPANESE)
            {
                /* Two-state toggle - Left and Right both just flip it,
                 * same reasoning as ToggleLanguage's own comment. */
                ToggleLanguage(GetParent(hWnd));
            }
            return 0;

        case VK_RETURN:
            if (id == IDC_VC_SCALE_VALUE || id == IDC_VC_FRAMESKIP_LABEL || id == IDC_VC_JAPANESE)
            {
                /* Spinner, not a toggle - decide just moves on, same as
                 * Sound Config's own value spinners. */
                SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborDown(id)));
            }
            else if (id == IDC_VC_COLORCORRECT)
            {
                /* Toggle the real backing bool and repaint - BM_GETCHECK/
                 * BM_SETCHECK don't work as a state store once the
                 * checkbox is BS_OWNERDRAW (see CeBmpFontDrawOwnerCheckbox()'s
                 * header comment). */
                s_colorCorrection = !s_colorCorrection;
                InvalidateRect(hWnd, NULL, TRUE);
            }
            else if (id == IDC_VC_DEBUGLOG)
            {
                CeLogSetEnabled(!CeLogIsEnabled());
                InvalidateRect(hWnd, NULL, TRUE);
            }
            else if (id == IDOK)
            {
                /* IDOK is subclassed too now (for the Up/Down wrap), so
                 * its own decide press has to be forwarded explicitly
                 * instead of falling through with no effect. */
                SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            }
            return 0;

        case VK_ESCAPE:
            /* Claiming WANTALLKEYS above means this control, not the
             * dialog manager, now sees the physical Back key too -
             * without this it would silently do nothing while focus was
             * on one of these controls, instead of committing and
             * closing like OK does (see IDOK/IDCANCEL below). */
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pVideoOrigProc, hWnd, message, wParam, lParam);
}

/* One-shot at WM_INITDIALOG, and again right after IDC_VC_JAPANESE is
 * toggled so this still-open dialog reflects the change immediately
 * instead of only the next time it's reopened. The "Scale:" caption and
 * the four scale-mode radio captions (1:1/Expand/Full screen 1:1/Expand
 * half) are deliberately left untranslated even in Japanese mode - see
 * ce_resource.h's comment on IDC_VC_LBL_FRAMESKIP. */
static void ApplyVideoConfigLanguage(HWND hDlg)
{
    unsigned i;

    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_VC_COLORCORRECT,   L"\x8272\x8abf\x88dc\x6b63"); /* 色調補正 */
        SetDlgItemTextW(hDlg, IDC_VC_LBL_FRAMESKIP,  L"\x30d5\x30ec\x30fc\x30e0\x30b9\x30ad\x30c3\x30d7\x3a"); /* フレームスキップ: */
        SetDlgItemTextW(hDlg, IDC_VC_DEBUGLOG,       L"\x30c7\x30d0\x30c3\x30b0\x30ed\x30b0\x3092\x6709\x52b9\x306b\x3059\x308b"); /* デバッグログを有効にする */
        SetDlgItemTextW(hDlg, IDOK,     L"\x6c7a\x5b9a");                            /* 決定 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_VC_COLORCORRECT,   L"Color Correction");
        SetDlgItemTextW(hDlg, IDC_VC_LBL_FRAMESKIP,  L"Frame Skip:");
        SetDlgItemTextW(hDlg, IDC_VC_DEBUGLOG,       L"Enable Debug Log");
        SetDlgItemTextW(hDlg, IDOK,     L"OK");
    }

    /* IDC_VC_COLORCORRECT/IDC_VC_DEBUGLOG (CHECKBOX)
     * and IDOK (PUSHBUTTON) are BS_OWNERDRAW - their text set above is read
     * straight back out by WM_DRAWITEM via GetWindowTextW(), nothing to
     * hide. IDC_VC_JAPANESE is left out of both branches (UpdateLanguage
     * Label owns its text). The plain LTEXT captions are hidden here and
     * repainted by WM_PAINT via CeBmpFontPaintLabel(). */
    for (i = 0; i < CE_VIDEO_LABEL_COUNT; i++)
        ShowWindow(GetDlgItem(hDlg, kVideoLabelIds[i]), SW_HIDE);
}

/* Re-run every time the language spinner is toggled ("-"/"+" tap or
 * decide-key path, both below) - also refreshes IDC_VC_JAPANESE's own
 * value text via UpdateLanguageLabel. Since the Shinonome bitmap font
 * (ce_bmpfont.c) is baked into the binary there's no font-load failure
 * to reconcile any more - the spinner simply reflects CeLangIsJapanese(). */
static void RefreshVideoConfigLanguage(HWND hDlg)
{
    UpdateLanguageLabel(hDlg);
    ApplyVideoConfigLanguage(hDlg);
}

/* Shared by the WM_COMMAND MINUS/PLUS handlers and the physical-key
 * Left/Right path in VideoCtrlProc below - a two-state toggle has no
 * real "-" vs. "+" direction, so both simply flip it, same as Frame
 * Skip's Up/Down naturally clamping instead of wrapping at the ends of
 * its own range. */
static void ToggleLanguage(HWND hDlg)
{
    CeLangSetJapanese(!CeLangIsJapanese());
    RefreshVideoConfigLanguage(hDlg);
}

static INT_PTR CALLBACK VideoConfigDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        /* No CheckDlgButton() for the checkboxes - initial check state
         * comes straight from s_colorCorrection / CeLogIsEnabled() the first time WM_DRAWITEM paints them, same
         * as every later toggle (BS_OWNERDRAW - see
         * CeBmpFontDrawOwnerCheckbox()). */
        UpdateScaleLabel(hDlg);
        UpdateFrameSkipLabel(hDlg);
        RefreshVideoConfigLanguage(hDlg);

        s_frameSkipRepeatDir = 0;
        s_origFrameSkipBtnProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_UP), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_UP), GWLP_WNDPROC, (LONG_PTR)FrameSkipButtonSubclassProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_DOWN), GWLP_WNDPROC, (LONG_PTR)FrameSkipButtonSubclassProc);

        s_pVideoOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE),       GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_COLORCORRECT),      GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_LABEL),   GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_JAPANESE),          GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_DEBUGLOG),          GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK),                     GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);

        /* Belt-and-suspenders initial focus, same pattern as every other
         * dialog in this port (ce_fileopen.c's WM_SETLISTFOCUS is the
         * first/most-documented instance): a synchronous SetFocus() from
         * WM_INITDIALOG alone doesn't always stick on this device. */
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
        PostMessage(hDlg, WM_SETVIDEOFOCUS, 0, 0);
        return FALSE;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        if (dis->CtlID == IDC_VC_COLORCORRECT)
            CeBmpFontDrawOwnerCheckbox(dis, s_colorCorrection);
        else if (dis->CtlID == IDC_VC_DEBUGLOG)
            CeBmpFontDrawOwnerCheckbox(dis, CeLogIsEnabled());
        else
            CeBmpFontDrawOwnerButton(dis);
        return TRUE;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        unsigned i;
        for (i = 0; i < CE_VIDEO_LABEL_COUNT; i++)
            CeBmpFontPaintLabel(hdc, hDlg, kVideoLabelIds[i]);
        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
            PostMessage(hDlg, WM_SETVIDEOFOCUS, 0, 0);
        }
        break;

    case WM_SETVIDEOFOCUS:
        SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_VC_SCALE_MINUS:
            StepScaleMode(hDlg, -1);
            return TRUE;

        case IDC_VC_SCALE_PLUS:
            StepScaleMode(hDlg, 1);
            return TRUE;

        case IDC_VC_COLORCORRECT:
            /* Toggle the real backing bool and repaint - CheckDlgButton/
             * BM_SETCHECK don't work as a state store once the checkbox
             * is BS_OWNERDRAW (see CeBmpFontDrawOwnerCheckbox()'s header
             * comment); WM_DRAWITEM reads s_colorCorrection directly. */
            s_colorCorrection = !s_colorCorrection;
            InvalidateRect(GetDlgItem(hDlg, IDC_VC_COLORCORRECT), NULL, TRUE);
            return TRUE;

        case IDC_VC_LANG_MINUS:
        case IDC_VC_LANG_PLUS:
            /* Same control on both IDs - see ToggleLanguage's comment
             * on why a two-state spinner has no real "-" vs. "+"
             * direction. IDC_VC_JAPANESE itself (the value readout in
             * between) intentionally has no case here, same as
             * IDC_VC_SCALE_VALUE above it - tapping the readout does
             * nothing, only "-"/"+" (or Left/Right when it has focus,
             * see VideoCtrlProc) change it. */
            ToggleLanguage(hDlg);
            return TRUE;

        case IDC_VC_DEBUGLOG:
            /* Toggle ce_log.c's enable flag directly and repaint - same
             * BS_OWNERDRAW / direct-backing-data pattern as the two
             * checkboxes above. Persisted by CeVideoSaveConfig() on OK. */
            CeLogSetEnabled(!CeLogIsEnabled());
            InvalidateRect(GetDlgItem(hDlg, IDC_VC_DEBUGLOG), NULL, TRUE);
            return TRUE;

        case IDC_VC_FRAMESKIP_DOWN:
            if (s_frameSkip > 0)
                s_frameSkip--;
            UpdateFrameSkipLabel(hDlg);
            return TRUE;

        case IDC_VC_FRAMESKIP_UP:
            if (s_frameSkip < 30)
                s_frameSkip++;
            UpdateFrameSkipLabel(hDlg);
            return TRUE;

        case IDOK:
        case IDCANCEL:
            /* Physical Back (IDCANCEL) acts the same as touching OK
             * here - this device has no meaningful "discard changes"
             * gesture, only "go back", so both commit and close (same
             * philosophy as every settings dialog in an earlier
             * prototype). Scale mode/Open Last Folder are already applied
             * live (ce_gapi.c/ce_fileopen.c read them straight from
             * module state); this just persists everything to disk and
             * pokes the core to re-poll its two options. */
            CeVideoSaveConfig();
            s_dirty = 1;
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
    return FALSE;
}

void CeShowVideoConfigDialog(HWND owner)
{
    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE), MAKEINTRESOURCEW(IDD_VIDEOCONFIG),
               owner, VideoConfigDlgProc);
}
