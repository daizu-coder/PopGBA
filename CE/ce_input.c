/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * Physical keyboard -> GBA joypad mapping (see ce_input.h) plus the
 * native Input Config remap dialog (IDD_INPUTCONFIG in ce_res.rc).
 *
 * Default key assignments follow the same base scheme the sister
 * Genesis/SNES CE ports use (arrow keys = D-pad, Enter = A, Backspace =
 * B, Tab = Select, 'M' = Start), minus the 'A'/'S' = X/Y entries those
 * ports had - GBA has no X/Y buttons, only A/B/L/R ('Q' = L, 'W' = R,
 * unchanged from the sister ports). Four extra rows (Up+Right/
 * Right+Down/Down+Left/Left+Up, unbound by default) let one physical
 * key stand in for two D-pad directions held at once - ported from an
 * earlier prototype (see its own ce_input.c header
 * comment), which in turn ported the idea from PopSG.
 *
 * Polling (GetAsyncKeyState), not WM_KEYDOWN, for two reasons: (1) it's
 * what retro_input_state_t needs anyway (called from inside retro_run,
 * not from the window's message loop), and (2) the Input Config dialog
 * below reuses the exact same primitive to detect "which key did the
 * user just press" without fighting the dialog manager's own keyboard
 * navigation (IsDialogMessage intercepts Tab/arrows/Enter for control
 * focus before they'd ever reach a WM_KEYDOWN handler).
 */
#include "ce_input.h"
#include "ce_log.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_resource.h"

#include <libretro.h>
#include <stdio.h>

/* This toolchain's winuser.h only declares VK_OEM_MINUS behind
 * `#if (_WIN32_WINNT >= 0x0500)`, and nothing in this build defines
 * _WIN32_WINNT - defining it just to pull that one constant in risks
 * changing the guarded declarations/behavior of everything else in
 * windows.h, so declare the numeric value directly instead (it's a
 * fixed, documented VK_* code, not toolchain-specific). */
#ifndef VK_OEM_MINUS
#define VK_OEM_MINUS 0xBD
#endif

typedef struct
{
    unsigned        id;         /* RETRO_DEVICE_ID_JOYPAD_* */
    int             ctrlId;     /* IDC_IC_BTN_* in the config dialog */
    const char     *cfgKey;     /* ce_config.c key name (also the dialog's row label) */
    int             defaultVk;
    int             vk;         /* current mapping - live, mutated by the config dialog */
    int             idB;        /* second RETRO_DEVICE_ID_JOYPAD_* this same key also
                                  * sets, or -1 - see the four diagonal rows below and
                                  * CeInputPoll()'s OR-in-both logic. Ported from an
                                  * earlier prototype (see that prototype's
                                  * ce_input.c). */
} CeInputMapEntry;

static CeInputMapEntry s_map[] = {
    { RETRO_DEVICE_ID_JOYPAD_UP,     IDC_IC_BTN_UP,     "InputUp",     VK_UP,    VK_UP,     -1 },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,   IDC_IC_BTN_DOWN,   "InputDown",   VK_DOWN,  VK_DOWN,   -1 },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,   IDC_IC_BTN_LEFT,   "InputLeft",   VK_LEFT,  VK_LEFT,   -1 },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT,  IDC_IC_BTN_RIGHT,  "InputRight",  VK_RIGHT, VK_RIGHT,  -1 },
    { RETRO_DEVICE_ID_JOYPAD_SELECT, IDC_IC_BTN_SELECT, "InputSelect", VK_TAB,   VK_TAB,    -1 },
    { RETRO_DEVICE_ID_JOYPAD_START,  IDC_IC_BTN_START,  "InputStart",  'M',      'M',       -1 },
    { RETRO_DEVICE_ID_JOYPAD_A,      IDC_IC_BTN_A,      "InputA",      VK_RETURN,VK_RETURN, -1 },
    { RETRO_DEVICE_ID_JOYPAD_B,      IDC_IC_BTN_B,      "InputB",      VK_BACK,  VK_BACK,   -1 },
    { RETRO_DEVICE_ID_JOYPAD_L,      IDC_IC_BTN_L,      "InputL",      'Q',      'Q',       -1 },
    { RETRO_DEVICE_ID_JOYPAD_R,      IDC_IC_BTN_R,      "InputR",      'W',      'W',       -1 },
    /* Diagonal combos - unbound by default (defaultVk/vk both 0, see
     * VkToLabel's "None" case below): this device has no spare physical
     * buttons to default these onto, so they're opt-in remaps only. */
    { RETRO_DEVICE_ID_JOYPAD_UP,     IDC_IC_BTN_UPRIGHT,   "InputUpRight",   0, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT,  IDC_IC_BTN_RIGHTDOWN, "InputRightDown", 0, 0, RETRO_DEVICE_ID_JOYPAD_DOWN },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,   IDC_IC_BTN_DOWNLEFT,  "InputDownLeft",  0, 0, RETRO_DEVICE_ID_JOYPAD_LEFT },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,   IDC_IC_BTN_LEFTUP,    "InputLeftUp",    0, 0, RETRO_DEVICE_ID_JOYPAD_UP },
};
#define CE_INPUT_COUNT (sizeof(s_map) / sizeof(s_map[0]))

/* 0/1 per RETRO_DEVICE_ID_JOYPAD_* id, refreshed once per CeInputPoll(). */
static int s_padDown[16];

/* ------------------------------------------------------------------ */
/* Poll hooks (retro_input_poll_t / retro_input_state_t)              */
/* ------------------------------------------------------------------ */

void CeInputInit(void)
{
    unsigned i;

    for (i = 0; i < CE_INPUT_COUNT; i++)
        s_map[i].vk = CeConfigGetInt(s_map[i].cfgKey, s_map[i].defaultVk);

    CeLog("CeInputInit: loaded key mapping from config file");
}

void CeInputPoll(void)
{
    unsigned i;

    for (i = 0; i < 16; i++)
        s_padDown[i] = 0;

    /* vk==0 rows are unbound diagonal combos (see s_map[] above) -
     * GetAsyncKeyState(0) isn't a real key, so skip them instead of
     * querying it. A bound row sets both id and (if present) idB, so a
     * single physical key can drive two D-pad directions at once - same
     * OR-in-both logic as an earlier prototype. */
    for (i = 0; i < CE_INPUT_COUNT; i++)
    {
        if (s_map[i].vk == 0)
            continue;
        if (GetAsyncKeyState(s_map[i].vk) & 0x8000)
        {
            s_padDown[s_map[i].id] = 1;
            if (s_map[i].idB >= 0)
                s_padDown[s_map[i].idB] = 1;
        }
    }
}

int16_t CeInputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;

    if (port != 0 || device != RETRO_DEVICE_JOYPAD || id >= 16)
        return 0;

    return s_padDown[id] ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Input Config dialog (remap-by-press)                                */
/* ------------------------------------------------------------------ */

typedef struct { int vk; const wchar_t *label; } VkName;

/* Friendly labels for the well-known keys - anything not listed here
 * still works for remapping (see the full-range VK scan below), it just
 * displays as "VK_xx" in the dialog until a friendly name is added. */
static const VkName kVkNames[] = {
    { VK_UP,     L"Up" },    { VK_DOWN,   L"Down" },
    { VK_LEFT,   L"Left" },  { VK_RIGHT,  L"Right" },
    { VK_RETURN, L"Enter" }, { VK_BACK,   L"Backspace" },
    { VK_TAB,    L"Tab" },   { VK_SPACE,  L"Space" },
    { VK_ESCAPE, L"Esc" },   { VK_OEM_MINUS, L"-" },
    /* This device's own dedicated hardware buttons, identified by the
     * user pressing each one while a remap was pending and reading back
     * which VK_xx this scan landed on (2026-08-01): Voice = VK_DC,
     * Function = VK_14, Forward = VK_21, Previous = VK_22. Labelled in
     * English per the button's own function, not what these VK codes
     * conventionally mean on a PC keyboard (VK_14/21/22 are Caps Lock/
     * PageUp/PageDown there) - that PC meaning is irrelevant on this
     * hardware. (Tried the device's own printed Japanese names here
     * first - rendered fine with DEFAULT_GUI_FONT, but the user reported
     * the dialog got noticeably slower, 2026-08-01 round 9 - reverted to
     * English and dropped the font switch below along with it.) */
    { 0xDC, L"Voice" },      { 0x14, L"Function" },
    { 0x21, L"Forward" },    { 0x22, L"Previous" },
    { '0',L"0" },{ '1',L"1" },{ '2',L"2" },{ '3',L"3" },{ '4',L"4" },
    { '5',L"5" },{ '6',L"6" },{ '7',L"7" },{ '8',L"8" },{ '9',L"9" },
    { 'A',L"A" },{ 'B',L"B" },{ 'C',L"C" },{ 'D',L"D" },{ 'E',L"E" },
    { 'F',L"F" },{ 'G',L"G" },{ 'H',L"H" },{ 'I',L"I" },{ 'J',L"J" },
    { 'K',L"K" },{ 'L',L"L" },{ 'M',L"M" },{ 'N',L"N" },{ 'O',L"O" },
    { 'P',L"P" },{ 'Q',L"Q" },{ 'R',L"R" },{ 'S',L"S" },{ 'T',L"T" },
    { 'U',L"U" },{ 'V',L"V" },{ 'W',L"W" },{ 'X',L"X" },{ 'Y',L"Y" },
    { 'Z',L"Z" },
};
#define CE_VKNAME_COUNT (sizeof(kVkNames) / sizeof(kVkNames[0]))

static const wchar_t *VkToLabel(int vk)
{
    static wchar_t fallback[16];
    unsigned i;
    if (vk == 0)
        return L"None"; /* unbound diagonal combo row - see s_map[] */
    for (i = 0; i < CE_VKNAME_COUNT; i++)
        if (kVkNames[i].vk == vk)
            return kVkNames[i].label;
    _snwprintf(fallback, 16, L"VK_%02X", vk);
    return fallback;
}

/* Remap-press detection accepts any VK code (not just kVkNames), so
 * dedicated hardware buttons this port doesn't have a name for yet
 * still work without needing their VK code documented up front:
 * whatever code WM_KEYDOWN reports for that physical button just gets
 * picked up in InputBtnCtrlProc below and shown as "VK_xx" (see
 * VkToLabel's fallback) if it isn't one of the named keys above. */

static int CtrlIdToIndex(int ctrlId)
{
    unsigned i;
    for (i = 0; i < CE_INPUT_COUNT; i++)
        if (s_map[i].ctrlId == ctrlId)
            return (int)i;
    return -1;
}

/* -1 when no remap is pending, otherwise the s_map[] index waiting for
 * a new key - the same role the earlier prototype's KeySet dialog splits into
 * g_bSetButtonMode + g_iSetButton (see that prototype's source). */
static int s_waitingIndex = -1;

/* This dialog's remap capture used to snapshot GetAsyncKeyState() the
 * moment a wait began and poll it every 20ms via a WM_TIMER, filtering
 * out the very key that started the wait so it couldn't immediately
 * re-bind to itself. In practice that didn't hold up on real hardware:
 * this device's dedicated decide/OK button kept self-assigning Enter
 * almost every time regardless (user report, 2026-08-08) - the likely
 * reason being that this particular button doesn't behave like an
 * ordinary keyboard key at the GetAsyncKeyState level at all (round 9's
 * "wired to synthesize the same input as a stylus tap" theory), so a
 * snapshot taken through GetAsyncKeyState can't be trusted to reflect
 * its real down/up state no matter how carefully it's timed - and every
 * physical press naturally generates a WM_KEYUP moments after its
 * WM_KEYDOWN, which an unconditional WM_KEYUP capture (removed along
 * with the polling below) would just as readily mistake for "a new key
 * was pressed".
 *
 * Ported from an earlier prototype's KeySet remap dialog instead
 * (see its ButtonProc/DLGKeySet), which sidesteps all of this by
 * never touching GetAsyncKeyState for capture: it only acts on a raw
 * WM_KEYDOWN, and *only* WM_KEYDOWN - never WM_KEYUP - so the key-up
 * that follows whatever started the wait can't be mistaken for a
 * second, different keypress in the first place. BeginWaitForKey/
 * EndWaitForKey below are that same idea, just keeping this file's
 * existing s_waitingIndex in place of the earlier prototype's separate pair of
 * globals. */
static void BeginWaitForKey(HWND hDlg, int index)
{
    s_waitingIndex = index;
    SetWindowTextW(GetDlgItem(hDlg, s_map[index].ctrlId), L"Press key");
}

static void EndWaitForKey(HWND hDlg)
{
    (void)hDlg;
    s_waitingIndex = -1;
}

/* Abandons a pending remap without changing the binding, restoring the
 * button's caption to whatever it showed before BeginWaitForKey blanked
 * it to "Press key". Used when the button that's already waiting
 * gets tapped again - see InputConfigDlgProc's BN_CLICKED handling. */
static void CancelWaitForKey(HWND hDlg)
{
    if (s_waitingIndex < 0)
        return;
    SetWindowTextW(GetDlgItem(hDlg, s_map[s_waitingIndex].ctrlId), VkToLabel(s_map[s_waitingIndex].vk));
    EndWaitForKey(hDlg);
}

/* Assigns vk as index's new binding and ends the wait - see
 * InputBtnCtrlProc's WM_KEYDOWN below, the only caller now that capture
 * is WM_KEYDOWN-only (no more WM_KEYUP/WM_SYSKEY.../WM_TIMER paths to
 * keep in sync with). */
static void CaptureKeyAsBinding(HWND hDlg, int index, int vk)
{
    s_map[index].vk = vk;
    SetWindowTextW(GetDlgItem(hDlg, s_map[index].ctrlId), VkToLabel(vk));
    EndWaitForKey(hDlg);
}

/* Registry-based persistence (samDesired/RegFlushKey lessons of round
 * 3/8 - see the dev notes) turned out not to survive an actual power-off on
 * this device (round 9 user report), so this now goes through
 * ce_config.c's plain config file instead - see that module's header
 * comment for why a real file write is the more dependable mechanism
 * here. */
static void CeInputSaveConfig(void)
{
    unsigned i;

    for (i = 0; i < CE_INPUT_COUNT; i++)
        CeConfigSetInt(s_map[i].cfgKey, s_map[i].vk);

    CeConfigSave();
    CeLog("CeInputSaveConfig: saved key mapping");
}

/* Messages any idle native dialog generates on its own (button repaint
 * colour queries, cursor/hit-test housekeeping, ...) that have nothing
 * to do with a physical key press - excluded from the "\x6c7a\x5b9a"
 * diagnostic logging (both here and ce_main.c's WndProc) so real signal
 * isn't buried in it. Discovered the hard way: round 7's unfiltered
 * logging caught WM_CTLCOLORBTN (0x0135) while waiting for a remap
 * press, which looked like a candidate but is just routine button
 * painting that happens whether or not anything was pressed. */
static int IsRoutineDialogChatter(UINT msg)
{
    switch (msg)
    {
    case WM_PAINT:          case WM_NCPAINT:
    case WM_ERASEBKGND:     case WM_SETCURSOR:
    case WM_NCHITTEST:      case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:    case WM_GETTEXT:
    case WM_GETTEXTLENGTH:
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:    case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:
        return 1;
    default:
        return 0;
    }
}

/* Every row caption is a plain LTEXT hidden here and repainted by
 * WM_PAINT via CeBmpFontPaintLabel() (STATIC has no ownerdraw style).
 * The remap buttons and OK are BS_OWNERDRAW and redraw themselves from
 * WM_DRAWITEM. The A/B/L/R and diagonal-combo captions keep their .rc
 * text in English but are re-set to full-width Ａ/Ｂ/Ｌ/Ｒ and 右上/右下
 * /左下/左上 in Japanese (ApplyInputConfigLanguage) - either way they
 * need real IDs and the hide/repaint cycle so the bitmap font draws
 * them. */
static const int kInputLabelIds[] = {
    IDC_IC_LBL_UP, IDC_IC_LBL_DOWN, IDC_IC_LBL_LEFT, IDC_IC_LBL_RIGHT,
    IDC_IC_LBL_A,  IDC_IC_LBL_B,    IDC_IC_LBL_L,    IDC_IC_LBL_R,
    IDC_IC_LBL_UPRIGHT, IDC_IC_LBL_RIGHTDOWN, IDC_IC_LBL_DOWNLEFT, IDC_IC_LBL_LEFTUP,
    IDC_IC_LBL_SELECT, IDC_IC_LBL_START,
};
#define CE_INPUT_LABEL_COUNT (sizeof(kInputLabelIds) / sizeof(kInputLabelIds[0]))

/* One-shot at WM_INITDIALOG (nothing here changes CeLangIsJapanese()
 * while it's open). In Japanese all fourteen row captions are set here
 * (the direction/Select/Start names, plus A/B/L/R as full-width Ａ/Ｂ/Ｌ
 * /Ｒ and the four diagonal combos as 右上/右下/左下/左上 - user
 * request); in English A/B/L/R and the diagonal combos keep the .rc
 * text ("A".."R", "Up R".."L Up") untouched. The Shinonome set covers
 * every one of these glyphs (verified). */
static void ApplyInputConfigLanguage(HWND hDlg)
{
    unsigned i;

    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_IC_LBL_UP,     L"\x4e0a");                         /* 上 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_DOWN,   L"\x4e0b");                         /* 下 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_LEFT,   L"\x5de6");                         /* 左 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_RIGHT,  L"\x53f3");                         /* 右 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_A,      L"\xff21");                         /* Ａ */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_B,      L"\xff22");                         /* Ｂ */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_L,      L"\xff2c");                         /* Ｌ */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_R,      L"\xff32");                         /* Ｒ */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_UPRIGHT,   L"\x53f3\x4e0a");                /* 右上 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_RIGHTDOWN, L"\x53f3\x4e0b");                /* 右下 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_DOWNLEFT,  L"\x5de6\x4e0b");                /* 左下 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_LEFTUP,    L"\x5de6\x4e0a");                /* 左上 */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_SELECT, L"\x30bb\x30ec\x30af\x30c8");       /* セレクト */
        SetDlgItemTextW(hDlg, IDC_IC_LBL_START,  L"\x30b9\x30bf\x30fc\x30c8");       /* スタート */
        SetDlgItemTextW(hDlg, IDOK,     L"\x6c7a\x5b9a");                            /* 決定 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_IC_LBL_UP,     L"Up");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_DOWN,   L"Down");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_LEFT,   L"Left");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_RIGHT,  L"Right");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_SELECT, L"Select");
        SetDlgItemTextW(hDlg, IDC_IC_LBL_START,  L"Start");
        SetDlgItemTextW(hDlg, IDOK,     L"OK");
    }

    for (i = 0; i < CE_INPUT_LABEL_COUNT; i++)
        ShowWindow(GetDlgItem(hDlg, kInputLabelIds[i]), SW_HIDE);
}

#define WM_SETINPUTFOCUS (WM_APP + 204)

/* Physical-key focus chain for this dialog: despite the two-column grid
 * layout (left column Up/Down/Left/Right then the four diagonal combo
 * rows, right column A/B/L/R then Select/Start below a blank spacer row
 * - see ce_res.rc's IDD_INPUTCONFIG layout), focus moves through all
 * fifteen stops (fourteen remap buttons + OK) as a single loop in
 * reading order - left column top-to-bottom, then right column
 * top-to-bottom, then OK, then back to the top (same
 * left-to-right/top-to-bottom convention the sister Genesis/SNES CE
 * ports use). Up and Left both step back one stop, Down and Right both
 * step forward one stop - there's no separate "swap column" gesture
 * since the two columns are just this one list wrapped at row 8, not
 * independent axes (stepping down from R skips straight to Select,
 * since the blank spacer row has no control of its own to stop on).
 * Same explicit-subclass-every-control technique as this project's own
 * VideoNeighborUp/Down (ce_video.c) and an earlier prototype's
 * MiscNeighbor/KeySetNeighbor (see that prototype's source): this
 * device's dialog manager's own arrow-key group navigation isn't
 * confirmed reliable on this hardware/toolchain, so every control below
 * claims WANTARROWS | WANTALLKEYS and this table decides where each
 * press goes, guaranteeing the loop instead of hoping the default
 * WS_GROUP wraparound behaves as MSDN describes. */
static const int kInputOrder[15] = {
    IDC_IC_BTN_UP,        IDC_IC_BTN_DOWN,      IDC_IC_BTN_LEFT,     IDC_IC_BTN_RIGHT,
    IDC_IC_BTN_UPRIGHT,   IDC_IC_BTN_RIGHTDOWN, IDC_IC_BTN_DOWNLEFT, IDC_IC_BTN_LEFTUP,
    IDC_IC_BTN_A,         IDC_IC_BTN_B,         IDC_IC_BTN_L,        IDC_IC_BTN_R,
    IDC_IC_BTN_SELECT,    IDC_IC_BTN_START,
    IDOK,
};
#define CE_INPUT_ORDER_COUNT (sizeof(kInputOrder) / sizeof(kInputOrder[0]))

static int InputOrderIndex(int ctrlId)
{
    unsigned i;
    for (i = 0; i < CE_INPUT_ORDER_COUNT; i++)
        if (kInputOrder[i] == ctrlId)
            return (int)i;
    return -1;
}

static int InputNeighborUp(int ctrlId)
{
    int i = InputOrderIndex(ctrlId);
    if (i < 0)
        return ctrlId;
    return kInputOrder[(i + CE_INPUT_ORDER_COUNT - 1) % CE_INPUT_ORDER_COUNT];
}

static int InputNeighborDown(int ctrlId)
{
    int i = InputOrderIndex(ctrlId);
    if (i < 0)
        return ctrlId;
    return kInputOrder[(i + 1) % CE_INPUT_ORDER_COUNT];
}

static WNDPROC s_pInputOrigProc = NULL;

/* Subclasses all twelve remap buttons plus OK, claiming every key
 * unconditionally (DLGC_WANTARROWS | DLGC_WANTALLKEYS) - same blanket
 * approach as VideoCtrlProc/SoundCtrlProc above and an earlier
 * prototype's ButtonProc (see that prototype's source), which this proc's
 * WM_KEYDOWN case below is otherwise a direct port of (see
 * BeginWaitForKey's comment for why: capture is WM_KEYDOWN-only,
 * deliberately not also WM_KEYUP/WM_SYSKEY.../polling like an earlier
 * version of this dialog). Claiming WANTALLKEYS also means
 * IsDialogMessage() no longer swallows a decide/Enter press into a
 * synthesized BN_CLICKED before this dialog ever sees it (the round-9
 * workaround this replaced, see CancelWaitForKey's caller below) - the
 * raw key message reaches this proc directly instead, so starting a
 * remap and completing it with a decide press both go through one
 * unambiguous code path instead of depending on the dialog manager's
 * own Enter-to-click translation. */
static LRESULT CALLBACK InputBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    HWND hDlg = GetParent(hWnd);
    int  id   = GetDlgCtrlID(hWnd);
    int  idx  = CtrlIdToIndex(id);

    switch (message)
    {
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    case WM_KEYDOWN:
        /* Already waiting for a new binding on THIS control: any key at
         * all - including decide/Enter, and including the arrow keys
         * that would otherwise navigate - is the new binding, same
         * capture-wins-over-navigation priority as the earlier prototype's ButtonProc.
         * No filtering against "the key that started the wait" here on
         * purpose - see BeginWaitForKey's comment for why that turned
         * out to be the wrong fix. */
        if (idx >= 0 && idx == s_waitingIndex)
        {
            CaptureKeyAsBinding(hDlg, idx, (int)wParam);
            return 0;
        }
        switch (wParam)
        {
        case VK_UP:
        case VK_LEFT:  SetFocus(GetDlgItem(hDlg, InputNeighborUp(id)));   return 0;
        case VK_DOWN:
        case VK_RIGHT: SetFocus(GetDlgItem(hDlg, InputNeighborDown(id))); return 0;

        case VK_RETURN:
        case VK_SPACE:
            if (id == IDOK)
            {
                /* IDOK is subclassed too (for the Up/Down wrap), so its
                 * own decide press has to be forwarded explicitly. */
                SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            }
            else if (idx >= 0)
            {
                /* Not yet waiting - decide starts the remap directly,
                 * the same as a touch tap (see the BN_CLICKED handler
                 * in InputConfigDlgProc), without depending on a
                 * synthesized click. */
                BeginWaitForKey(hDlg, idx);
            }
            return 0;

        case VK_ESCAPE:
            /* Claiming WANTALLKEYS above means this control, not the
             * dialog manager, now sees the physical Back key too. */
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
        break;
    }

    return CallWindowProc(s_pInputOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK InputConfigDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        unsigned i;
        for (i = 0; i < CE_INPUT_COUNT; i++)
            SetWindowTextW(GetDlgItem(hDlg, s_map[i].ctrlId), VkToLabel(s_map[i].vk));
        ApplyInputConfigLanguage(hDlg);

        s_pInputOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        for (i = 0; i < CE_INPUT_COUNT; i++)
            SetWindowLongPtrW(GetDlgItem(hDlg, s_map[i].ctrlId), GWLP_WNDPROC, (LONG_PTR)InputBtnCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC, (LONG_PTR)InputBtnCtrlProc);

        /* Belt-and-suspenders initial focus, same pattern as every other
         * dialog in this port (see Video/Sound Config's own
         * WM_INITDIALOG) - a synchronous SetFocus() from WM_INITDIALOG
         * alone doesn't always stick on this device. */
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_IC_BTN_UP));
        PostMessage(hDlg, WM_SETINPUTFOCUS, 0, 0);
        return FALSE;
    }

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        unsigned i;
        for (i = 0; i < CE_INPUT_LABEL_COUNT; i++)
            CeBmpFontPaintLabel(hdc, hDlg, kInputLabelIds[i]);
        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            SetFocus(GetDlgItem(hDlg, IDC_IC_BTN_UP));
            PostMessage(hDlg, WM_SETINPUTFOCUS, 0, 0);
        }
        break;

    case WM_SETINPUTFOCUS:
        SetFocus(GetDlgItem(hDlg, IDC_IC_BTN_UP));
        break;

    /* No WM_TIMER here anymore (the GetAsyncKeyState-polling remap
     * capture it used to drive is gone - see BeginWaitForKey's comment)
     * and no WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP either:
     * WM_INITDIALOG above always parks focus on one of the twelve
     * subclassed remap buttons (or OK), so any such message is delivered
     * straight to InputBtnCtrlProc instead of ever reaching this dialog
     * proc (see that function's own WM_KEYDOWN handling). */

    case WM_COMMAND:
    {
        int ctrlId = LOWORD(wParam);
        int idx = CtrlIdToIndex(ctrlId);

        if (idx >= 0 && HIWORD(wParam) == BN_CLICKED)
        {
            if (idx == s_waitingIndex)
            {
                /* A genuine re-tap on the button that's already waiting
                 * for a new key - a decide/Enter press no longer reaches
                 * here at all now that every remap button is subclassed
                 * (InputBtnCtrlProc's WM_KEYDOWN captures that directly,
                 * bypassing the BN_CLICKED synthesis that used to make
                 * this ambiguous - see the dev notes' round-9 note for the
                 * history). Cancel instead of restarting, same re-tap-
                 * to-cancel gesture as an earlier prototype's own
                 * remap buttons (see that project's ButtonProc). */
                CancelWaitForKey(hDlg);
                return TRUE;
            }
            BeginWaitForKey(hDlg, idx);
            return TRUE;
        }

        switch (ctrlId)
        {
        case IDOK:
        case IDCANCEL:
            /* Physical Back (IDCANCEL, no longer a visible button on
             * this dialog - see ce_res.rc) acts the same as touching OK
             * here - this device has no meaningful "discard changes"
             * gesture, only "go back", so both commit and close (same
             * philosophy as every settings dialog in an earlier
             * prototype). */
            EndWaitForKey(hDlg);
            CeInputSaveConfig();
            EndDialog(hDlg, ctrlId);
            return TRUE;
        }
        return FALSE;
    }

    default:
        /* General-purpose diagnostic left in place for any *other*
         * hardware button that turns out to be similarly hard to detect
         * in the future (the Decide/OK button mystery this was originally
         * added for is resolved - see the BN_CLICKED handling above).
         * Logs while a remap press is pending (a short, user-initiated
         * window, not continuous), filtered through
         * IsRoutineDialogChatter() so ordinary button-repaint/cursor
         * chatter any idle dialog generates doesn't get mistaken for a
         * candidate (round 7 learned that the hard way with
         * WM_CTLCOLORBTN). */
        if (s_waitingIndex >= 0 && !IsRoutineDialogChatter(msg))
            CeLog("InputConfigDlgProc: msg=0x%04X wParam=0x%08X while waiting for a key press",
                  (unsigned)msg, (unsigned)wParam);
        return FALSE;
    }
    /* WM_ACTIVATE/WM_SETINPUTFOCUS above end in break, not return - this
     * catches the fall-through, same fix as SoundConfigDlgProc/
     * VideoConfigDlgProc's identical "control reaches end of non-void
     * function" warning (see the dev notes). */
    return FALSE;
}

void CeShowInputConfigDialog(HWND owner)
{
    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE), MAKEINTRESOURCEW(IDD_INPUTCONFIG),
               owner, InputConfigDlgProc);
}

int CeInputIsWaitingForKeyRemap(void)
{
    return s_waitingIndex >= 0;
}
