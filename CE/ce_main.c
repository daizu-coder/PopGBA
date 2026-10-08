/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * PopGBA's Windows CE libretro frontend: a thin shell that drives
 * retro_init/retro_load_game/retro_run and turns the retro_set_*
 * callbacks into real GDI/waveOut/key I/O. It has the same shape as the
 * sister ports' frontends (PopSG and PopSNES, both hardware-validated).
 */

#include "ce_app_config.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#include <libretro.h>

#include "ce_log.h"
#include "ce_gapi.h"
#include "ce_input.h"
#include "ce_audio.h"
#include "ce_video.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_fileopen.h"
#include "ce_resource.h"

static const wchar_t kWndClassName[] = CE_APP_WND_CLASS;
static const wchar_t kMutexName[]    = CE_APP_MUTEX_NAME;
static const wchar_t kAppTitle[]     = CE_APP_TITLE;

static HWND   g_hwnd     = NULL;
static HANDLE g_mutex    = NULL;
static volatile int g_running = 0;

/* Japanese-capable replacement for MessageBoxW() - implemented further
 * down (IDD_MSGBOX / IDD_CONFIRM section), forward-declared here so the
 * save/load-state helpers above that section can use it. See
 * ce_res.rc's IDD_MSGBOX comment for why MessageBoxW() can't carry
 * Japanese text on this device any more. */
static void CeShowMsgBox(HWND owner, const wchar_t *text);

/* Set by RestartProcess() when a dynarec-safe ROM switch has spawned a
 * suspended relaunched instance; CeShutdown() resumes it (see that
 * function's comment for why the resume is deferred there instead of
 * happening immediately in RestartProcess()). NULL in the ordinary
 * (non-restart) shutdown path. */
static HANDLE s_restartResumeThread  = NULL;
static HANDLE s_restartResumeProcess = NULL;

static wchar_t g_romPath[MAX_PATH] = L""; /* last successfully-loaded ROM's path, for save-state/SRAM file naming */

/* g_romLoaded: a game has been successfully retro_load_game()'d at
 * least once (stays true across File>Open reloads until exit).
 * g_paused: the touch-to-reveal menu is up right now - retro_run() is
 * not called and GAPI is not holding the display while this is true, so
 * normal GDI (the menu, WM_PAINT) can draw. */
static int  g_romLoaded = 0;
static int  g_paused    = 0;

/* Target ms/frame for WinMain's pacing loop (see the "no frame pacing"
 * root cause in the dev notes' white-screen investigation - real-hardware
 * logs showed CeAudioPushSamples's ring buffer overrunning continuously
 * (hundreds of thousands of dropped samples), which only makes sense if
 * retro_run() is being called far faster than the core's own 59.728fps
 * timing since each call always emulates exactly one GBA frame's worth
 * of GBA-clock time regardless of how fast the host races through it).
 * Updated from the core's real fps once a ROM is loaded (LoadRomFlow) -
 * this default just covers the brief window between retro_init() and
 * the first successful load. */
static double g_frameIntervalMs = 1000.0 / 59.728;

static void CeShutdown(int exitCode); /* used by MainMenuDlgProc, below */
static void CeShowShellChrome(HWND hwnd); /* used by CeShutdown, defined further below */
static int  ResolveBiosSystemDir(void);      /* defined below; called once from WinMain */
static void CeHideShellChrome(HWND hwnd); /* used by ShowMainMenuDialog and WinMain, defined further below */

/* round 66: picture of the main menu, taken just before it closes for
 * Open ROM (see MainMenuDlgProc's IDC_MM_OPEN), painted by the main
 * window's WM_PAINT while the ROM picker is up so the menu still looks
 * like it's behind the picker. Freed once the picker closes. */
static HBITMAP s_menuSnap;
static RECT s_menuSnapRect; /* in main-window client coordinates */

static void CeMenuSnapFree(HWND hwnd)
{
    if (!s_menuSnap)
        return;
    DeleteObject(s_menuSnap);
    s_menuSnap = NULL;
    InvalidateRect(hwnd, &s_menuSnapRect, TRUE);
}

static void CeMenuSnapTake(HWND hDlg)
{
    HWND owner = GetParent(hDlg);
    RECT rc;
    POINT pt;
    int w, h;
    HDC scr, mem;
    HGDIOBJ old;

    if (!owner)
        return;
    CeMenuSnapFree(owner);
    GetWindowRect(hDlg, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return;

    scr = GetDC(NULL);
    mem = CreateCompatibleDC(scr);
    s_menuSnap = CreateCompatibleBitmap(scr, w, h);
    if (mem && s_menuSnap)
    {
        old = SelectObject(mem, s_menuSnap);
        BitBlt(mem, 0, 0, w, h, scr, rc.left, rc.top, SRCCOPY);
        SelectObject(mem, old);
        pt.x = rc.left;
        pt.y = rc.top;
        ScreenToClient(owner, &pt);
        SetRect(&s_menuSnapRect, pt.x, pt.y, pt.x + w, pt.y + h);
    }
    else if (s_menuSnap)
    {
        DeleteObject(s_menuSnap);
        s_menuSnap = NULL;
    }
    if (mem)
        DeleteDC(mem);
    ReleaseDC(NULL, scr);
}

static void CeMenuSnapPaint(HWND hwnd)
{
    HDC hdc, mem;
    HGDIOBJ old;

    if (!s_menuSnap)
        return;
    hdc = GetDC(hwnd);
    mem = CreateCompatibleDC(hdc);
    if (mem)
    {
        old = SelectObject(mem, s_menuSnap);
        BitBlt(hdc, s_menuSnapRect.left, s_menuSnapRect.top,
               s_menuSnapRect.right - s_menuSnapRect.left,
               s_menuSnapRect.bottom - s_menuSnapRect.top,
               mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    ReleaseDC(hwnd, hdc);
}

/* ------------------------------------------------------------------ */
/* libretro callbacks                                                  */
/* ------------------------------------------------------------------ */

/* Set by the core via RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK
 * whenever Video Config's Frame Skip is above 0 (ce_video.c) - NULL
 * otherwise (Frame Skip Off, or before retro_load_game()'s first
 * check_variables() call). Invoked once per frame from WinMain's main
 * loop, right before retro_run(), per that environment call's contract -
 * see the call site below. */
static retro_audio_buffer_status_callback_t g_audioBuffStatusCb = NULL;

/* Audio "catch-up" mode - see WinMain's frame-pacer block. When Frame
 * Skip is enabled (g_audioBuffStatusCb != NULL) and the waveOut ring
 * buffer drains below CE_AUDIO_CATCHUP_LOW_PCT, the pacer stops sleeping
 * and runs retro_run() back-to-back (skipped frames, ~12ms each, produce
 * a full frame's worth of audio) to physically refill the ring, until it
 * recovers past CE_AUDIO_CATCHUP_OK_PCT. s_audioCatchup carries the latch
 * state from the pacer to the g_audioBuffStatusCb feed a few lines later,
 * where it forces the core's "underrun likely" flag so every catch-up
 * frame skips rendering regardless of the raw occupancy threshold. With
 * Frame Skip off (the default) g_audioBuffStatusCb is NULL, so this never
 * engages and existing behaviour is unchanged.
 *
 * HW-verified 2026-08-31 (Frame Skip 4 + this catch-up + ce_audio.c's
 * underrun fade + the 24576 ring default): audio underrun ~33/s -> 0.36/s
 * (below the rival "comparison" port's ~1.0/s), retro_run avg 25ms ->
 * 16ms (real-time reached), gap-entry step 270 -> 46. Frame Skip 0
 * (default) path is byte-for-byte unchanged - no regression. Full numbers
 * in the dev notes round 34 / 34b. */
#define CE_AUDIO_CATCHUP_LOW_PCT 45   /* enter catch-up below this ring occupancy % */
#define CE_AUDIO_CATCHUP_OK_PCT  75   /* leave catch-up at/above this ring occupancy % */
static int s_audioCatchup = 0;

/* gpSP's own error_msg()/info_msg() (libretro.c) - used for e.g. "Could
 * not load BIOS image file"/"Could not load the game file" - are no-ops
 * unless the core has a log_cb, which it only gets if this environment
 * call is answered (see RETRO_ENVIRONMENT_GET_LOG_INTERFACE below).
 * Without this, retro_load_game() failures were showing up in
 * PopGBA_debug.log as a bare "LoadRomFlow: retro_load_game failed" with
 * no indication of *why* - the core's own diagnostic text was being
 * silently swallowed. Routed straight into CeLog() so it lands in the
 * same debug log as everything else. */
static void RETRO_CALLCONV ce_retro_log(enum retro_log_level level, const char *fmt, ...)
{
    char msg[512];
    va_list args;

    va_start(args, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, args);
    msg[sizeof(msg) - 1] = '\0';
    va_end(args);

    CeLog("core[%d]: %s", (int)level, msg);
}

/* Converts an existing wide-char path (the exe's own directory, the BIOS
 * system dir, or a picked ROM file) to a narrow string safe to hand to
 * gpSP's fopen()-based file I/O (common.h's file_open() macro). Encoded
 * as CP_UTF8, decoded back to wide by ce_fopen_utf8() (compat/stdio.h)
 * instead of the CRT's own fopen() doing a lossy CP_ACP conversion
 * internally - see compat/stdio.h's header comment for the full history
 * (this used to go through GetShortPathNameW()'s FAT 8.3 short name,
 * with a CP_ACP fallback, because this device's GetACP() is 1252/
 * Western and can't represent Japanese at all; CP_UTF8 sidesteps that
 * codepage question entirely instead of working around it). */
static void WidePathToNarrow(const wchar_t *wide, char *out, size_t outSize)
{
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)outSize, NULL, NULL);
}

/* Narrow directory handed to the core for RETRO_ENVIRONMENT_GET_SYSTEM_
 * DIRECTORY (where gpSP looks for gba_bios.bin). Empty until a folder
 * that actually holds gba_bios.bin is located; while empty the
 * environment query returns false and the core looks next to the ROM
 * instead. If no usable gba_bios.bin turns up there either, the core
 * runs on its built-in open-source BIOS. Never gates startup, the main
 * menu or a ROM load. */
static char g_biosSysDirUtf8[MAX_PATH * 3] = "";

static int DirHasGbaBiosW(const wchar_t *dir)
{
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%s\\gba_bios.bin", dir);
    p[MAX_PATH - 1] = L'\0';
    return GetFileAttributesW(p) != 0xFFFFFFFF;
}

static void SetBiosSysDirFromW(const wchar_t *dir)
{
    WidePathToNarrow(dir, g_biosSysDirUtf8, sizeof(g_biosSysDirUtf8));
    CeLog("BIOS: system dir configured");
}

/* Point g_biosSysDirUtf8 at a folder holding gba_bios.bin without
 * persisting an absolute path that breaks if the app is moved:
 *   (a) the folder remembered from a previous BIOS pick (config key
 *       "BiosDir"). v1.0.2 removed the BIOS picker, so nothing writes
 *       this key any more, but a PopGBA.cfg from v1.0.0/v1.0.1 may still
 *       carry one and it is honoured so those users keep their BIOS;
 *   (b) else gba_bios.bin sitting next to AppMain.exe.
 * Either can be a Japanese folder name - see WidePathToNarrow()'s
 * comment. Returns 1 if a BIOS dir is now configured, 0 if none was
 * found (the core then looks next to the ROM, and otherwise uses its
 * built-in open-source BIOS). Earlier builds
 * copied gba_bios.bin to a fixed ASCII cache dir (\Storage Card\
 * PopGBA_data); that copy is gone - the user keeps the BIOS where they
 * put it. */
static int ResolveBiosSystemDir(void)
{
    wchar_t dir[MAX_PATH];
    char dirUtf8[MAX_PATH * 3];
    wchar_t *slash;

    dirUtf8[0] = '\0';
    CeConfigGetString("BiosDir", dirUtf8, sizeof(dirUtf8));
    if (dirUtf8[0])
    {
        MultiByteToWideChar(CP_UTF8, 0, dirUtf8, -1, dir, MAX_PATH);
        dir[MAX_PATH - 1] = L'\0';
        if (DirHasGbaBiosW(dir))
        {
            SetBiosSysDirFromW(dir);
            return 1;
        }
    }

    GetModuleFileNameW(NULL, dir, MAX_PATH);
    slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = L'\0';
    if (DirHasGbaBiosW(dir))
    {
        SetBiosSysDirFromW(dir);
        return 1;
    }

    return 0;
}

/* Temporary: backs RETRO_ENVIRONMENT_GET_PERF_INTERFACE so libretro.c's
 * retro_run() can time its own internal phases (switch_to_cpu_thread()/
 * render_audio()/video_run() - see the "perf: cpu_thread"/"perf:
 * render_audio"/"perf: video_run" logging added there) via
 * perf_cb.get_time_usec() instead of needing <windows.h> directly in that
 * portable core file (shared with the PSP/3DS/x86 targets). Backed by
 * GetTickCount(), so ~1ms resolution - plenty given these phases cost
 * single-digit-to-tens of ms on this device. perf_register/start/stop are
 * unused no-ops: nothing in this build defines PERF_TEST, so libretro.c's
 * RETRO_PERFORMANCE_* counter macros never call them - only perf_log() is
 * invoked unconditionally (retro_deinit()), hence the no-op body rather
 * than leaving any of these NULL. Remove this whole block, the
 * environment case below, and the matching perf logging in libretro.c
 * once the bottleneck is identified. */
static retro_time_t RETRO_CALLCONV ce_perf_get_time_usec(void)
{
    return (retro_time_t)GetTickCount() * 1000;
}

static retro_perf_tick_t RETRO_CALLCONV ce_perf_get_counter(void)
{
    return (retro_perf_tick_t)GetTickCount();
}

static uint64_t RETRO_CALLCONV ce_perf_get_cpu_features(void)
{
    return 0;
}

static void RETRO_CALLCONV ce_perf_counter_noop(struct retro_perf_counter *counter)
{
    (void)counter;
}

static void RETRO_CALLCONV ce_perf_log(void)
{
}

static bool ce_environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        enum retro_pixel_format *fmt = (enum retro_pixel_format *)data;
        /* This device's GAPI surface is RGB565 (confirmed on both prior
         * CE ports); refuse anything else so the core doesn't silently
         * assume a format we can't display. gpSP's retro_load_game()
         * unconditionally sets RGB565 itself (libretro.c) regardless of
         * what this returns, so this is a belt-and-suspenders check
         * rather than a real gate - kept for parity with the sister
         * ports and in case that ever changes upstream. */
        return (*fmt == RETRO_PIXEL_FORMAT_RGB565);
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        /* Video Config's color-correction/frame-skip settings
         * (ce_video.c) ride the core's own existing core-options protocol
         * (gpsp_color_correction / gpsp_frameskip - see
         * check_variables() in libretro.c) instead of a new side channel -
         * CE just needs to answer these queries. */
        struct retro_variable *var = (struct retro_variable *)data;
        return CeVideoEnvGetVariable(var->key, &var->value) ? true : false;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        /* Lets a Video Config change made mid-session (ROM already
         * loaded, retro_load_game() - which would otherwise be the only
         * point check_variables() re-reads these - not called again)
         * take effect on the very next retro_run() instead of needing a
         * File>Open reload. */
        *(bool *)data = CeVideoConsumeDirty() ? true : false;
        return true;

    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
    {
        /* The core only asks for this when frame skip is "auto" (see
         * check_variables()/init_frameskip() in libretro/libretro.c) -
         * without answering it, that mode silently never skips anything
         * (retro_audio_buff_active stays false forever). data is NULL
         * when the core wants to unregister. */
        const struct retro_audio_buffer_status_callback *cb =
            (const struct retro_audio_buffer_status_callback *)data;
        g_audioBuffStatusCb = cb ? cb->callback : NULL;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    {
        struct retro_log_callback *cb = (struct retro_log_callback *)data;
        cb->log = ce_retro_log;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
    {
        struct retro_perf_callback *cb = (struct retro_perf_callback *)data;
        cb->get_time_usec    = ce_perf_get_time_usec;
        cb->get_cpu_features = ce_perf_get_cpu_features;
        cb->get_perf_counter = ce_perf_get_counter;
        cb->perf_register    = ce_perf_counter_noop;
        cb->perf_start       = ce_perf_counter_noop;
        cb->perf_stop        = ce_perf_counter_noop;
        cb->perf_log         = ce_perf_log;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    {
        /* gpSP does not need a real GBA BIOS image: when gba_bios.bin
         * (copyrighted Nintendo firmware, not something this port can
         * ship) is missing or looks wrong, retro_load_game() in
         * libretro.c silently uses its built-in open-source BIOS
         * (bios/open_gba_bios.bin) instead, so a missing BIOS never makes
         * a ROM load fail. A user who does have their own dump only
         * needs one gba_bios.bin dropped next to AppMain.exe: answering
         * this query with the folder ResolveBiosSystemDir() located (next
         * to AppMain.exe, or one remembered from an older version's BIOS
         * picker) points the core there. Until one is located g_biosSys
         * DirUtf8 is empty - return false so the core looks for
         * gba_bios.bin next to the ROM being loaded (main_path,
         * libretro.c) instead. */
        if (g_biosSysDirUtf8[0] == '\0')
        {
            CeLog("ce_environment: SYSTEM_DIRECTORY not resolved yet");
            return false;
        }
        CeLog("ce_environment: SYSTEM_DIRECTORY resolved to \"%s\"", g_biosSysDirUtf8);
        *(const char **)data = g_biosSysDirUtf8;
        return true;
    }

    default:
        /* Everything else (SET_CORE_OPTIONS*, GET_INPUT_BITMASKS, ...) is
         * optional per the libretro API contract - returning false tells
         * the core to use its built-in defaults, which is exactly what we
         * want until there is a settings UI for each of those too. */
        return false;
    }
}

static void ce_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    /* Keeps Video Config's Frame Skip consecutive-skip counter (see
     * ce_video.h) in sync with what the core actually did this frame:
     * data is NULL exactly when the core skipped rendering. */
    CeVideoFrameSkipNotifyRendered(data != NULL);

    if (!data)
        return; /* duplicate/skipped frame - nothing new to draw */

    /* Self-contained per call (width/height/pitch given fresh every
     * time, and always RGB565 per ce_environment()'s SET_PIXEL_FORMAT
     * handling above). */
    GapiBlitRGB565(data, width, height, (unsigned)pitch);
}

static void ce_audio_sample_noop(int16_t left, int16_t right)
{
    /* The core only ever uses retro_set_audio_sample_batch (see
     * retro_set_audio_sample() in libretro.c - it's an intentional
     * no-op setter), but we still register a real callback rather than
     * NULL to avoid relying on that being true forever. */
    (void)left;
    (void)right;
}

/* Temporary perf instrumentation (same pattern as ce_gapi.c's existing
 * "perf: blit" logging - see GapiBlitRGB565()): platform/libretro/
 * libretro.c calls audio_batch_cb() exactly once per retro_run(), so
 * this is directly comparable, per-frame, to retro_run's own timing
 * below and blit's. Lets retro_run_avg - blit_avg - audio_push_avg
 * stand in for "core CPU/PPU/sound-chip emulation alone", without
 * touching platform/libretro/libretro.c or anything under pico/. Remove
 * once the bottleneck is identified. */
static size_t ce_audio_sample_batch(const int16_t *data, size_t frames)
{
    static unsigned s_accumMs = 0, s_maxMs = 0, s_count = 0;
    DWORD t0 = GetTickCount();
    size_t ret = CeAudioPushSamples(data, frames);
    unsigned elapsed = (unsigned)(GetTickCount() - t0);

    s_accumMs += elapsed;
    if (elapsed > s_maxMs)
        s_maxMs = elapsed;
    if (++s_count >= 60)
    {
        CeLog("perf: audio_push avg=%ums max=%ums over %u frames",
              s_accumMs / s_count, s_maxMs, s_count);
        s_accumMs = 0;
        s_maxMs = 0;
        s_count = 0;
    }
    return ret;
}

static void ce_input_poll(void)
{
    CeInputPoll();
}

static int16_t ce_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return CeInputState(port, device, index, id);
}

/* ------------------------------------------------------------------ */
/* ROM loading                                                         */
/* ------------------------------------------------------------------ */

/* gpSP's retro_get_system_info() sets need_fullpath = true (libretro.c),
 * i.e. the core wants to open/read the ROM itself from the given path
 * rather than have the frontend preload the whole file into memory first -
 * same as the sister PopSG port, unlike the PopSNES port's
 * own PickAndLoadRom() (that core does not set need_fullpath). This is
 * simpler and avoids an extra malloc()+fread() of the entire file for no
 * benefit. */
static int PickRom(HWND owner, wchar_t *outPath, size_t outPathCount)
{
    WIN32_FIND_DATAW fd;
    HANDLE hFind;

    memset(outPath, 0, outPathCount * sizeof(wchar_t));

    /* Custom listbox-based picker (ce_fileopen.c), not GetOpenFileNameW()
     * - the standard common dialog has no way to render Japanese folder/
     * file names on this device (see ce_fileopen.c's header comment). */
    if (!CeShowFileOpenDialog(owner, outPath, outPathCount, CE_FILEOPEN_ROM))
    {
        CeLog("PickRom: file picker cancelled");
        return 0;
    }

    hFind = FindFirstFileW(outPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        CeLog("PickRom: selected file no longer exists");
        return 0;
    }
    FindClose(hFind);

    return 1;
}

/* ------------------------------------------------------------------ */
/* Battery-backed cartridge save (SRAM)                                */
/* ------------------------------------------------------------------ */

/* What the .srm file holds (last full load or save), so the 30-second
 * autosave and the exit save (CeSaveSramIfChanged) can skip the write
 * when the game hasn't changed its SRAM since. On a real PW-G5300 the
 * write's fclose() alone took 144-1925ms on the main thread (2026-10-07),
 * long enough to drain the audio ring and cut the sound out for up to
 * ~1.8s every 30s even when nothing had changed. The core's save RAM is
 * always 0x20000 bytes (retro_get_memory_size() in libretro.c).
 * s_srmShadowValid is 0 when the file's content isn't known (a short
 * read, a failed write). */
static unsigned char s_srmShadow[0x20000];
static int s_srmShadowValid = 0;

/* 1 when this game's .srm file exists (CeLoadSram opened it, or
 * CeSaveSram wrote it). While it doesn't, s_srmShadow holds the SRAM as
 * it was right after loading, and every checkpoint (pause, ROM switch,
 * autosave, exit) writes only once the game has changed it, so a game
 * the player never saved in doesn't get a .srm - the core reports its
 * full 0x20000 bytes for every game, even one with no save at all.
 * Once the file exists, the pause and ROM-switch checkpoints write every
 * time (CeSaveSramCheckpoint) - same as the sister PopSG port's v1.0.6. */
static int s_srmFileExists = 0;

static void CeSramRemember(const void *sram, size_t size)
{
    if (size <= sizeof(s_srmShadow))
    {
        memcpy(s_srmShadow, sram, size);
        s_srmShadowValid = 1;
    }
    else
        s_srmShadowValid = 0;
}

/* Loads "<romPath>.srm" into the core's SRAM, if this game has any
 * (RETRO_MEMORY_SAVE_RAM) and a save file already exists. This is what
 * makes a game's own in-cartridge save feature survive across app
 * restarts, same as a real battery-backed cartridge would - ported from
 * the sister PopSNES port's own CeLoadSram/CeSaveSram, which added
 * this after a real power-off test showed relying only on graceful
 * app-exit/ROM-switch checkpoints lost saves (see that project's
 * the dev notes round 8/9). No .srm file yet is the normal case for a new
 * game (or one with no SRAM at all) and isn't logged as an error. */
static void CeLoadSram(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    wchar_t sramPath[MAX_PATH + 8];
    FILE *f;
    size_t got;

    s_srmShadowValid = 0;
    s_srmFileExists = 0;
    if (!sram || size == 0)
        return; /* this game has no battery-backed SRAM */

    _snwprintf(sramPath, MAX_PATH + 8, L"%s.srm", g_romPath);
    f = _wfopen(sramPath, L"rb");
    if (!f)
    {
        CeLog("CeLoadSram: no .srm file yet (new game, or none saved)");
        CeSramRemember(sram, size); /* no checkpoint writes until the game changes it (s_srmFileExists) */
        return;
    }
    s_srmFileExists = 1;

    /* Read at most `size` bytes - a mismatched-size .srm (shouldn't
     * happen for a given ROM, but don't overrun the core's buffer if it
     * somehow does) is truncated, not rejected outright. */
    got = fread(sram, 1, size, f);
    fclose(f);
    if (got == size)
        CeSramRemember(sram, size);
    CeLog("CeLoadSram: loaded %lu of %lu bytes", (unsigned long)got, (unsigned long)size);
}

/* Writes the core's current SRAM out to "<romPath>.srm" - the other half
 * of CeLoadSram(). Called whenever a loaded game's SRAM is about to stop
 * being the live one (File>Open loading a different ROM, or app exit),
 * plus a pause-time checkpoint (ShowMainMenuDialog) and a periodic
 * autosave (WinMain's loop), same three checkpoints the sister
 * PopSNES port settled on. The autosave and app exit go through
 * CeSaveSramIfChanged(), the pause and ROM-switch checkpoints through
 * CeSaveSramCheckpoint(), both below. */
static void CeSaveSram(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    wchar_t sramPath[MAX_PATH + 8];
    FILE *f;
    size_t wrote;
    int closeErr;

    if (!sram || size == 0)
        return; /* this game has no battery-backed SRAM - nothing to save */

    _snwprintf(sramPath, MAX_PATH + 8, L"%s.srm", g_romPath);
    f = _wfopen(sramPath, L"wb");
    if (!f)
    {
        s_srmShadowValid = 0;
        CeLog("CeSaveSram: failed to open .srm file for write");
        return;
    }

    wrote = fwrite(sram, 1, size, f);
    closeErr = fclose(f);
    s_srmFileExists = 1; /* "wb" created or truncated it, even if the write then failed */
    if (wrote == size && closeErr == 0)
        CeSramRemember(sram, size);
    else
        s_srmShadowValid = 0;
    CeLog("CeSaveSram: saved %lu bytes", (unsigned long)size);
}

/* WinMain's 30-second autosave and CeShutdown()'s exit save: same as
 * CeSaveSram(), but skips the write when the SRAM still matches what the
 * .srm file holds (see s_srmShadow) - or, while there is no .srm yet,
 * what the SRAM held right after loading. Exiting from the main menu
 * always comes right after the menu's own pause-time save, so the exit
 * write was a second copy of the same bytes. The pause and ROM-switch
 * checkpoints go through CeSaveSramCheckpoint() below. */
static void CeSaveSramIfChanged(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);

    if (sram && s_srmShadowValid && size <= sizeof(s_srmShadow) &&
        memcmp(s_srmShadow, sram, size) == 0)
        return;
    CeSaveSram();
}

/* The pause-time save (ShowMainMenuDialog) and the ROM-switch saves
 * (LoadRomPath's in-process switch, CeShutdown's RestartProcess() path):
 * always write while the game's .srm exists, as before; while it
 * doesn't, write only if the SRAM changed since loading (see
 * s_srmFileExists). */
static void CeSaveSramCheckpoint(void)
{
    if (s_srmFileExists)
        CeSaveSram();
    else
        CeSaveSramIfChanged();
}

/* Paints "Loading..."/"読み込み中..." directly onto hwnd - whichever
 * window is actually on screen at the call site below (the main menu
 * dialog during the interactive Open ROM flow, or the main window during
 * WinMain's command-line ROM-relaunch flow after RestartProcess()).
 * Draws straight via GetDC/ReleaseDC instead of InvalidateRect+WM_PAINT:
 * nothing pumps hwnd's message queue again until the caller's blocking
 * retro_load_game() call returns (up to ~8s on hardware, see the dev notes),
 * so a queued repaint would never get processed in time to be seen - this
 * needs to land on screen synchronously, right here, right before that
 * call. */
static void CeShowLoadingScreen(HWND hwnd)
{
    RECT rc;
    HDC hdc = GetDC(hwnd);
    const wchar_t *text = L"Loading...";
    int textW, x, y;

    if (!hdc)
        return;

    /* Japanese text when CeLangIsJapanese() is on. Since the switch to
     * the Shinonome bitmap font (ce_bmpfont.c) there's no background
     * font load to race any more - the glyphs are baked into the binary,
     * always available - so this can simply honour the setting with no
     * non-blocking dance. */
    if (CeLangIsJapanese())
        text = L"\x8aad\x307f\x8fbc\x307f\x4e2d..."; /* 読み込み中... */

    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(hdc, TRANSPARENT);

    textW = CeBmpFontGetTextWidth(text);
    x = rc.left + ((rc.right - rc.left) - textW) / 2;
    y = rc.top + ((rc.bottom - rc.top) - CE_BMPFONT_HEIGHT) / 2;
    CeBmpFontDrawTextW(hdc, x, y, text, RGB(255, 255, 255));

    ReleaseDC(hwnd, hdc);
}

/* Loads romPath directly into the core, in-process (unloading whatever
 * was running first, same as always). Shared by LoadRomFlow() (below,
 * after PickRom() gets a path interactively) and WinMain() (loading a
 * path handed to it on the command line by RestartProcess() below -
 * see that function's comment). Returns 1 on success, 0 if loading
 * failed - a MessageBox already explains why in that case, and whatever
 * was running before (if anything) is left unloaded either way, matching
 * this function's one caller-independent contract. */
static int LoadRomPath(HWND hwnd, const wchar_t *romPath)
{
    struct retro_game_info game;
    struct retro_system_av_info avInfo;
    static char pathUtf8[MAX_PATH * 3]; /* generous size; CP_ACP never expands beyond 2 bytes/char */
    wchar_t title[MAX_PATH + 32];
    const wchar_t *base;

    if (g_romLoaded)
    {
        CeSaveSramCheckpoint(); /* g_romPath/the core's SRAM/s_srmFileExists still refer to the *previous* game here - new one isn't loaded yet */
        retro_unload_game();
    }

    CeShowLoadingScreen(hwnd);

    WidePathToNarrow(romPath, pathUtf8, sizeof(pathUtf8));
    CeLog("LoadRomPath: ROM path resolved to \"%s\"", pathUtf8);
    memset(&game, 0, sizeof(game));
    game.path = pathUtf8;
    game.data = NULL; /* gpSP's retro_load_game() reads the file itself via game.path - see PickRom's comment */
    game.size = 0;

    if (!retro_load_game(&game))
    {
        /* A missing or wrong gba_bios.bin never gets here - the core falls
         * back to its built-in open-source BIOS (see GET_SYSTEM_DIRECTORY
         * in ce_environment). What does fail is the ROM itself: the file
         * can't be opened, is empty or over 512 MiB, or there is not
         * enough memory for it (retro_load_game()/load_gamepak()). Asking
         * for a BIOS can't fix any of those, so just say the ROM didn't
         * load. Up to v1.0.1 this asked for gba_bios.bin first, a leftover
         * from an older core that really needed one. */
        CeLog("LoadRomPath: retro_load_game failed");
        CeShowMsgBox(hwnd, CeLangIsJapanese()
            ? L"ROM \x3092\x8aad\x307f\x8fbc\x3081\x307e\x305b\x3093\x3067\x3057\x305f\x3002" /* ROM を読み込めませんでした。 */
            : L"Failed to load ROM.");
        g_romLoaded = 0;
        /* Restores hwnd (the loading screen's black fill + text, still
         * covering it) once the message box above is dismissed - RDW_
         * ALLCHILDREN so the main menu dialog's own buttons repaint too,
         * not just the dialog's own background. */
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        return 0;
    }

    g_romLoaded = 1;
    GapiInvalidateLastImage(); /* screenshot: don't save the previous ROM's picture */
    wcsncpy(g_romPath, romPath, MAX_PATH - 1);
    g_romPath[MAX_PATH - 1] = L'\0';

    CeLoadSram();

    retro_get_system_av_info(&avInfo);
    CeLog("LoadRomPath: loaded, geometry=%ux%u fps=%.3f sample_rate=%.0f",
          avInfo.geometry.base_width, avInfo.geometry.base_height,
          avInfo.timing.fps, avInfo.timing.sample_rate);
    CeAudioStart(avInfo.timing.sample_rate);

    if (avInfo.timing.fps > 0.0)
        g_frameIntervalMs = 1000.0 / avInfo.timing.fps; /* see g_frameIntervalMs's declaration - paces WinMain's loop to this */

    base = wcsrchr(romPath, L'\\');
    _snwprintf(title, MAX_PATH + 32, L"%s - %s", kAppTitle, base ? base + 1 : romPath);
    SetWindowTextW(g_hwnd, title); /* always the main window, even when
                                     * called with a dialog as `hwnd` */

    return 1;
}

/* Relaunches this same .exe with cmdLine as its *entire* command line (no
 * quoting/argv splitting on either end - WinMain treats a non-empty
 * lpCmdLine as exactly one ROM path verbatim, see WinMain below), then
 * lets the new process take over. Used by LoadRomFlow() instead of an
 * in-process reload whenever the dynarec JIT is active - see that call
 * site's comment for why. (round 20 briefly added a second caller, a
 * silent no-ROM "warm-up" restart from WinMain meant to work around a
 * display-corruption bug - hardware-tested and found not to help, see
 * the dev notes' round 21, so that caller was removed again; kept this
 * generalized on a plain cmdLine string rather than reverting the
 * rename, since a future similar need may come up again.) Returns 1 if
 * the new process was successfully started (the caller should tear
 * itself down right after, e.g. via CeShutdown()); 0 if CreateProcessW
 * itself failed, in which case nothing has changed yet (mutex still
 * held, no new process) and the caller should fall back to continuing
 * in this same process instead. */
static int RestartProcess(const wchar_t *cmdLine_in)
{
    wchar_t exePath[MAX_PATH];
    wchar_t cmdLine[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH))
    {
        CeLog("RestartProcess: GetModuleFileNameW failed, error=%lu", (unsigned long)GetLastError());
        return 0;
    }

    wcsncpy(cmdLine, cmdLine_in, MAX_PATH - 1);
    cmdLine[MAX_PATH - 1] = L'\0';

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    /* CREATE_SUSPENDED: the new process's main thread (and therefore its
     * own WinMain/CreateMutexW call) doesn't run until ResumeThread() is
     * called. That call is *not* made here: it's deferred to CeShutdown()
     * (see this process's caller, LoadRomFlow(), which always calls
     * CeShutdown() right after this function returns success), which
     * resumes it only after this process has torn itself down via
     * retro_deinit(). Historically (round 26, when the ~11MiB translation
     * caches were a runtime combined VirtualAlloc in the since-removed
     * CE/ce_dynarec_mmap.c) resuming immediately here let the new process
     * try to grab its own ~11MiB while this old process still held its
     * copy, intermittently starving it (logged: "combined VirtualAlloc
     * FAILED ... error=8" = ERROR_NOT_ENOUGH_MEMORY) and hanging the new
     * ROM a few frames in. The caches are now plain .bss static arrays
     * committed at process load, so that specific race is gone, but the
     * deferred-resume ordering is kept: it is harmless and still
     * guarantees the old process is fully down before the new one runs.
     *
     * The mutex release below still has to happen *before* the deferred
     * resume (not before this function returns) - the new instance's
     * WinMain single-instance check runs as soon as its thread resumes,
     * so it must never observe the mutex still held. CeShutdown() closes
     * g_mutex further down its own teardown, after this release already
     * ran and nulled it out, so that later close is a no-op - ordering
     * between the two is not a concern. */
    if (!CreateProcessW(exePath, cmdLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
    {
        CeLog("RestartProcess: CreateProcessW failed, error=%lu", (unsigned long)GetLastError());
        return 0;
    }

    if (g_mutex)
    {
        CloseHandle(g_mutex);
        g_mutex = NULL; /* CeShutdown() no-ops its own CloseHandle when this is NULL */
    }
    s_restartResumeThread  = pi.hThread;
    s_restartResumeProcess = pi.hProcess;
    return 1;
}

/* Picks a ROM (via PickRom) and hands its path to the core. Safe to call
 * both for the first load and for File>Open while a game is already
 * running (unloads the previous game first). Returns 1 on success, 0 if
 * the user cancelled the picker or loading failed - in both failure
 * cases whatever was running before is left untouched. */
static int LoadRomFlow(HWND hwnd)
{
    wchar_t romPath[MAX_PATH];

    if (!PickRom(hwnd, romPath, MAX_PATH))
        return 0; /* cancelled/failed - PickRom already logged why */

    if (g_romLoaded && CeVideoIsDynarecEnabled())
    {
        /* Switching ROMs in-process while the dynarec JIT is active is
         * unsafe on this device: real-hardware testing found it
         * reproducibly hangs/faults a few instructions into the new
         * ROM's reset_gba() on every second-or-later load, regardless of
         * which ROM or switch direction, and millisecond-timestamped
         * per-instruction checkpoints traced the stall to real
         * multi-second delays with no crash report - not a logic bug in
         * the (trivial, verified-correct) code at that point. Several
         * allocation-strategy fixes for the dynarec translation caches
         * (free+realloc, then reuse-without-freeing) were ruled out one
         * at a time, and the caches were ultimately switched to plain
         * .bss static arrays with no runtime allocation object at all -
         * the in-process second-load hang still reproduces, so the
         * residual state is definitively NOT memory-management-related.
         * Whatever state a session that has actually JIT-translated-and-
         * executed code leaves behind, a fresh process doesn't have it.
         * So: restart the whole app with the new ROM's
         * path instead of reloading in-process. Interpreter-mode
         * sessions (dynarec never ran) aren't affected by this bug at
         * all and keep using the cheaper in-process LoadRomPath() below. */
        if (RestartProcess(romPath))
        {
            CeLog("LoadRomFlow: restarting process for a dynarec-safe ROM switch");
            CeShutdown(0); /* never returns */
        }
        CeLog("LoadRomFlow: process restart failed, falling back to in-process reload");
        /* fall through - better to risk the known in-process issue than
         * to leave the user stuck on "Open ROM" doing nothing at all. */
    }

    return LoadRomPath(hwnd, romPath);
}

/* ------------------------------------------------------------------ */
/* Save state (single slot per ROM: "<romPath>.state")                */
/* ------------------------------------------------------------------ */

/* Returns 1 on success, 0 on failure. Shows no UI itself - the caller
 * (SaveStateDlgProc) folds the result into its own single dialog so no
 * second modal is ever nested (the dev notes round-22 glitch). */
static int CeSaveState(void)
{
    size_t size;
    void *buffer;
    wchar_t statePath[MAX_PATH + 8];
    FILE *f;

    size = retro_serialize_size();
    if (size == 0)
    {
        CeLog("CeSaveState: retro_serialize_size returned 0");
        return 0;
    }

    /* VirtualAlloc rather than malloc/heap: the dev notes' round-28
     * investigation flagged a plain heap malloc() of this size as the
     * leading suspect for "Save failed." reports that only happen with
     * the dynarec JIT active (its ~11MiB of translation caches - now .bss
     * static arrays, then a runtime combined VirtualAlloc - crowd this
     * process's 32MB address space). VirtualAlloc pulls
     * straight from the same address space the heap would use, but
     * doesn't fight the heap allocator's own bookkeeping/fragmentation,
     * and is freed immediately below regardless of outcome. */
    buffer = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer)
    {
        CeLog("CeSaveState: VirtualAlloc(%lu) failed (error=%lu)",
              (unsigned long)size, (unsigned long)GetLastError());
        return 0;
    }

    if (!retro_serialize(buffer, size))
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        CeLog("CeSaveState: retro_serialize failed");
        return 0;
    }

    _snwprintf(statePath, MAX_PATH + 8, L"%s.state", g_romPath);
    f = _wfopen(statePath, L"wb");
    if (!f)
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        CeLog("CeSaveState: failed to open state file for write (error=%lu)",
              (unsigned long)GetLastError());
        return 0;
    }

    fwrite(buffer, 1, size, f);
    fclose(f);
    VirtualFree(buffer, 0, MEM_RELEASE);
    CeLog("CeSaveState: saved %lu bytes", (unsigned long)size);
    return 1;
}

static int CeLoadState(HWND owner)
{
    wchar_t statePath[MAX_PATH + 8];
    FILE *f;
    long size;
    void *buffer;

    _snwprintf(statePath, MAX_PATH + 8, L"%s.state", g_romPath);
    f = _wfopen(statePath, L"rb");
    if (!f)
    {
        CeLog("CeLoadState: no state file found");
        CeShowMsgBox(owner, CeLangIsJapanese()
                     ? L"\x30bb\x30fc\x30d6\x30c7\x30fc\x30bf\x304c\x3042\x308a\x307e\x305b\x3093\x3002" /* セーブデータがありません。 */
                     : L"No save state found.");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0)
    {
        fclose(f);
        CeLog("CeLoadState: empty/unreadable state file");
        CeShowMsgBox(owner, CeLangIsJapanese()
                     ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                     : L"Load failed.");
        return 0;
    }

    /* VirtualAlloc, not malloc - see CeSaveState()'s comment above. */
    buffer = VirtualAlloc(NULL, (size_t)size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer)
    {
        fclose(f);
        CeLog("CeLoadState: VirtualAlloc(%ld) failed (error=%lu)",
              size, (unsigned long)GetLastError());
        CeShowMsgBox(owner, CeLangIsJapanese()
                     ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                     : L"Load failed.");
        return 0;
    }

    if (fread(buffer, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        VirtualFree(buffer, 0, MEM_RELEASE);
        CeLog("CeLoadState: short read");
        CeShowMsgBox(owner, CeLangIsJapanese()
                     ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                     : L"Load failed.");
        return 0;
    }
    fclose(f);

    if (!retro_unserialize(buffer, (size_t)size))
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        CeLog("CeLoadState: retro_unserialize failed");
        CeShowMsgBox(owner, CeLangIsJapanese()
                     ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\xff08\x975e\x4e92\x63db\x306e\x30bb\x30fc\x30d6\x30c7\x30fc\x30bf\xff09" /* ロードに失敗しました（非互換のセーブデータ） */
                     : L"Load failed (incompatible save?).");
        return 0;
    }

    VirtualFree(buffer, 0, MEM_RELEASE);
    CeLog("CeLoadState: loaded %ld bytes", size);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Menu (touch-to-reveal - hidden during gameplay, shown at startup     */
/* before any ROM is loaded and whenever the screen is tapped mid-game) */
/*                                                                      */
/* Implemented as a modal DialogBoxW (CE/ce_res.rc's IDD_MAINMENU) with */
/* plain PUSHBUTTON controls, not a real HMENU - see ce_resource.h for  */
/* why (this coredll doesn't export SetMenu).                          */
/* ------------------------------------------------------------------ */

static HINSTANCE g_hInstance = NULL;

/* ------------------------------------------------------------------ */
/* Japanese-capable message box / confirmation (IDD_MSGBOX/IDD_CONFIRM) */
/* ------------------------------------------------------------------ */

/* See ce_res.rc's IDD_MSGBOX comment: MessageBoxW() draws with whatever
 * system font Windows CE finds, and this device has no CJK-capable one
 * any more (jptahoma.ttc removed - see ce_lang.h), so Japanese text
 * through it comes back as tofu. These two little dialogs paint their
 * own text with the Shinonome bitmap font (ce_bmpfont.c) instead, the
 * same way every other piece of Japanese UI text in this port now does.
 * Ported from the sister PopGB / PopSNES projects. */
#define CE_MSGBOX_MAX_TEXT  256
#define CE_MSGBOX_MAX_LINE  64
#define CE_MSGBOX_MAX_LINES 4
static wchar_t s_msgBoxText[CE_MSGBOX_MAX_TEXT];

/* Greedily packs `text` onto at most `maxLines` lines that each fit
 * `maxWidth` real pixels, returning the line count used. When a line has
 * an ASCII space it breaks there (English wraps on word boundaries);
 * Japanese has no spaces so it just fills each line with as many
 * characters as fit. At least one character is kept per line even if it
 * alone overflows; anything past the last line is dropped. */
static int WrapMsgBoxLines(const wchar_t *text, int maxWidth,
                           wchar_t lines[][CE_MSGBOX_MAX_LINE], int maxLines)
{
    int n = (int)wcslen(text);
    int pos = 0, count = 0;
    wchar_t probe[CE_MSGBOX_MAX_LINE];

    while (pos < n && count < maxLines)
    {
        int take = 0, lastSpace = 0, i;

        for (i = 1; pos + i <= n && i < CE_MSGBOX_MAX_LINE - 1; i++)
        {
            wcsncpy(probe, text + pos, i);
            probe[i] = L'\0';
            if (CeBmpFontGetTextWidth(probe) > maxWidth)
                break;
            take = i;
            if (text[pos + i - 1] == L' ')
                lastSpace = i;
        }
        if (take == 0)
            take = 1; /* force progress on a single overflowing glyph */
        if (pos + take < n && lastSpace > 0 && count < maxLines - 1)
            take = lastSpace; /* break on the word boundary, not mid-word */

        wcsncpy(lines[count], text + pos, take);
        lines[count][take] = L'\0';
        count++;
        pos += take;
        while (pos < n && text[pos] == L' ')
            pos++; /* swallow the space we broke on */
    }
    return count;
}

/* Shared WM_PAINT body for both dialogs: wraps s_msgBoxText against the
 * hidden LTEXT's real (post-creation, pixel) rect and centres up to
 * CE_MSGBOX_MAX_LINES Shinonome-font lines in it. */
static void MsgTextPaint(HWND hDlg, int textCtrlId)
{
    PAINTSTRUCT ps;
    HDC hdc;
    RECT rc;
    wchar_t lines[CE_MSGBOX_MAX_LINES][CE_MSGBOX_MAX_LINE];
    COLORREF fg;
    int rectW, rectH, count, lineH, totalH, y, i;

    hdc = BeginPaint(hDlg, &ps);
    GetWindowRect(GetDlgItem(hDlg, textCtrlId), &rc);
    MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);
    rectW = rc.right - rc.left;
    rectH = rc.bottom - rc.top;

    count = WrapMsgBoxLines(s_msgBoxText, rectW, lines, CE_MSGBOX_MAX_LINES);

    fg = GetSysColor(COLOR_WINDOWTEXT);
    SetBkMode(hdc, TRANSPARENT);

    lineH  = CE_BMPFONT_HEIGHT + 2;
    totalH = count > 0 ? (count - 1) * lineH + CE_BMPFONT_HEIGHT : 0;
    y = rc.top + (rectH - totalH) / 2;
    if (y < rc.top)
        y = rc.top; /* tall block: pin to the top rather than clip above the rect */

    for (i = 0; i < count; i++)
    {
        int x = rc.left + (rectW - CeBmpFontGetTextWidth(lines[i])) / 2;
        CeBmpFontDrawTextW(hdc, x, y + i * lineH, lines[i], fg);
    }

    EndPaint(hDlg, &ps);
}

/* BS_OWNERDRAW breaks IsDialogMessage()'s normal DEFPUSHBUTTON Enter
 * routing and Escape-to-Cancel handling, so every owner-draw button in
 * this port that needs physical-key support reimplements it by hand -
 * see MainMenuBtnCtrlProc below and SoundCtrlProc/VideoCtrlProc/
 * InputBtnCtrlProc in the other files. */
static WNDPROC s_pMsgBoxOkOrigProc = NULL;

static LRESULT CALLBACK MsgBoxBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            return 0;
        case VK_ESCAPE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }
    return CallWindowProc(s_pMsgBoxOkOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MsgBoxDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        ShowWindow(GetDlgItem(hDlg, IDC_MB_TEXT), SW_HIDE);
        s_pMsgBoxOkOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC, (LONG_PTR)MsgBoxBtnCtrlProc);
        return TRUE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
        MsgTextPaint(hDlg, IDC_MB_TEXT);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

static WNDPROC s_pConfirmBtnOrigProc = NULL;

static LRESULT CALLBACK ConfirmBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;
        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_CF_NO, 0), (LPARAM)hWnd);
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            SetFocus(GetDlgItem(hDlg, id == IDC_CF_YES ? IDC_CF_NO : IDC_CF_YES));
            return 0;
        }
    }
    return CallWindowProc(s_pConfirmBtnOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK ConfirmDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        ShowWindow(GetDlgItem(hDlg, IDC_CF_TEXT), SW_HIDE);
        SetDlgItemTextW(hDlg, IDC_CF_YES, CeLangIsJapanese() ? L"\x306f\x3044"       /* はい */ : L"Yes");
        SetDlgItemTextW(hDlg, IDC_CF_NO,  CeLangIsJapanese() ? L"\x3044\x3044\x3048" /* いいえ */ : L"No");
        s_pConfirmBtnOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC, (LONG_PTR)ConfirmBtnCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_NO),  GWLP_WNDPROC, (LONG_PTR)ConfirmBtnCtrlProc);
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_CF_YES));
        return FALSE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
        MsgTextPaint(hDlg, IDC_CF_TEXT);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CF_YES:
            EndDialog(hDlg, 1);
            return TRUE;
        case IDC_CF_NO:
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

static void CeShowMsgBox(HWND owner, const wchar_t *text)
{
    wcsncpy(s_msgBoxText, text, CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';
    DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_MSGBOX), owner, MsgBoxDlgProc);
}

/* ------------------------------------------------------------------ */
/* Save State: ask + run + acknowledge, all in one self-drawn dialog  */
/* ------------------------------------------------------------------ */

/* The old Save State path opened IDD_CONFIRM, closed it, then opened
 * IDD_MSGBOX - two modal dialogs back-to-back under the main menu. On
 * this device's GWES that hand-off exposes the main menu for a frame and
 * its owner-draw "ステートセーブ" button repaints to the front (the same
 * nested-modal composition glitch the dev notes' round 22 fought on the
 * ROM-picker path). This reuses the IDD_CONFIRM template but never
 * closes/reopens: on Yes it runs the save in place and morphs its own
 * text/buttons into the result acknowledgement. */
static WNDPROC s_pSsBtnOrigProc = NULL;
static int     s_ssPhase        = 0; /* 0 = asking, 1 = showing result */

static LRESULT CALLBACK SsCfBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int  id   = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;
        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if (s_ssPhase == 0) /* only the ask phase has two buttons */
                SetFocus(GetDlgItem(hDlg, id == IDC_CF_YES ? IDC_CF_NO : IDC_CF_YES));
            return 0;
        }
    }
    return CallWindowProc(s_pSsBtnOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK SaveStateDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        s_ssPhase = 0;
        ShowWindow(GetDlgItem(hDlg, IDC_CF_TEXT), SW_HIDE);
        SetDlgItemTextW(hDlg, IDC_CF_YES, CeLangIsJapanese() ? L"\x306f\x3044"       /* はい */ : L"Yes");
        SetDlgItemTextW(hDlg, IDC_CF_NO,  CeLangIsJapanese() ? L"\x3044\x3044\x3048" /* いいえ */ : L"No");
        s_pSsBtnOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC, (LONG_PTR)SsCfBtnCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_NO),  GWLP_WNDPROC, (LONG_PTR)SsCfBtnCtrlProc);
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_CF_YES));
        return FALSE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
        MsgTextPaint(hDlg, IDC_CF_TEXT);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CF_YES:
            if (s_ssPhase == 0)
            {
                int ok = CeSaveState();
                wcsncpy(s_msgBoxText, CeLangIsJapanese()
                        ? (ok ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3057\x305f\x3002"                     /* セーブしました。 */
                              : L"\x30bb\x30fc\x30d6\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002")  /* セーブに失敗しました。 */
                        : (ok ? L"State saved." : L"Save failed."),
                        CE_MSGBOX_MAX_TEXT - 1);
                s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';
                s_ssPhase = 1;
                ShowWindow(GetDlgItem(hDlg, IDC_CF_NO), SW_HIDE);
                SetDlgItemTextW(hDlg, IDC_CF_YES, L"OK");
                {   /* recentre the lone OK button */
                    RECT rc, rb;
                    GetClientRect(hDlg, &rc);
                    GetWindowRect(GetDlgItem(hDlg, IDC_CF_YES), &rb);
                    MapWindowPoints(NULL, hDlg, (POINT *)&rb, 2);
                    SetWindowPos(GetDlgItem(hDlg, IDC_CF_YES), NULL,
                                 (rc.right - (rb.right - rb.left)) / 2, rb.top,
                                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                }
                InvalidateRect(hDlg, NULL, TRUE);
                SetFocus(GetDlgItem(hDlg, IDC_CF_YES));
            }
            else
            {
                EndDialog(hDlg, 1);
            }
            return TRUE;

        case IDC_CF_NO:
        case IDCANCEL:
            EndDialog(hDlg, s_ssPhase ? 1 : 0);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

static void CeConfirmAndSaveState(HWND owner)
{
    wcsncpy(s_msgBoxText, CeLangIsJapanese()
            ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3059\x304b\xff1f" /* セーブしますか？ */
            : L"Save state?",
            CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';
    DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_CONFIRM), owner, SaveStateDlgProc);
}

/* ------------------------------------------------------------------ */
/* Main menu dialog                                                    */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Screenshot (ported from the sister PopGB / PopSG / PopSNES)         */
/* ------------------------------------------------------------------ */

static void PutLE16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void PutLE32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Saves the paused game image as a 16-bit RGB565 BMP (BI_BITFIELDS -
 * the DIB section's own pixel format, so no conversion: one fwrite per
 * row). Uses the on-screen image at the current Scale (x1/x2, from
 * ce_gapi.c's DIB via GapiGetLastImage()) so the file matches what the
 * user sees. Written to "<exe-dir>\Screenshots\<ROM name>_NNN.bmp",
 * creating the folder on first use and taking the first unused number.
 * The header is built byte by byte rather than from BITMAPFILEHEADER so
 * struct packing can't shift it. _wfopen() directly (wide path, so
 * Japanese folder/ROM names are safe - the compat/stdio.h fopen()
 * redirect is only for the core's narrow UTF-8 paths). Returns 1 on
 * success. */
static int CeSaveScreenshot(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t romName[MAX_PATH];
    wchar_t path[MAX_PATH + 16];
    wchar_t *p;
    const wchar_t *base;
    unsigned n, y, rowBytes, padBytes;
    unsigned long imageBytes;
    unsigned char hdr[66];
    static const unsigned char pad[4] = { 0, 0, 0, 0 };
    const void *img;
    unsigned imgW, imgH, imgPitch;
    FILE *f;

    if (!GapiGetLastImage(&img, &imgW, &imgH, &imgPitch))
    {
        CeLog("CeSaveScreenshot: no rendered frame yet");
        return 0;
    }

    if (!GetModuleFileNameW(NULL, dir, MAX_PATH))
        return 0;
    p = wcsrchr(dir, L'\\');
    if (!p)
        return 0;
    p[1] = L'\0';
    if (wcslen(dir) + 12 >= MAX_PATH)
        return 0;
    wcscat(dir, L"Screenshots");
    CreateDirectoryW(dir, NULL); /* already existing is fine */
    if (GetFileAttributesW(dir) == 0xFFFFFFFF)
    {
        CeLog("CeSaveScreenshot: can't create Screenshots folder");
        return 0;
    }

    base = wcsrchr(g_romPath, L'\\');
    wcsncpy(romName, base ? base + 1 : g_romPath, MAX_PATH - 1);
    romName[MAX_PATH - 1] = L'\0';
    p = wcsrchr(romName, L'.');
    if (p)
        *p = L'\0';
    if (!romName[0])
        wcscpy(romName, L"PopGBA");

    for (n = 1; n <= 999; n++)
    {
        _snwprintf(path, MAX_PATH + 16, L"%s\\%s_%03u.bmp", dir, romName, n);
        path[MAX_PATH + 15] = L'\0';
        if (GetFileAttributesW(path) == 0xFFFFFFFF)
            break;
    }
    if (n > 999)
    {
        CeLog("CeSaveScreenshot: all 999 numbers used");
        return 0;
    }

    rowBytes = imgW * 2;
    padBytes = (4 - (rowBytes & 3)) & 3;
    imageBytes = (unsigned long)(rowBytes + padBytes) * imgH;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    PutLE32(hdr + 2, sizeof(hdr) + imageBytes);  /* file size */
    PutLE32(hdr + 10, sizeof(hdr));              /* pixel data offset */
    PutLE32(hdr + 14, 40);                       /* BITMAPINFOHEADER size */
    PutLE32(hdr + 18, imgW);
    PutLE32(hdr + 22, imgH);                     /* positive = bottom-up */
    PutLE16(hdr + 26, 1);                        /* planes */
    PutLE16(hdr + 28, 16);                       /* bits per pixel */
    PutLE32(hdr + 30, 3);                        /* BI_BITFIELDS */
    PutLE32(hdr + 34, imageBytes);
    PutLE32(hdr + 38, 2835);                     /* 72 dpi */
    PutLE32(hdr + 42, 2835);
    PutLE32(hdr + 54, 0xF800);                   /* R mask */
    PutLE32(hdr + 58, 0x07E0);                   /* G mask */
    PutLE32(hdr + 62, 0x001F);                   /* B mask */

    f = _wfopen(path, L"wb");
    if (!f)
    {
        CeLog("CeSaveScreenshot: can't open output file");
        return 0;
    }
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = imgH; y-- > 0; )
    {
        fwrite((const unsigned char *)img + (size_t)y * imgPitch, 1, rowBytes, f);
        if (padBytes)
            fwrite(pad, 1, padBytes, f);
    }
    if (ferror(f))
    {
        fclose(f);
        DeleteFileW(path);
        CeLog("CeSaveScreenshot: write failed");
        return 0;
    }
    fclose(f);

    CeLog("CeSaveScreenshot: saved %ux%u as #%03u", imgW, imgH, n);
    return 1;
}

static void CeScreenshotAndReport(HWND owner)
{
    if (CeSaveScreenshot())
        CeShowMsgBox(owner, CeLangIsJapanese()
            ? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x3092\x4fdd\x5b58\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットを保存しました。 */
            : L"Screenshot saved.");
    else
        CeShowMsgBox(owner, CeLangIsJapanese()
            ? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x306e\x4fdd\x5b58\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットの保存に失敗しました。 */
            : L"Screenshot failed.");
}

/* Swaps every IDD_MAINMENU caption between English (the .rc template's
 * own text) and Japanese, gated by CeLangIsJapanese() - see ce_lang.h.
 * Called from WM_INITDIALOG and again right after Video Config returns
 * (that's where the toggle lives). The buttons are BS_OWNERDRAW and
 * repaint themselves from the text just set (WM_DRAWITEM ->
 * CeBmpFontDrawOwnerButtonTheme); IDC_MM_HINT is a plain LTEXT (no ownerdraw
 * style exists for STATIC), so it's hidden here and repainted by
 * WM_PAINT via CeBmpFontPaintLabel(). The title bar is never touched -
 * non-client area the OS paints with its own font, which the bitmap
 * font can't reach (renders as tofu on this device). */
static void ApplyMainMenuLanguage(HWND hDlg)
{
    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"ROM\x3092\x958b\x304f...");                             /* ROMを開く... */
        SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"\x30b9\x30c6\x30fc\x30c8\x30bb\x30fc\x30d6");            /* ステートセーブ */
        SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"\x30b9\x30c6\x30fc\x30c8\x30ed\x30fc\x30c9");            /* ステートロード */
        SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"\x30dc\x30bf\x30f3\x8a2d\x5b9a");                        /* ボタン設定 */
        SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"\x30b5\x30a6\x30f3\x30c9\x8a2d\x5b9a");                  /* サウンド設定 */
        SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"\x753b\x9762\x8a2d\x5b9a");                              /* 画面設定 */
        SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"\x753b\x9762\x4fdd\x5b58\x3059\x308b");         /* 画面保存する */
        SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"\x7d42\x4e86");                                          /* 終了 */
        SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"\x623b\x308b\x30ad\x30fc\x3067\x30b2\x30fc\x30e0\x518d\x958b"); /* 戻るキーでゲーム再開 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"Open ROM...");
        SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"Save State");
        SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"Load State");
        SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"Input Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"Sound Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"Video Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"Screenshot");
        SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"Exit");
        SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"Press Back to resume the game.");
    }

    ShowWindow(GetDlgItem(hDlg, IDC_MM_HINT), SW_HIDE);
    /* Full erase+redraw so the hint label's transparent Shinonome
     * repaint doesn't stack old glyph bits under the new-language text
     * (CeBmpFontPaintLabel() never clears its rect first). */
    InvalidateRect(hDlg, NULL, TRUE);
}

/* Reading order matching ce_res.rc's IDD_MAINMENU layout - also the
 * wraparound order for MainMenuNeighbor()'s arrow-key cycling. */
static const int kMainMenuButtonIds[] = {
    IDC_MM_OPEN, IDC_MM_SAVESTATE, IDC_MM_LOADSTATE,
    IDC_MM_VIDEO, IDC_MM_SOUND, IDC_MM_INPUT, IDC_MM_SCREENSHOT, IDC_MM_EXIT,
};

/* Pastel/rounded main-menu skin, ported as-is from the sister PopGB
 * (PopGB, itself from PopSG) - pastel fills with darker borders, a
 * deep-ivory window background, per-button vector icons, blue/white
 * reverse video on focus; see CeBmpFontDrawOwnerButtonTheme() in
 * ce_bmpfont.c. Applies only to this dialog's own buttons via
 * WM_DRAWITEM and its own client background via WM_ERASEBKGND - every
 * other dialog keeps the plain gray CeBmpFontDrawOwnerButton() look. */
#define CE_MENU_BG_CREAM   RGB(0xF0, 0xE1, 0xBC)
#define CE_MENU_TEXT_DARK  RGB(0x2A, 0x2C, 0x30)

typedef struct { int id; COLORREF bg, border; CeMenuIcon icon; int stacked; } CeMenuButtonTheme;

static const CeMenuButtonTheme kMainMenuTheme[] = {
    { IDC_MM_OPEN,       RGB(0x6F, 0xA8, 0xDC), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_OPEN,       0 }, /* blue */
    { IDC_MM_SAVESTATE,  RGB(0xF5, 0xEC, 0x9E), RGB(0x6E, 0x66, 0x12), CE_MENU_ICON_SAVE,       0 }, /* pastel lemon */
    { IDC_MM_LOADSTATE,  RGB(0xBF, 0xE3, 0xD0), RGB(0x1D, 0x5A, 0x3C), CE_MENU_ICON_LOAD,       0 }, /* mint */
    { IDC_MM_VIDEO,      RGB(0xC9, 0xE4, 0xB0), RGB(0x3C, 0x5A, 0x1B), CE_MENU_ICON_VIDEO,      1 }, /* green */
    { IDC_MM_SOUND,      RGB(0xB9, 0xD7, 0xEE), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_SOUND,      1 }, /* blue */
    { IDC_MM_INPUT,      RGB(0xF2, 0xB8, 0xC6), RGB(0x7A, 0x2E, 0x4C), CE_MENU_ICON_INPUT,      1 }, /* pink */
    { IDC_MM_SCREENSHOT, RGB(0xD9, 0xCC, 0xF0), RGB(0x4E, 0x34, 0x80), CE_MENU_ICON_SCREENSHOT, 0 }, /* lavender */
    { IDC_MM_EXIT,       RGB(0xF0, 0xA9, 0xA0), RGB(0x7A, 0x23, 0x18), CE_MENU_ICON_EXIT,       0 }, /* coral */
};

/* CE/icon/popgba_mascot.bmp, embedded as IDB_MAINMENU (ce_res.rc). Loaded
 * lazily in WM_INITDIALOG, blitted by WM_PAINT into the blank strip
 * right of the IDC_MM_HINT text, freed in WM_DESTROY. */
static HBITMAP s_hMainMenuBmp = NULL;
#define CE_MAINMENU_BUTTON_COUNT (sizeof(kMainMenuButtonIds) / sizeof(kMainMenuButtonIds[0]))

/* Skips disabled buttons (Save/Load State while no ROM is loaded). */
static int MainMenuNeighbor(HWND hDlg, int id, int delta)
{
    int idx, step;
    for (idx = 0; idx < (int)CE_MAINMENU_BUTTON_COUNT; idx++)
        if (kMainMenuButtonIds[idx] == id)
            break;
    if (idx >= (int)CE_MAINMENU_BUTTON_COUNT)
        return id;

    for (step = 1; step <= (int)CE_MAINMENU_BUTTON_COUNT; step++)
    {
        int nextIdx = ((idx + delta * step) % (int)CE_MAINMENU_BUTTON_COUNT
                       + (int)CE_MAINMENU_BUTTON_COUNT) % (int)CE_MAINMENU_BUTTON_COUNT;
        int nextId = kMainMenuButtonIds[nextIdx];
        if (IsWindowEnabled(GetDlgItem(hDlg, nextId)))
            return nextId;
    }
    return id;
}

/* Once the IDD_MAINMENU buttons became BS_OWNERDRAW the built-in
 * WM_KEYDOWN -> BN_CLICKED conversion for the physical decide key stopped
 * firing (touch/stylus still works). IDD_MAINMENU has no DEFPUSHBUTTON
 * for IsDialogMessage() to fall back on, so this subclass claims
 * WM_GETDLGCODE outright and does its own arrow-key focus cycling +
 * decide/Back handling - the same tradeoff SoundCtrlProc/VideoCtrlProc/
 * InputBtnCtrlProc already make in the other dialogs. */
static WNDPROC s_pMainMenuOrigProc = NULL;

static LRESULT CALLBACK MainMenuBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_UP:
        case VK_LEFT:
            SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, -1)));
            return 0;
        case VK_DOWN:
        case VK_RIGHT:
            SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, 1)));
            return 0;
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;
        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }
    return CallWindowProc(s_pMainMenuOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MainMenuDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        unsigned i;
        EnableWindow(GetDlgItem(hDlg, IDC_MM_SAVESTATE), g_romLoaded);
        EnableWindow(GetDlgItem(hDlg, IDC_MM_LOADSTATE), g_romLoaded);
        EnableWindow(GetDlgItem(hDlg, IDC_MM_SCREENSHOT), g_romLoaded);
        ApplyMainMenuLanguage(hDlg);

        s_pMainMenuOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_MM_OPEN), GWLP_WNDPROC);
        for (i = 0; i < CE_MAINMENU_BUTTON_COUNT; i++)
            SetWindowLongPtrW(GetDlgItem(hDlg, kMainMenuButtonIds[i]), GWLP_WNDPROC, (LONG_PTR)MainMenuBtnCtrlProc);

        if (!s_hMainMenuBmp)
        {
            s_hMainMenuBmp = LoadBitmapW(g_hInstance, MAKEINTRESOURCEW(IDB_MAINMENU));
            if (!s_hMainMenuBmp)
                CeLog("MainMenu: LoadBitmapW(IDB_MAINMENU) failed, err=%lu", (unsigned long)GetLastError());
        }
        return TRUE;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        unsigned i;
        for (i = 0; i < sizeof(kMainMenuTheme) / sizeof(kMainMenuTheme[0]); i++)
        {
            if (kMainMenuTheme[i].id == (int)dis->CtlID)
            {
                CeBmpFontDrawOwnerButtonTheme(dis, kMainMenuTheme[i].bg, kMainMenuTheme[i].border, CE_MENU_TEXT_DARK,
                                              kMainMenuTheme[i].icon, kMainMenuTheme[i].stacked, CE_MENU_BG_CREAM);
                return TRUE;
            }
        }
        CeBmpFontDrawOwnerButton(dis); /* fallback, shouldn't hit any control here */
        return TRUE;
    }

    /* Cream client background for the pastel skin - painted here rather
     * than via a class brush so it's scoped to this dialog. Created once
     * and kept for the process lifetime. */
    case WM_ERASEBKGND:
    {
        static HBRUSH s_hCreamBrush = NULL;
        RECT rc;
        if (!s_hCreamBrush)
            s_hCreamBrush = CreateSolidBrush(CE_MENU_BG_CREAM);
        GetClientRect(hDlg, &rc);
        FillRect((HDC)wParam, &rc, s_hCreamBrush);
        return TRUE;
    }

    case WM_DESTROY:
        if (s_hMainMenuBmp)
        {
            DeleteObject(s_hMainMenuBmp);
            s_hMainMenuBmp = NULL;
        }
        return FALSE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        CeBmpFontPaintLabel(hdc, hDlg, IDC_MM_HINT);

        /* popgba_mascot.bmp (IDB_MAINMENU) in the blank strip to the
         * right of the hint text - starts just past the Shinonome text
         * width and runs to the dialog's right/bottom edge, aspect ratio
         * preserved, bottom-right aligned (same as PopGB / PopSG). */
        if (s_hMainMenuBmp)
        {
            HWND hHint = GetDlgItem(hDlg, IDC_MM_HINT);
            RECT rcHint, rcClient;
            wchar_t hintText[128];
            BITMAP bm;

            hintText[0] = 0;
            GetWindowTextW(hHint, hintText, 128);
            GetWindowRect(hHint, &rcHint);
            MapWindowPoints(NULL, hDlg, (POINT *)&rcHint, 2);
            GetClientRect(hDlg, &rcClient);

            if (GetObject(s_hMainMenuBmp, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0)
            {
                long boxL = rcHint.left + CeBmpFontGetTextWidth(hintText) + 8;
                long boxR = rcClient.right - 4;
                long boxT = rcHint.top;
                long boxB = rcClient.bottom - 2;
                long boxW = boxR - boxL;
                long boxH = boxB - boxT;

                if (boxW > 8 && boxH > 8)
                {
                    long drawW = boxW;
                    long drawH = drawW * bm.bmHeight / bm.bmWidth;
                    HDC memDC;
                    HGDIOBJ oldBmp;

                    if (drawH > boxH)
                    {
                        drawH = boxH;
                        drawW = drawH * bm.bmWidth / bm.bmHeight;
                    }

                    /* No SetStretchBltMode() - this coredll doesn't
                     * export it; CE's default is fine for this shrink. */
                    memDC = CreateCompatibleDC(hdc);
                    oldBmp = SelectObject(memDC, s_hMainMenuBmp);
                    StretchBlt(hdc, (int)(boxR - drawW), (int)(boxB - drawH),
                               (int)drawW, (int)drawH,
                               memDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(memDC, oldBmp);
                    DeleteDC(memDC);
                }
            }
        }

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_MM_OPEN:
            /* round 22: closes this dialog *before* opening the ROM
             * picker - nesting Select ROM on top of this still-live
             * dialog garbles the screen on the first Open ROM after
             * power-on (rounds 17-21; round 65 retried nesting and it
             * came straight back). Round 66: so the menu doesn't vanish
             * from behind the picker, grab its pixels first and have the
             * main window paint that picture while the picker is up (see
             * CeMenuSnapTake() / WndProc's WM_PAINT). */
            CeMenuSnapTake(hDlg);
            EndDialog(hDlg, IDC_MM_OPEN);
            return TRUE;

        case IDC_MM_EXIT:
            CeShutdown(0); /* never returns */
            return TRUE;

        case IDC_MM_SAVESTATE:
            /* User request: confirm before overwriting the single save
             * slot, then acknowledge the result - all in one self-drawn
             * dialog (never a nested second modal; see SaveStateDlgProc).
             * "No" just leaves the menu open. */
            if (g_romLoaded)
                CeConfirmAndSaveState(hDlg);
            return TRUE; /* stays open either way, like Input/Sound Config */

        case IDC_MM_LOADSTATE:
            /* Closes and resumes on success (like Resume Game) so the
             * user immediately sees the loaded state; stays open on
             * failure (CeLoadState already showed why). */
            if (g_romLoaded && CeLoadState(hDlg))
                EndDialog(hDlg, IDC_MM_LOADSTATE);
            return TRUE;

        case IDC_MM_SCREENSHOT:
            if (g_romLoaded)
                CeScreenshotAndReport(hDlg);
            return TRUE; /* stays open, like PopGB / PopSG */

        case IDC_MM_INPUT:
            CeShowInputConfigDialog(hDlg);
            return TRUE;

        case IDC_MM_SOUND:
            CeShowSoundConfigDialog(hDlg);
            return TRUE;

        case IDC_MM_VIDEO:
            CeShowVideoConfigDialog(hDlg);
            /* Video Config is where the Japanese/English toggle lives -
             * re-apply here so switching it and returning to this
             * still-open menu updates it immediately. */
            ApplyMainMenuLanguage(hDlg);
            return TRUE;

        case IDCANCEL:
            /* Hardware Back / OS close gesture: same as Resume if a
             * game is already running (nothing to lose by dismissing),
             * otherwise ignored - there's nothing to go back to yet. */
            if (g_romLoaded)
                EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

/* Pauses (if a game is running), shows the menu dialog modally (blocks
 * until closed), then resumes if a game is loaded when it returns -
 * whether that's the game that was already running, or one just picked
 * via Open ROM. Also how the very first "no ROM loaded" screen is shown
 * from WinMain, where wasPlaying is simply false. */
static void ShowMainMenuDialog(HWND hwnd)
{
    int wasPlaying = g_romLoaded && !g_paused;

    if (wasPlaying)
    {
        g_paused = 1;
        CeAudioSetPaused(1);
        GapiShutdown();

        /* Autosave SRAM at every pause, not just at graceful shutdown/
         * ROM switch - a real power-off doesn't run CeShutdown() at all,
         * so relying only on those two checkpoints misses that case
         * entirely (lesson from the sister PopSNES port's round 9).
         * Opening the touch-to-reveal menu is a frequent, cheap, natural
         * checkpoint to also save at. Writes only if the game changed
         * its SRAM while it has no .srm yet (CeSaveSramCheckpoint). */
        CeSaveSramCheckpoint();
    }

    /* Only forces a repaint here when there's no ROM loaded yet (the
     * initial "No ROM loaded" screen, which does need its black
     * background painted once). While a game is paused for the menu the
     * DIB section still holds the last frame, and WM_PAINT re-blits it
     * via GapiForceRepaint() as the menu/sub-dialogs expose the game
     * window - so nothing to force here. */
    if (!g_romLoaded)
        InvalidateRect(hwnd, NULL, TRUE);

    /* round 22: loops instead of a single DialogBoxW call - see
     * IDC_MM_OPEN's comment (MainMenuDlgProc, above) for why Open ROM
     * closes this dialog first rather than nesting Select ROM on top of
     * it. A cancelled pick or a failed load re-opens a *fresh* instance
     * of this same menu, so two modal dialogs are never live at once. */
    for (;;)
    {
        INT_PTR result = DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_MAINMENU), hwnd, MainMenuDlgProc);
        int loaded;

        if (result != IDC_MM_OPEN)
            break; /* Load State / Resume(Back) / Exit(never returns) - menu is done */

        loaded = LoadRomFlow(hwnd);
        CeMenuSnapFree(hwnd); /* picker is closed - drop the menu picture */
        if (loaded)
            break; /* loaded - resume/start gameplay below */

        /* Cancelled the picker, or the load failed - loop back to a
         * fresh main menu. */
    }

    if (g_romLoaded)
    {
        g_paused = 0;
        CeAudioSetPaused(0);
        if (!GapiInit(hwnd))
            CeLog("ShowMainMenuDialog: GapiInit failed - continuing without video output");

        /* Re-assert taskbar hiding every time gameplay is (re-)entered,
         * not just once at WinMain startup - the sister PopSNES
         * port found this necessary on this device (see that project's
         * ce_main.c comment): a custom DialogBoxW becoming the
         * foreground window (this menu) may cause the shell to restore
         * the taskbar, so it needs re-hiding every time control returns
         * from the menu dialog to gameplay. */
        CeHideShellChrome(hwnd);
    }
}

/* ------------------------------------------------------------------ */
/* Window / shutdown                                                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        g_running = 0;
        PostQuitMessage(0);
        return 0;

    case WM_LBUTTONDOWN:
        /* Touch-to-reveal, same interaction both prior CE ports use.
         * ShowMainMenuDialog() is modal, so this can't re-enter while
         * already showing (input goes to the dialog, not this window). */
        ShowMainMenuDialog(hwnd);
        return 0;

    case WM_PAINT:
    {
        /* The GDI video backend (ce_gapi.c) draws into this window's own
         * client area, so - unlike the old GAPI fullscreen surface, which
         * bypassed window painting entirely - it IS subject to normal
         * invalidation/repaint. No ROM yet: just the black "No ROM
         * loaded" background. ROM loaded (a modal menu / sub-dialog
         * closing over the paused game exposes part of the game window):
         * re-blit the last frame plus its letterbox border from the DIB
         * section via GapiForceRepaint(). */
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!g_romLoaded)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        if (g_romLoaded)
            GapiForceRepaint();
        CeMenuSnapPaint(hwnd); /* main-menu picture behind the ROM picker, if any */
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void CeShutdown(int exitCode)
{
    /* Lesson from both prior CE ports on this device (see either
     * project's dev notes - snes9x2002's is the more thoroughly
     * documented of the two): a plain `return` from WinMain lets the
     * normal exit path run, which on this device/toolchain combination
     * can itself crash or hang threads mid-teardown. Always terminate
     * via ExitProcess, called directly from here (not via WM_CLOSE/
     * WM_DESTROY/PostQuitMessage, which an earlier prototype traced a real
     * hang to on this device). CeAudioStop() joins the audio thread
     * cleanly before we ever get here, so ExitProcess() isn't tearing
     * down a thread still mid-waveOutWrite. */
    CeAudioStop();
    if (g_romLoaded)
    {
        if (s_restartResumeThread)
            CeSaveSramCheckpoint(); /* dynarec-safe ROM switch (RestartProcess()) - same as the in-process switch in LoadRomPath() */
        else
            CeSaveSramIfChanged();
    }
    retro_unload_game();
    retro_deinit(); /* dynarec translation caches are .bss static arrays now (no free needed); kept before the s_restartResumeThread resume below anyway - see that comment */

    GapiShutdown();
    CeShowShellChrome(g_hwnd);

    if (g_mutex)
        CloseHandle(g_mutex);

    if (s_restartResumeThread)
    {
        /* RestartProcess() left a relaunched instance suspended instead
         * of resuming it immediately - see that function's comment. Wake
         * it only now, once this process has fully torn down (the ~11MiB
         * dynarec caches are .bss so nothing to free, but the ordering
         * kept the old round-26 VirtualAlloc race from ever recurring and
         * still guarantees a clean handoff). */
        ResumeThread(s_restartResumeThread);
        CloseHandle(s_restartResumeThread);
        CloseHandle(s_restartResumeProcess);
        s_restartResumeThread  = NULL;
        s_restartResumeProcess = NULL;
    }

    ExitProcess((UINT)exitCode);
}

/* "HHTaskBar" is the standard window class of the Windows CE Explorer
 * taskbar on this device - FindWindow+ShowWindow(HIDE) on it is what
 * both prior CE ports on this hardware settled on after aygshell.dll's
 * SHFullScreen proved unreliable/fragile (see either project's
 * the dev notes). Needs nothing beyond coredll.dll. */
static HWND CeFindTaskBarWindow(void)
{
    return FindWindowW(L"HHTaskBar", NULL);
}

static void CeHideShellChrome(HWND hwnd)
{
    HWND hTaskBar = CeFindTaskBarWindow();
    (void)hwnd;
    if (hTaskBar)
    {
        ShowWindow(hTaskBar, SW_HIDE);
        CeLog("CeHideShellChrome: hid HHTaskBar window directly");
    }
    else
    {
        CeLog("CeHideShellChrome: HHTaskBar window not found");
    }
}

/* Restores shell chrome on exit - this is a shared-shell CE device, not
 * a single-purpose game handheld, so leaving the taskbar hidden after
 * this app closes would affect the user's other apps until reboot. */
static void CeShowShellChrome(HWND hwnd)
{
    HWND hTaskBar = CeFindTaskBarWindow();
    (void)hwnd;
    if (hTaskBar)
        ShowWindow(hTaskBar, SW_SHOW);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSW wc;
    MSG msg;

    (void)hPrevInstance;
    (void)nCmdShow;

    CeLog("WinMain start, build " __DATE__ " " __TIME__);

    g_hInstance = hInstance;

    g_mutex = CreateMutexW(NULL, TRUE, kMutexName);
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(kWndClassName, NULL);
        if (existing)
        {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CeLog("WinMain: another instance is already running, exiting");
        ExitProcess(0);
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWndClassName;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN));

    if (!RegisterClassW(&wc))
    {
        CeLog("WinMain: RegisterClassW failed, error=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"RegisterClassW failed", kAppTitle, MB_OK);
        CeShutdown(1);
    }

    /* WS_POPUP (not just WS_VISIBLE): both prior CE ports on this
     * hardware confirmed a plain overlapped window is still managed by
     * the shell as a regular window and doesn't reliably reclaim the
     * taskbar's screen space once CeHideShellChrome() hides it. */
    g_hwnd = CreateWindowW(kWndClassName, CE_APP_TITLE L" - No ROM loaded", WS_VISIBLE | WS_POPUP,
                            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                            NULL, NULL, hInstance, NULL);
    if (!g_hwnd)
    {
        CeLog("WinMain: CreateWindowW failed, error=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"CreateWindowW failed", kAppTitle, MB_OK);
        CeShutdown(1);
    }

    CeHideShellChrome(g_hwnd);
    /* Re-assert visibility/layout after hiding the taskbar - without
     * this the window doesn't re-layout to cover the space the taskbar
     * just vacated (confirmed by both prior CE ports). */
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    CeConfigLoad(); /* before any *_Init() below - they read their settings out of this shared table */
    CeLangInit(); /* kicks off the jptahoma.ttc background load; see ce_lang.h */

    /* No non-ASCII path guard here: showing the main menu must never be
     * blocked. gpSP only reaches a narrow path it can't reopen (the
     * picked ROM - see WidePathToNarrow()'s
     * comment) once the user actually starts a load, so the hard-exit
     * guard lives only at the ROM picker's result (LoadRomFlow/
     * LoadRomPath). A non-ASCII install folder is no longer fatal - at
     * worst gba_bios.bin next to AppMain.exe isn't found and the core
     * runs on its built-in open-source BIOS instead.
     * BIOS/ROM-free operations (browsing menus, Input/Sound/Video config)
     * stay usable regardless. */

    CeInputInit();
    CeAudioInit();
    CeVideoInit();
    CeFileOpenInit();

    /* Pre-locate gba_bios.bin (next to AppMain.exe, or a folder
     * remembered from an older version's BIOS picker). Only done here, at
     * startup. Non-fatal and silent when nothing is found - the core then
     * looks next to the ROM, and otherwise uses its built-in open-source
     * BIOS. Never blocks the menu. */
    ResolveBiosSystemDir();

    retro_set_environment(ce_environment);
    retro_set_video_refresh(ce_video_refresh);
    retro_set_audio_sample(ce_audio_sample_noop);
    retro_set_audio_sample_batch(ce_audio_sample_batch);
    retro_set_input_poll(ce_input_poll);
    retro_set_input_state(ce_input_state);

    retro_init();
    CeLog("WinMain: retro_init done");

    /* A non-empty lpCmdLine here means LoadRomFlow()'s dynarec-safe-
     * restart path (RestartProcess(romPath)) relaunched us with a ROM
     * path as the *entire* command line - no argv splitting on either
     * end, this process and the one that spawned it are the only two
     * parties involved. Load it directly and skip straight to gameplay
     * instead of blocking on the "No ROM loaded" menu; if that load
     * fails for some reason, fall back to the normal menu same as a
     * fresh launch would show. */
    if (lpCmdLine && lpCmdLine[0] != L'\0')
    {
        CeLog("WinMain: relaunched with ROM path on command line");
        if (LoadRomPath(g_hwnd, lpCmdLine))
        {
            if (!GapiInit(g_hwnd))
                CeLog("WinMain: GapiInit failed after command-line ROM load - continuing without video output");
            CeHideShellChrome(g_hwnd);
        }
        else
        {
            ShowMainMenuDialog(g_hwnd);
        }
    }
    else
    {
        /* Start on the menu ("No ROM loaded", black background) - blocks
         * here until the user opens a ROM (ShowMainMenuDialog only
         * returns once g_romLoaded is true, or the app has already
         * exited via Exit inside the dialog).
         *
         * A first-boot-only display corruption was tracked here through
         * the dev notes rounds 17-21: the very first time this process
         * opened the main menu with the Select ROM dialog nested on top
         * of it, the screen rendered corrupted (main menu and file
         * picker pixels blended together) - every fresh launch,
         * reproducibly, but only ever this first time. Four hardware-
         * tested fixes aimed at the nested dialog itself (a deferred font
         * load, a forced repaint, a process pre-warm restart, and simply
         * accepting it as cosmetic-only) didn't stick; round 22 instead
         * removed the nesting structurally (round 65 retried nesting and
         * the glitch came back) - see MainMenuDlgProc's IDC_MM_OPEN
         * case and ShowMainMenuDialog()'s own comment. */
        ShowMainMenuDialog(g_hwnd);
    }

    g_running = 1;
    while (g_running)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running)
            break;

        if (!g_romLoaded || g_paused)
        {
            /* No game running (still on the "No ROM loaded" screen) or
             * the touch-to-reveal menu is up: nothing to emulate/blit
             * this tick. Sleep instead of spinning the message pump at
             * 100% CPU for no reason. */
            Sleep(10);
            continue;
        }

        {
            /* Frame pacing - see g_frameIntervalMs's declaration. Without
             * this, retro_run() was being called back-to-back as fast as
             * the host CPU allows: it always emulates exactly one GBA
             * frame's worth of GBA-clock time per call regardless of how
             * long that takes in real time, so an unthrottled loop just
             * fast-forwards the game far ahead of real time. Confirmed on
             * real hardware (2026-08-10): CeAudioPushSamples's ring
             * buffer (125ms of cushion) was overrunning continuously,
             * hundreds of thousands of samples dropped - only possible if
             * frames were being produced far faster than the 65536Hz
             * they're meant to play back at. Fixed-schedule (not
             * sleep-a-constant-10ms) so small per-frame variance doesn't
             * accumulate into drift. s_nextDueTick == 0 is "never
             * scheduled yet" - true on the very first frame and again
             * right after resuming from the menu/pause (nothing else
             * resets it), where treating "due" as "now" avoids trying to
             * burst-catch-up through however long the menu was open. */
            static double s_nextDueTick = 0.0;
            static int    s_catchup     = 0;
            DWORD nowTick = GetTickCount();

            /* Audio catch-up (see s_audioCatchup's declaration): only when
             * Frame Skip is enabled (g_audioBuffStatusCb != NULL) - with it
             * off, g_audioBuffStatusCb is NULL and this whole branch is
             * skipped, so s_catchup stays 0 and pacing is unchanged. While
             * the ring is draining, drop the sleep so retro_run() runs
             * back-to-back: each skipped frame (~12ms wall) still produces a
             * full GBA frame of audio (~16.7ms), so the ring physically
             * refills at ~1.4x real time until it recovers. */
            if (g_audioBuffStatusCb)
            {
                int      caActive, caUnder;
                unsigned caOcc;
                CeAudioGetBufferStatus(&caActive, &caOcc, &caUnder);
                if (caActive)
                {
                    if (!s_catchup && caOcc < CE_AUDIO_CATCHUP_LOW_PCT)
                        s_catchup = 1;
                    else if (s_catchup && caOcc >= CE_AUDIO_CATCHUP_OK_PCT)
                        s_catchup = 0;
                }
                else
                    s_catchup = 0;
            }
            else
                s_catchup = 0;
            s_audioCatchup = s_catchup;

            if (s_nextDueTick == 0.0)
                s_nextDueTick = (double)nowTick;

            if (!s_catchup)
            {
                if ((double)nowTick < s_nextDueTick)
                {
                    Sleep((DWORD)(s_nextDueTick - (double)nowTick));
                    nowTick = GetTickCount();
                }

                s_nextDueTick += g_frameIntervalMs;

                /* Behind schedule (this frame's own work took longer than
                 * g_frameIntervalMs, or we're the first frame after a pause) -
                 * resync to now rather than let the loop try to silently
                 * fast-forward through the backlog to catch up, which would
                 * reproduce the exact runaway this pacing exists to prevent. */
                if (s_nextDueTick < (double)nowTick)
                    s_nextDueTick = (double)nowTick;
            }
            else
            {
                /* In catch-up: no sleep. Keep the schedule pinned to now so
                 * that when the ring recovers and s_catchup clears, the
                 * pacer doesn't see a huge backlog and try to fast-forward
                 * through it. */
                s_nextDueTick = (double)nowTick;
            }
        }

        if (g_audioBuffStatusCb)
        {
            /* Contractually "called right before retro_run() every
             * frame" (see the RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_
             * STATUS_CALLBACK doc in libretro.h) - only set while Video
             * Config's Frame Skip is above 0 (ce_video.c), so this is a
             * no-op call when it's Off. */
            int active, underrunLikely;
            unsigned occupancyPercent;

            if (CeVideoFrameSkipShouldForceRender())
            {
                /* Frame Skip's own cap (see ce_video.h) has already been
                 * hit: report "buffer not active" so the core's
                 * FRAMESKIP_AUTO logic (which only skips when told the
                 * buffer is active *and* underrunning) renders this one
                 * frame regardless of the real audio state. */
                active = 0;
                occupancyPercent = 0;
                underrunLikely = 0;
            }
            else
            {
                CeAudioGetBufferStatus(&active, &occupancyPercent, &underrunLikely);
                /* In catch-up mode force "underrun likely" so the core
                 * skips rendering on every frame until the ring recovers -
                 * CeAudioGetBufferStatus only reports it below ~25%, but
                 * catch-up starts at CE_AUDIO_CATCHUP_LOW_PCT (45%), and
                 * full 26ms frames in that band refill the ring too slowly. */
                if (s_audioCatchup)
                    underrunLikely = 1;
            }
            g_audioBuffStatusCb(active ? true : false, occupancyPercent, underrunLikely ? true : false);
        }

        {
            /* Temporary perf instrumentation - see ce_audio_sample_batch()
             * and ce_gapi.c's "perf: blit" for the matching per-frame
             * counters. retro_run()'s own wall time already includes both
             * of those (the core calls ce_video_refresh/
             * ce_audio_sample_batch synchronously from inside it), so
             * comparing this average against "perf: blit" + "perf:
             * audio_push" shows how much is core emulation vs. CE-side
             * I/O. Remove once the bottleneck is identified. */
            static unsigned s_accumMs = 0, s_maxMs = 0, s_count = 0;
            DWORD t0 = GetTickCount();
            unsigned elapsed;

            retro_run();

            elapsed = (unsigned)(GetTickCount() - t0);
            s_accumMs += elapsed;
            if (elapsed > s_maxMs)
                s_maxMs = elapsed;
            if (++s_count >= 60)
            {
                unsigned blitAvgMs = 0, blitMaxMs = 0;

                CeLog("perf: retro_run avg=%ums max=%ums over %u frames",
                      s_accumMs / s_count, s_maxMs, s_count);

                /* Unified cross-core summary line: the same fixed shape
                 * every sister CE port emits, so their debug logs line up
                 * side by side. Purely additive - the detailed breakdown
                 * (perf: cpu_thread/render_audio/video_run in libretro.c,
                 * perf: blit's loop split in ce_gapi.c, audio underrun
                 * counts in ce_audio.c) is left untouched. The blit
                 * numbers are ce_gapi.c's last 60-frame rollover; its
                 * counter and this one are both 60 frames but need not
                 * land on the same frame. */
                GapiGetLastBlitPerf(&blitAvgMs, &blitMaxMs);
                CeLog("perf: retro_run avg=%ums max=%ums blit avg=%ums max=%ums",
                      s_accumMs / s_count, s_maxMs, blitAvgMs, blitMaxMs);

                s_accumMs = 0;
                s_maxMs = 0;
                s_count = 0;
            }
        }

        /* Periodic SRAM autosave - the pause-time save in
         * ShowMainMenuDialog() only helps if the menu actually gets
         * opened before the device is powered off; this covers a
         * straight-through play session that never touches the menu at
         * all. ~30s is arbitrary (same margin the sister PopSNES
         * port uses) - frequent enough to bound how much an in-game save
         * could be lost. CeSaveSramIfChanged() only writes when the SRAM has
         * changed: the write stalls this loop long enough to cut the
         * sound out (see s_srmShadow). */
        {
            static DWORD s_lastSramSaveTick = 0;
            DWORD now = GetTickCount();
            if (s_lastSramSaveTick == 0)
                s_lastSramSaveTick = now; /* first frame of gameplay - start the 30s window now, not at an immediate save */
            else if (now - s_lastSramSaveTick >= 30000)
            {
                CeSaveSramIfChanged();
                s_lastSramSaveTick = now;
            }
        }
    }

    CeLog("WinMain: normal shutdown");
    CeShutdown(0);
    return 0; /* unreachable - CeShutdown() calls ExitProcess() */
}
