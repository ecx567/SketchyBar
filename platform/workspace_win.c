// platform/workspace_win.c
//
// Virtual desktop + front-app adapter (S1 stub; S7 fills in).
//
// Replaces src/workspace.m (NSWorkspace notifications, backingScaleFactor).
// On Windows 11, virtual-desktop awareness comes from IVirtualDesktopManager
// (GetWindowDesktopId, IsWindowOnCurrentVirtualDesktop); front-app detection
// uses GetForegroundWindow / SetWinEventHook(EVENT_SYSTEM_FOREGROUND).
//
// S1: compile-time stub establishing the file.

#include "win_platform.h"

#include <windows.h>
