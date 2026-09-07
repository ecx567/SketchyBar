// platform/media_win.c
//
// Media now-playing adapter (S1 stub; S6 fills in).
//
// Replaces src/media.m (MediaRemote). Uses SMTC
// (SystemMediaTransportControls / Windows.Media) to post MEDIA_CHANGED and
// COVER_CHANGED, extracting artwork as a byte array for the `media.artwork`
// image pipeline.
//
// S1: compile-time stub establishing the file.

#include "win_platform.h"

#include <windows.h>
