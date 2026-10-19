// Incendio di cespugli ed erba (Lanterna e Bacchetta del Fuoco), parte di
// "Spada e oggetti interazione ambiente"
#ifndef ZELDA3_HD_BURN_H_
#define ZELDA3_HD_BURN_H_

#include "types.h"

// Da' fuoco al cespuglio o all'erba nel punto (x, y) del mondo esterno, se
// c'e'; con spread il fuoco passa piano piano a quelli vicini (Bacchetta del
// Fuoco e Lanterna), senza brucia solo quella tile. true se ha preso fuoco.
bool HdBurn_Ignite(uint16 x, uint16 y, bool spread);
// Fa avanzare l'incendio: una volta per frame
void HdBurn_Update(void);
// Spegne tutto e dimentica le tile annerite: entrando in una casa o
// caricando un salvataggio il gioco fa ricrescere tutti i cespugli
void HdBurn_Reset(void);

// Per il disegno delle fiamme (hd_fx.c) e della loro luce (hd_light.c)
enum {
  kHdBurnTime = 66,      // frame in cui una tile resta in fiamme
  kHdScorchTime = 420,   // frame in cui l'annerimento passa da fresco a definitivo
};
typedef struct HdBurnTile {
  uint16 x, y;   // angolo in alto a sinistra della tile 16x16, in pixel del mondo
  uint16 age;    // frame passati da quando ha preso fuoco (o da quando si e' spenta)
  uint8 seed;    // varia la forma delle fiamme da una tile all'altra
  uint8 area;    // area esterna della tile
} HdBurnTile;
// Tile in fiamme nell'area attuale e tile gia' bruciate (annerite) di tutte le
// aree: quelle da disegnare hanno area == overworld_area_index
int HdBurn_GetBurning(const HdBurnTile **tiles);
int HdBurn_GetScorched(const HdBurnTile **tiles);

#endif  // ZELDA3_HD_BURN_H_
