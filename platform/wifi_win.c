// platform/wifi_win.c
//
// Wi-Fi adapter (S1 stub; S6 fills in).
//
// Replaces src/wifi.m (CoreWLAN + SCDynamicStore). Uses WinRT
// NetworkInformation / WlanApi to surface the SSID and post WIFI_CHANGED.
//
// S1: compile-time stub establishing the file.

#include "win_platform.h"

#include <windows.h>
