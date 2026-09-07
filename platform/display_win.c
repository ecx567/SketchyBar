// platform/display_win.c
//
// Display management via EnumDisplayMonitors / DPI (S1 stub; S6/S7 fill in).
//
// Replaces src/display_nsscreen.m (NSScreen frame, visibleFrame, notch,
// backingScaleFactor). Maps WM_DISPLAYCHANGE / EnumDisplayMonitors to the
// DISPLAY_* events and GetDpiForWindow to workspace_get_scale. There is no
// menu-bar strip on Windows, so the notch/top-inset remain config-driven only.
//
// S1: compile-time stub establishing the file.

#include "win_platform.h"

#include <windows.h>
