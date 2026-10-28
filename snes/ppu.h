
#ifndef ZELDA3_SNES_PPU_H_
#define ZELDA3_SNES_PPU_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "snes/saveload.h"
typedef struct Ppu Ppu;

#include "src/types.h"

typedef struct BgLayer {
  uint16_t hScroll;
  uint16_t vScroll;
  // -- snapshot starts here
  bool tilemapWider;
  bool tilemapHigher;
  uint16_t tilemapAdr;
  // -- snapshot ends here
  uint16_t tileAdr;
} BgLayer;

enum {
  kPpuXPixels = 256 + kPpuExtraLeftRight * 2,
};

typedef uint16_t PpuZbufType;

typedef struct PpuPixelPrioBufs {
  // This holds the prio in the upper 8 bits and the color in the lower 8 bits.
  PpuZbufType data[kPpuXPixels];
} PpuPixelPrioBufs;

enum {
  kPpuRenderFlags_NewRenderer = 1,
  // Render mode7 upsampled by 4x4
  kPpuRenderFlags_4x4Mode7 = 2,
  // Use 240 height instead of 224
  kPpuRenderFlags_Height240 = 4,
  // Disable sprite render limits
  kPpuRenderFlags_NoSpriteLimits = 8,
};


struct Ppu {
  bool lineHasSprites;
  uint8_t lastBrightnessMult;
  uint8_t lastMosaicModulo;
  uint8_t renderFlags;
  uint32_t renderPitch;
  uint8_t *renderBuffer;
  uint8_t extraLeftCur, extraRightCur, extraLeftRight, extraBottomCur;
  // Cerchio di transizione disegnato a parte: la finestra 1 vale per tutta la
  // riga, fasce laterali del widescreen comprese
  bool windowFullSides;
  // Posizione verticale vera (con il segno) di ogni sprite, quando la vista
  // e' spostata rispetto alla telecamera del gioco (vedi zelda_rtl.c)
  bool objYValid, objXValid;
  // sprite con y = 0xF0 da disegnare comunque (pezzi di Link e sprite spostati
  // dalla telecamera, g_oam_real_f0; vedi zelda_rtl.c)
  uint8_t objShowF0[128];
  // Sprite sempre davanti agli strati (mentre il fondale arriva da altrove)
  bool objOnTop;
  // Cifre al massimo dell'HUD (BG3, palette 5, caselle 0x90-0x99): il colore 2
  // diventa il colore cgram[0x10], che nessuno strato mostra (vedi zelda_rtl.c)
  bool bg3MaxDigits;
  int16_t objY[128], objX[128];
  // Ombre morbide (hd_fx.c): per ogni voce OAM se fa ombra e, per ogni pixel
  // del fotogramma (objShadowStride per riga), se lo sprite disegnato li'
  // viene da una di quelle voci. objShadowOut = NULL: non serve
  uint8_t objCastShadow[128];
  uint8_t *objShadowOut;
  int objShadowStride;
  uint8_t objShadowLine[kPpuXPixels];
  // Ghiaccio (hd_fx.c): le voci OAM con objIce si disegnano con i blu dei
  // nemici congelati (palette 2), ogni colore v sostituito da iceRemap[v]
  uint8_t objIce[128];
  uint8_t iceRemap[16];
  // Scala delle voci OAM in 1/256 (0 = normale): lo sprite si disegna
  // ingrandito o rimpicciolito attorno al suo centro (super bomba, hd_fx.c)
  uint16_t objScale[128];
  float mode7PerspectiveLow, mode7PerspectiveHigh;
  // Spostamento visivo della telecamera fluida (non tocca la logica di gioco)
  int16_t bgBiasX, bgBiasY, objBiasX, objBiasY;
  // Cono della Lanterna: quadrante (0-3) della mappa di BG1 da tenere sullo
  // schermo secondario, -1 = nessun cono (vedi PpuClipLampCone)
  int8_t bg1ConeQuadrant;
  // Mappa del mondo (Mode 7): fuori dalla mappa di 512x512 invece del nero si
  // ripete la fascia di nuvole in alto (righe di caselle 0-7), vedi ppu.c
  bool m7CloudFill;
  // Mappa progressiva: maschera 512x512, un byte per pixel della mappa: 0 =
  // visibile, 1 = nuvola, 2 = contorno della nuvola (piu' scuro). NULL = mappa
  // tutta visibile (messaging.c, MapFog_BuildMask)
  const uint8 *m7Fog;
  // Mappa progressiva: fascia di nuvole di 16x64 caselle che si ripete senza
  // cuciture, seguita dalla mappa tutta coperta di 64x64 (MapFog_CloudTiles);
  // NULL = le prime 8 righe della mappa
  const uint8 *m7FogClouds;

  // TMW / TSW etc
  uint8 screenEnabled[2];
  uint8 screenWindowed[2];
  uint8 mosaicEnabled;
  uint8 mosaicSize;
  // object/sprites
  uint16_t objTileAdr1;
  uint16_t objTileAdr2;
  uint8_t objSize;
  // Window
  uint8_t window1left;
  uint8_t window1right;
  uint8_t window2left;
  uint8_t window2right;
  uint32_t windowsel;

  // color math
  uint8_t clipMode;
  uint8_t preventMathMode;
  bool addSubscreen;
  bool subtractColor;
  bool halfColor;
  uint8 mathEnabled;
  uint8_t fixedColorR, fixedColorG, fixedColorB;
  // settings
  bool forcedBlank;
  uint8_t brightness;
  uint8_t mode;

  // vram access
  uint16_t vramPointer;
  uint16_t vramIncrement;
  bool vramIncrementOnHigh;
  // cgram access
  uint8_t cgramPointer;
  bool cgramSecondWrite;
  uint8_t cgramBuffer;
  // oam access
  uint16_t oamAdr;
  bool oamSecondWrite;
  uint8_t oamBuffer;

  // background layers
  BgLayer bgLayer[4];
  uint8_t scrollPrev;
  uint8_t scrollPrev2;
  
  // mode 7
  int16_t m7matrix[8]; // a, b, c, d, x, y, h, v
  uint8_t m7prev;
  bool m7largeField;
  bool m7charFill;
  bool m7xFlip;
  bool m7yFlip;
  bool m7extBg_always_zero;
  // mode 7 internal
  int32_t m7startX;
  int32_t m7startY;

  uint16_t oam[0x110];
  
  // store 31 extra entries to remove the need for clamp
  uint8_t brightnessMult[32 + 31];
  uint8_t brightnessMultHalf[32 * 2];
  uint16_t cgram[0x100];
  uint8_t mosaicModulo[kPpuXPixels];
  uint32_t colorMapRgb[256];
  PpuPixelPrioBufs bgBuffers[2];
  PpuPixelPrioBufs objBuffer;
  uint16_t vram[0x8000];
};

Ppu* ppu_init();
void ppu_free(Ppu* ppu);
void ppu_reset(Ppu* ppu);
void ppu_handleVblank(Ppu* ppu);
void ppu_runLine(Ppu* ppu, int line);
uint8_t ppu_read(Ppu* ppu, uint8_t adr);
void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val);
void ppu_saveload(Ppu *ppu, SaveLoadFunc *func, void *ctx);
void PpuBeginDrawing(Ppu *ppu, uint8_t *buffer, size_t pitch, uint32_t render_flags);

// Returns the current render scale, 1x = 256px, 2x=512px, 4x=1024px
int PpuGetCurrentRenderScale(Ppu *ppu, uint32_t render_flags);

void PpuSetMode7PerspectiveCorrection(Ppu *ppu, int low, int high);
void PpuSetExtraSideSpace(Ppu *ppu, int left, int right, int bottom);

#endif  // ZELDA3_SNES_PPU_H_
