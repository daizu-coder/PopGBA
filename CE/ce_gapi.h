/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * GDI video backend for PopGBA (a GAPI/gx.dll wrapper through round 28 -
 * see ce_gapi.c's header comment for why it was replaced, and the dev notes).
 * Ported from the sister PopGB port's GDI backend: an off-screen
 * RGB565 DIB section sized to the physical screen, hand-scaled into, then
 * transferred with a plain 1:1 BitBlt (StretchDIBits was measured much
 * slower on this device). The "Gapi" prefix is kept only so ce_main.c's
 * call sites are unchanged - there is no GAPI here any more.
 */
#ifndef CE_GAPI_H
#define CE_GAPI_H

#include <windows.h>

/* Builds the DIB section / memory DC against hwnd (idempotent - safe to
 * call again around a modal menu). Returns 1 on success, 0 on failure
 * (GetDC / CreateDIBSection / CreateCompatibleDC failing). */
int GapiInit(HWND hwnd);

/* No-op for the GDI backend (kept because ce_main.c brackets every modal
 * menu with it); the DIB section is released only at process exit. */
void GapiShutdown(void);

/* Nearest-neighbour / integer-double blit of a tightly-described RGB565
 * source buffer (src, srcW x srcH pixels, srcPitchBytes bytes/row - all
 * given fresh every call, exactly what retro_video_refresh_t hands us)
 * into the DIB section, then a 1:1 BitBlt to the game window. Scale mode
 * (x1 centred / x2 fullscreen) comes from CeVideoGetScaleMode(). No-op if
 * GapiInit() didn't succeed. */
void GapiBlitRGB565(const void *src, unsigned srcW, unsigned srcH, unsigned srcPitchBytes);

/* Re-blit the last frame (kept in the DIB section) plus its letterbox
 * border. Call from WM_PAINT while a ROM is loaded - GDI draws into the
 * window's client area, so a dialog closing over the paused game needs
 * the exposed region redrawn. No-op before the first frame. */
void GapiForceRepaint(void);

/* Last "perf: blit" avg/max (milliseconds) logged by GapiBlitRGB565()'s
 * 60-frame instrumentation, so ce_main.c can fold them into the unified
 * cross-core "perf: retro_run ... blit ..." summary line. Either pointer
 * may be NULL; both values are 0 until the first 60-frame rollover. */
void GapiGetLastBlitPerf(unsigned *avgMs, unsigned *maxMs);

/* Screenshot support (ported from the sister PopGB's
 * CeVideoGetLastImage()). GapiGetLastImage() hands back the image
 * currently on screen at the active Scale - the dstW x dstH sub-rect of
 * the RGB565 DIB section the last GapiBlitRGB565() wrote, rows `pitch`
 * bytes apart, top-down. Returns 0 until at least one frame has been
 * blitted since the last GapiInvalidateLastImage() (called on every ROM
 * load, so a screenshot can't save the previous game's picture). */
void GapiInvalidateLastImage(void);
int  GapiGetLastImage(const void **pixels, unsigned *width, unsigned *height, unsigned *pitch);

#endif
