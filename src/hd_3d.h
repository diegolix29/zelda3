#ifndef ZELDA3_HD_3D_H_
#define ZELDA3_HD_3D_H_
// 3D view: tilted (and optionally curved) world. See hd_3d.c.
#include "types.h"

typedef struct Ppu Ppu;

enum {
  kHd3d_NumLevels = 6,   // tilt: 0 = off, then 20, 35, 50, 65, 75 degrees
  kHd3d_NumCurves = 4,   // world curve: off, light, medium, strong
  kHd3d_NumDists = 3,    // sprite draw distance: screen, 2x, 3x
};

extern int g_view3d, g_view3d_curve, g_view3d_dist, g_view3d_walls;

// Indoor walls raised in 3D (0 = flat, 1 = on)
void Hd3d_SetWalls(int on, const char *why);
void Hd3d_ToggleWalls(void);

void Hd3d_SetDrawDistance(int dist, const char *why);
void Hd3d_CycleDrawDistance(void);
const char *Hd3d_DrawDistanceName(int dist);
// Sprite draw distance: true when enemies beyond the view should still be
// drawn, with the extra margins (pixels) left/right, above and below the view
bool Hd3d_FarSprites(int *margin_x, int *margin_top, int *margin_bottom);
// Pieces of far enemies, kept out of the real OAM (sprite.c, Sprite_Main):
// position relative to the game camera, OAM word 1 (tile + flags), big size
void Hd3d_FarPiecesClear(void);
void Hd3d_FarPiece(int slot, int x, int y, uint16 char_flags, int big);

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

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))   // the compiler checks the arguments
#endif
void Hd3d_Log(const char *fmt, ...);
// Writes every sprite piece and group of the next 3D frame to the log (key 6)
void Hd3d_RequestDump(void);

#endif  // ZELDA3_HD_3D_H_
