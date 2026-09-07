#pragma once
#ifdef _WIN32
#include "../platform/win_graphics_types.h"
#else
#include <CoreGraphics/CoreGraphics.h>
#endif

CGContextRef context_create(CGSize size, CGFloat scale);
