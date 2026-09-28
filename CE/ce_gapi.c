/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * GDI video backend for PopGBA (was a GAPI/gx.dll wrapper through
 * round 28 - see this file's git history / the dev notes).
 *
 * Ported from the sister PopGB port's ce_video.c GDI backend. The
 * shape, and the reasons for it, are identical:
 *
 *   - gx.dll is dropped entirely. GDI (GetDC/CreateDIBSection/BitBlt/
 *     PatBlt) is part of coredll.dll like every other Win32 call this
 *     port already makes, so it carries no extra DLL dependency and no
 *     licensing question about the unverifiable gx.dll builds that
 *     circulate for this device family.
 *   - The obvious "let StretchDIBits() do the scale-and-letterbox in one
 *     call" was tried on real hardware for the gnuboy port and found to
 *     be *much* slower than a flat 1:1 BitBlt - this device's display
 *     driver has no fast path for arbitrary-ratio stretching. So the
 *     scale is done here by hand (the same nearest-neighbour / integer-
 *     double loops the old GAPI backend used on its raw framebuffer
 *     pointer) into an off-screen RGB565 DIB section sized to the whole
 *     physical screen, then that section is transferred with a single
 *     plain 1:1 BitBlt().
 *   - The DIB section is top-down (negative biHeight) to match the
 *     core's top-to-bottom row order, and BI_BITFIELDS with explicit
 *     5-6-5 masks because a plain BI_RGB 16bpp DIB defaults to 5-5-5.
 *
 * Only two Scale modes exist for gpSP (see CeScaleMode in ce_video.h):
 *   - CE_SCALE_1TO1 ("x1"): the core's 240x160 output, centred, no scale.
 *   - CE_SCALE_X2   ("x2"): exact integer 2x = 480x320, fills the screen.
 * Both are exact, so there is no fractional-scale path here - just a
 * per-row memcpy for x1, a broadcast-pair store + row-duplicate for x2,
 * and a generic nearest-neighbour fallback that gpSP's fixed geometry
 * never actually reaches.
 *
 * Unlike GAPI's exclusive fullscreen surface, GDI draws into the game
 * window's own client area, so this backend IS subject to normal
 * invalidation/repaint: GapiForceRepaint() re-blits the last frame (kept
 * in the DIB section) plus its letterbox border, called from ce_main.c's
 * WM_PAINT while a ROM is loaded (a modal menu / sub-dialog closing over
 * the paused game).
 *
 * The public names still start with "Gapi" only to keep the call sites in
 * ce_main.c unchanged; there is no GAPI here any more.
 */
#include "ce_gapi.h"
#include "ce_log.h"
#include "ce_video.h"

#include <stdint.h>
#include <string.h>

/* ---- GDI surface (built once in GapiInit, kept for the process) ---- */

static HWND      s_hwnd      = NULL;
static int       s_physW     = 0;
static int       s_physH     = 0;
static HDC       s_memDC     = NULL;   /* the DIB section's own selected-in memory DC */
static HBITMAP   s_dibBitmap = NULL;
static uint16_t *s_dibBits   = NULL;   /* CPU-visible bits of s_dibBitmap */
static int       s_dibPitch  = 0;      /* byte stride/row (DIB rows are DWORD-aligned) */
static int       s_dibReady  = 0;

/* ---- cached destination geometry --------------------------------- */

/* The centred (x1) or full-screen (x2) rectangle the current frame is
 * scaled into, within both s_dibBits and the physical screen. Recomputed
 * only when s_geomDirty is set (first frame, or a Scale mode change);
 * every other frame reuses it and never touches the letterbox border
 * again - this device's screen is a direct, persistent framebuffer
 * behind GDI, not double-buffered, so untouched pixels stay put. */
static int s_dstX = 0, s_dstY = 0, s_dstW = 0, s_dstH = 0;
static int s_geomDirty = 1;
static int s_haveFrame = 0; /* has at least one real frame been blitted since GapiInit? */
static CeScaleMode s_lastScaleMode = CE_SCALE_X2;
static int s_lastImgValid = 0; /* screenshot: set by a real blit, cleared per ROM load */

/* ---- cheap perf instrumentation (no profiler on this device) ------ */
/* Logs avg/max blit time every 60 calls, then resets. "loop" is just the
 * pixel-fill into s_dibBits; the full number also includes the BitBlt to
 * screen and the once-per-mode-switch border clear. The last completed
 * 60-frame avg/max is snapshotted for ce_main.c's unified cross-core
 * "perf:" summary line via GapiGetLastBlitPerf(). */
static unsigned s_blitAccumMs     = 0;
static unsigned s_blitMaxMs       = 0;
static unsigned s_blitCount       = 0;
static unsigned s_blitLoopAccumMs = 0;
static unsigned s_blitLoopMaxMs   = 0;
static unsigned s_blitLastAvgMs   = 0;
static unsigned s_blitLastMaxMs   = 0;

void GapiGetLastBlitPerf(unsigned *avgMs, unsigned *maxMs)
{
    if (avgMs)
        *avgMs = s_blitLastAvgMs;
    if (maxMs)
        *maxMs = s_blitLastMaxMs;
}

int GapiInit(HWND hwnd)
{
    HDC hdc;
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } dibInfo;

    s_hwnd = hwnd;

    if (s_dibReady)
    {
        /* Idempotent. ce_main.c's ShowMainMenuDialog() brackets the menu
         * with GapiShutdown()/GapiInit() (the GAPI backend had to release
         * and re-acquire its exclusive fullscreen surface). GDI has no
         * such surface and the DIB section is kept alive across the menu
         * so WM_PAINT can redraw the paused frame from it, so re-entry is
         * a no-op apart from forcing one border re-clear. */
        s_geomDirty = 1;
        return 1;
    }

    s_physW = GetSystemMetrics(SM_CXSCREEN);
    s_physH = GetSystemMetrics(SM_CYSCREEN);
    CeLog("GapiInit: GDI backend, screen %dx%d", s_physW, s_physH);
    if (s_physW <= 0 || s_physH <= 0)
        return 0;

    hdc = GetDC(hwnd);
    if (!hdc)
    {
        CeLog("GapiInit: GetDC failed, error=%lu", (unsigned long)GetLastError());
        return 0;
    }

    /* Paint the physical screen black up front - otherwise whatever was
     * in video memory before this process wrote to it (a previous app,
     * the shell) stays visible behind every dialog until the first ROM
     * loads and starts producing frames. */
    PatBlt(hdc, 0, 0, s_physW, s_physH, BLACKNESS);

    memset(&dibInfo, 0, sizeof(dibInfo));
    dibInfo.h.biSize        = sizeof(BITMAPINFOHEADER);
    dibInfo.h.biWidth       = s_physW;
    dibInfo.h.biHeight      = -s_physH; /* negative = top-down */
    dibInfo.h.biPlanes      = 1;
    dibInfo.h.biBitCount    = 16;
    dibInfo.h.biCompression = BI_BITFIELDS;
    dibInfo.masks[0]        = 0xF800; /* red:   5 bits @ bit 11 */
    dibInfo.masks[1]        = 0x07E0; /* green: 6 bits @ bit 5  */
    dibInfo.masks[2]        = 0x001F; /* blue:  5 bits @ bit 0  */

    s_dibBitmap = CreateDIBSection(hdc, (BITMAPINFO *)&dibInfo, DIB_RGB_COLORS,
                                   (void **)&s_dibBits, NULL, 0);
    if (!s_dibBitmap || !s_dibBits)
    {
        CeLog("GapiInit: CreateDIBSection failed, error=%lu", (unsigned long)GetLastError());
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    s_dibPitch = ((s_physW * 16 + 31) / 32) * 4; /* DIB rows are DWORD-aligned */

    s_memDC = CreateCompatibleDC(hdc);
    if (!s_memDC)
    {
        CeLog("GapiInit: CreateCompatibleDC failed, error=%lu", (unsigned long)GetLastError());
        DeleteObject(s_dibBitmap);
        s_dibBitmap = NULL;
        s_dibBits = NULL;
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    SelectObject(s_memDC, s_dibBitmap);

    ReleaseDC(hwnd, hdc);

    s_dibReady  = 1;
    s_geomDirty = 1;
    s_haveFrame = 0;
    return 1;
}

void GapiShutdown(void)
{
    /* GDI backend: no exclusive display surface to hand back (unlike the
     * old GAPI one). The DIB section and memory DC are deliberately kept
     * alive - ce_main.c calls this around every modal menu, and WM_PAINT
     * redraws the paused frame from the DIB while that menu is up. They
     * are released only implicitly when the process exits (ExitProcess
     * from CeShutdown), the same way the sister PopGB port's GDI
     * backend leaves them. */
}

/* Re-blit the last frame (still in s_dibBits) plus its letterbox border.
 * Called from ce_main.c's WM_PAINT when a ROM is loaded - a modal menu or
 * sub-dialog closing over the paused game invalidates part of the game
 * window, and GDI (unlike GAPI's bypass-the-window fullscreen surface)
 * needs that region redrawn. Bypasses Frame Skip: a forced repaint must
 * never be silently dropped. */
void GapiForceRepaint(void)
{
    HDC hdc;

    if (!s_dibReady)
        return;

    hdc = GetDC(s_hwnd);
    if (!hdc)
        return;

    if (s_dstX > 0 || s_dstY > 0 || !s_haveFrame)
        PatBlt(hdc, 0, 0, s_physW, s_physH, BLACKNESS); /* x1 leaves a wide border; also covers "no frame yet" */
    if (s_haveFrame)
        BitBlt(hdc, s_dstX, s_dstY, s_dstW, s_dstH, s_memDC, s_dstX, s_dstY, SRCCOPY);

    ReleaseDC(s_hwnd, hdc);
}

void GapiInvalidateLastImage(void)
{
    s_lastImgValid = 0;
}

int GapiGetLastImage(const void **pixels, unsigned *width, unsigned *height, unsigned *pitch)
{
    if (!s_dibReady || !s_lastImgValid || s_dstW <= 0 || s_dstH <= 0)
        return 0;
    *pixels = (const uint8_t *)s_dibBits + (size_t)s_dstY * s_dibPitch + (size_t)s_dstX * 2;
    *width  = (unsigned)s_dstW;
    *height = (unsigned)s_dstH;
    *pitch  = (unsigned)s_dibPitch;
    return 1;
}

void GapiBlitRGB565(const void *src, unsigned srcW, unsigned srcH, unsigned srcPitchBytes)
{
    const uint8_t *srcBytes = (const uint8_t *)src;
    CeScaleMode scaleMode;
    uint8_t *dstBase;
    HDC hdc;
    DWORD t0, t1, tLoop0, tLoop1, elapsed, loopElapsed;
    unsigned y;

    if (!s_dibReady || !srcW || !srcH || !srcPitchBytes)
        return;

    t0 = GetTickCount();

    scaleMode = CeVideoGetScaleMode();
    if (scaleMode != s_lastScaleMode)
    {
        s_lastScaleMode = scaleMode;
        s_geomDirty = 1;
    }

    if (s_geomDirty)
    {
        HDC cdc = GetDC(s_hwnd);
        if (cdc)
        {
            PatBlt(cdc, 0, 0, s_physW, s_physH, BLACKNESS); /* wipe whichever border the previous mode left */
            ReleaseDC(s_hwnd, cdc);
        }

        if (scaleMode == CE_SCALE_1TO1)
        {
            /* Centred, unscaled - crop rather than scale if the source is
             * somehow larger than the screen (never happens for gpSP's
             * fixed 240x160 on this 480x320 panel; kept as a fallback). */
            s_dstW = (int)((srcW < (unsigned)s_physW) ? srcW : (unsigned)s_physW);
            s_dstH = (int)((srcH < (unsigned)s_physH) ? srcH : (unsigned)s_physH);
        }
        else /* CE_SCALE_X2 */
        {
            s_dstW = (int)(srcW * 2);
            s_dstH = (int)(srcH * 2);
            if (s_dstW > s_physW) s_dstW = s_physW;
            if (s_dstH > s_physH) s_dstH = s_physH;
        }

        s_dstX = (s_physW - s_dstW) / 2;
        s_dstY = (s_physH - s_dstH) / 2;
        /* Keep s_dstX even: the x2 / generic fast paths pack two pixels
         * into one 32-bit store, so the dest address must stay 4-byte
         * aligned. True by construction for 240x160 on a 480x320 screen;
         * a sister CE port once hit a real Datatype Misalignment BER from
         * exactly this off-by-one, so mask rather than trust it. */
        s_dstX &= ~1;

        s_geomDirty = 0;
    }

    dstBase = (uint8_t *)s_dibBits
              + (size_t)s_dstY * s_dibPitch
              + (size_t)s_dstX * 2;

    tLoop0 = GetTickCount();

    if ((unsigned)s_dstW == srcW && (unsigned)s_dstH == srcH)
    {
        /* x1: straight per-row copy into the centred sub-rect. */
        for (y = 0; y < srcH; y++)
            memcpy(dstBase + (size_t)y * s_dibPitch,
                   srcBytes + (size_t)y * srcPitchBytes,
                   (size_t)srcW * 2);
    }
    else if ((unsigned)s_dstW == srcW * 2 && (unsigned)s_dstH == srcH * 2)
    {
        /* x2: every dest pixel-pair is the same source pixel, so skip the
         * per-pixel accumulator - broadcast one source uint16 into both
         * halves of an aligned 32-bit store (little-endian: low half =
         * lower address = left pixel), then the second scanline for this
         * source row is a plain memcpy of the row just built. Same
         * s_scale2x idea the sister PopNES / PopGB ports use
         * (~18-20% blit-time drop measured on this device family). */
        for (y = 0; y < srcH; y++)
        {
            const uint16_t *srcRow = (const uint16_t *)(srcBytes + (size_t)y * srcPitchBytes);
            uint32_t *dstRow0 = (uint32_t *)(dstBase + (size_t)(y * 2) * s_dibPitch);
            uint8_t  *dstRow1 = dstBase + (size_t)(y * 2 + 1) * s_dibPitch;
            unsigned x;

            for (x = 0; x < srcW; x++)
            {
                uint32_t p = srcRow[x];
                dstRow0[x] = p | (p << 16);
            }
            memcpy(dstRow1, dstRow0, (size_t)srcW * 4);
        }
    }
    else
    {
        /* Generic nearest-neighbour safety fallback (16.16 accumulator,
         * one divide per axis per frame, never per pixel - this target
         * has no hardware integer divide). gpSP's fixed 240x160 output on
         * this 480x320 screen never reaches here. */
        unsigned xStep = (srcW << 16) / (unsigned)s_dstW;
        unsigned yStep = (srcH << 16) / (unsigned)s_dstH;
        unsigned yAccum = 0;
        int yy;

        for (yy = 0; yy < s_dstH; yy++)
        {
            const uint16_t *srcRow = (const uint16_t *)(srcBytes + (size_t)(yAccum >> 16) * srcPitchBytes);
            uint16_t *dstRow = (uint16_t *)(dstBase + (size_t)yy * s_dibPitch);
            unsigned xAccum = 0;
            int xx;

            for (xx = 0; xx < s_dstW; xx++)
            {
                dstRow[xx] = srcRow[xAccum >> 16];
                xAccum += xStep;
            }
            yAccum += yStep;
        }
    }

    tLoop1 = GetTickCount();
    loopElapsed = (DWORD)(tLoop1 - tLoop0);
    s_blitLoopAccumMs += loopElapsed;
    if (loopElapsed > s_blitLoopMaxMs)
        s_blitLoopMaxMs = loopElapsed;

    hdc = GetDC(s_hwnd);
    if (hdc)
    {
        BitBlt(hdc, s_dstX, s_dstY, s_dstW, s_dstH, s_memDC, s_dstX, s_dstY, SRCCOPY);
        ReleaseDC(s_hwnd, hdc);
        s_haveFrame = 1;
        s_lastImgValid = 1;
    }

    t1 = GetTickCount();
    elapsed = (DWORD)(t1 - t0);
    s_blitAccumMs += elapsed;
    if (elapsed > s_blitMaxMs)
        s_blitMaxMs = elapsed;
    if (++s_blitCount >= 60)
    {
        CeLog("perf: blit avg=%ums max=%ums loop_avg=%ums loop_max=%ums over %u frames",
              s_blitAccumMs / s_blitCount, s_blitMaxMs,
              s_blitLoopAccumMs / s_blitCount, s_blitLoopMaxMs, s_blitCount);
        s_blitLastAvgMs = s_blitAccumMs / s_blitCount;
        s_blitLastMaxMs = s_blitMaxMs;
        s_blitAccumMs = 0;
        s_blitMaxMs = 0;
        s_blitLoopAccumMs = 0;
        s_blitLoopMaxMs = 0;
        s_blitCount = 0;
    }
}
