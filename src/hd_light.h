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
// Vista 3D: la luce dell'ultimo HdLight_Apply sui pixel che non l'hanno avuta
// (kind 2 = terreno oltre lo schermo, 3 = sprite), nel punto uv dello schermo 2D
void HdLight_Projected(uint8 *pixels, int pitch, int width, int height, const uint32 *uv, const uint8 *kind);
// Alone delle fate, dopo l'acqua HD
void HdLight_FairyGlow(uint8 *pixels, int pitch, int width, int height);

// Fiamme del frame, in coordinate dello schermo (sorgenti delle braci)
enum { kHdMaxFire = 400 };
typedef struct HdFire {
  float x, y, strength;
  int8 dir;  // verso delle braci: 0 in alto, -1 a sinistra, 1 a destra (torce sulle pareti laterali), 2 in basso (parete sud)
} HdFire;
extern HdFire g_hd_fire[kHdMaxFire];
extern int g_hd_num_fire;
// Colore sottratto da HdLight_Apply nel frame (stanze buie con 1-2 torce),
// 0 se non c'e'. L'acqua HD lo usa per rifare la stessa luce sui suoi colori
extern int g_hd_light_sub;

#endif  // ZELDA3_HD_LIGHT_H_
