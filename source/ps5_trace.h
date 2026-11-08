/*
 * Lightweight boot tracing and fatal-signal logging for the PS5 build.
 *
 * The native title sandbox hides stdout, so progress is written to a file
 * under /download0 (and mirrored to klog when the kernel export is present).
 * A fatal-signal handler records the faulting PC and register state so a
 * crash can be resolved against the build's symbol table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Append a trace line. Safe to call from anywhere after the constructors. */
void c09_trace(const char *fmt, ...);

/* Write a 24-bit BMP of an ARGB (0xAABBGGRR) surface. Used for automatic
 * frame dumps that the host can read outside the app sandbox. */
void c09_write_bmp(const char *path, const void *pixels, int width, int height);

/* Install the fatal-signal handlers and open the trace file. Called by a
 * constructor, so applications normally never call it themselves. */
void c09_trace_init(void);

#ifdef __cplusplus
}
#endif
