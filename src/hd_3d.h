#ifndef ZELDA3_HD_3D_H_
#define ZELDA3_HD_3D_H_
// 3D view (step 1: tilted world). See hd_3d.c.
#include "types.h"

typedef struct Ppu Ppu;

enum { kHd3d_NumLevels = 4 };  // 0 = off, then 20, 35, 50 degrees

extern int g_view3d;

void Hd3d_Init(void);
void Hd3d_SetLevel(int level, const char *why);
void Hd3d_CycleLevel(void);
const char *Hd3d_LevelName(int level);

// Called from ZeldaDrawPpuFrame. BeginFrame returns true when this frame is
// drawn in 3D; then CaptureLine runs after every ppu_runLine, AfterLines once
// after the scanline loop, and EndFrame at the very end (after the HD effects).
bool Hd3d_BeginFrame(Ppu *ppu, int width, int height);
void Hd3d_CaptureLine(Ppu *ppu, int line);
void Hd3d_AfterLines(Ppu *ppu);
void Hd3d_EndFrame(uint8 *pixels, int pitch, int width, int height);

void Hd3d_Log(const char *fmt, ...);

#endif  // ZELDA3_HD_3D_H_
