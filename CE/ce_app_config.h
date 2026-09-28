/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Per-port identity macros for PopGBA (the same set of macros the sister
 * ports' frontends use, filled in for this port).
 */
#ifndef CE_APP_CONFIG_H
#define CE_APP_CONFIG_H

/* Human-readable name shown in message boxes, the title bar, and the
 * "no ROM loaded" placeholder window title. Keep it short - it's
 * concatenated with a filename in the title bar (see CE_APP_TITLE_FMT
 * usage in ce_main.c) on a 320px-wide screen. */
#define CE_APP_TITLE        L"PopGBA"

/* Narrow (ANSI) companion to CE_APP_TITLE, kept in sync by hand. Only
 * ce_res.rc's CAPTION lines use this: windres preprocesses .rc through
 * cpp (this file is already #included there), but a wide L"..." CAPTION
 * literal is unverified on this toolchain, so the resource script needs
 * a plain narrow string. Everything in C code uses the wide
 * CE_APP_TITLE above. */
#define CE_APP_TITLE_A      "PopGBA"

/* Window class name. Must be unique per app on the device - reused as
 * the FindWindowW() target for single-instance detection, so two ports
 * sharing this template must NOT share this string. */
#define CE_APP_WND_CLASS     L"PopGBAWnd"

/* Named mutex for the single-instance check in WinMain(). Same
 * uniqueness requirement as CE_APP_WND_CLASS. */
#define CE_APP_MUTEX_NAME    L"PopGBA_SingleInstance"

/* Config file, written next to AppMain.exe via GetModuleFileNameW() +
 * truncate-to-last-backslash (see CeConfigGetPath() in ce_config.c). */
#define CE_APP_CONFIG_FILENAME  L"PopGBA.cfg"

/* Debug log file, same directory convention as the config file (see
 * CeLogInit() in ce_log.c). */
#define CE_APP_LOG_FILENAME     L"PopGBA_debug.log"

/* HKCU registry subkey the language picker persists to (see ce_lang.c).
 * Keep the "Software\\" prefix. */
#define CE_APP_LANG_REGKEY   L"Software\\PopGBA\\Lang"

/* Prefix libretro core-option keys the gpSP core exposes (see
 * libretro_core_options.h: gpsp_frameskip, gpsp_frameskip_threshold,
 * gpsp_frameskip_interval, gpsp_color_correction, gpsp_frame_mixing,
 * gpsp_save_method, gpsp_drc). ce_video.c's CeVideoEnvGetVariable()
 * hardcodes the specific keys it answers rather than deriving them from
 * this prefix; this macro is documentation of that prefix, not something
 * the current code consumes. */
#define CE_APP_CORE_OPT_PREFIX  "gpsp_"

#endif /* CE_APP_CONFIG_H */
