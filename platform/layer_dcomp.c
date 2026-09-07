// platform/layer_dcomp.c
//
// DirectComposition visual layer (S1 stub; S3 fills this in).
//
// Replaces src/layer.m (CAContext + CALayer). An IDCompositionVisual wraps the
// surface's ID2D1Bitmap/D3D11 texture; layer_set_contents / layer_set_bounds
// become visual set-content / set-offset. S3 is the compositing spike that
// implements the real DirectComposition device + visual.
//
// S1: compile-time stub establishing the file.

#include "win_platform.h"

#include <windows.h>
