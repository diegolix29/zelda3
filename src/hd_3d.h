#ifndef ZELDA3_HD_3D_H_
#define ZELDA3_HD_3D_H_
// 3D view: tilted (and optionally curved) world. See hd_3d.c.
#include "types.h"

typedef struct Ppu Ppu;

enum {
  kHd3d_NumLevels = 6,   // tilt: 0 = off, then 20, 35, 50, 65, 75 degrees
  kHd3d_NumCurves = 4,   // world curve: off, light, medium, strong
};

extern int g_view3d, g_view3d_curve;

void Hd3d_Init(void);
void Hd3d_SetLevel(int level, const char *why);
void Hd3d_CycleLevel(void);
const char *Hd3d_LevelName(int level);
void Hd3d_SetCurve(int curve, const char *why);
void Hd3d_CycleCurve(void);
const char *Hd3d_CurveName(int curve);

// Called from ZeldaDrawPpuFrame. BeginFrame returns true when this frame is
// drawn in 3D; then CaptureLine runs after every ppu_runLine, AfterLines once
// after the scanline loop, and EndFrame at the very end (after the HD effects).
bool Hd3d_BeginFrame(Ppu *ppu, int width, int height);
void Hd3d_CaptureLine(Ppu *ppu, int line);
void Hd3d_AfterLines(Ppu *ppu);
void Hd3d_EndFrame(uint8 *pixels, int pitch, int width, int height);

void Hd3d_Log(const char *fmt, ...);

#endif  // ZELDA3_HD_3D_H_
