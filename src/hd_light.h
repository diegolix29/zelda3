// Illuminazione dinamica (luci attorno al fuoco, stanze buie)
#ifndef ZELDA3_HD_LIGHT_H_
#define ZELDA3_HD_LIGHT_H_

#include "types.h"

typedef struct Ppu Ppu;

extern bool g_dynamic_light;

// Prima e dopo il disegno della PPU: sospende il buio originale delle stanze buie
void HdLight_BeforeRender(Ppu *ppu);
void HdLight_AfterRender(Ppu *ppu);
// Applica luci e buio all'immagine appena disegnata (pixel 0xLLRRGGBB, LL = layer)
void HdLight_Apply(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);

// Fiamme del frame, in coordinate dello schermo (sorgenti delle braci)
enum { kHdMaxFire = 160 };
typedef struct HdFire {
  float x, y, strength;
} HdFire;
extern HdFire g_hd_fire[kHdMaxFire];
extern int g_hd_num_fire;

#endif  // ZELDA3_HD_LIGHT_H_
