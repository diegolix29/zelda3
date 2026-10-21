// Fuoco e ghiaccio su cespugli ed erba (Lanterna, bacchette e medaglioni):
// opzione "Interazione bruciatura e congelamento"
#ifndef ZELDA3_HD_BURN_H_
#define ZELDA3_HD_BURN_H_

#include "types.h"

// Da' fuoco al cespuglio o all'erba nel punto (x, y) del mondo esterno, se
// c'e'; con spread il fuoco passa piano piano a quelli vicini (Bacchetta del
// Fuoco e Lanterna), senza brucia solo quella tile. true se ha preso fuoco.
bool HdBurn_Ignite(uint16 x, uint16 y, bool spread);
// Congela il cespuglio o l'erba nel punto (x, y), se c'e' (Bacchetta del
// Ghiaccio). true se si e' congelato.
bool HdBurn_Freeze(uint16 x, uint16 y);
// Brucia (Medaglione della Fiamma) o congela (Medaglione dell'Etere) insieme
// tutti i cespugli e l'erba che si vedono sullo schermo, senza propagarsi
void HdBurn_IgniteScreen(void);
void HdBurn_FreezeScreen(void);
// Il cespuglio o l'erba appena tagliati o sollevati, con i resti nel punto
// (x, y), erano congelati? (allora vanno in pezzi come un vaso, in blu)
bool HdBurn_TakeFrozen(uint16 x, uint16 y);
// C'e' erba o un cespuglio congelati nel punto (x, y) del mondo esterno?
bool HdBurn_IsFrozenAt(uint16 x, uint16 y);
// Fa avanzare l'incendio: una volta per frame
void HdBurn_Update(void);
// Spegne tutto e dimentica le tile annerite e congelate: entrando in una casa
// o caricando un salvataggio il gioco fa ricrescere tutti i cespugli
void HdBurn_Reset(void);

// Per il disegno delle fiamme e del ghiaccio (hd_fx.c) e della luce (hd_light.c)
enum {
  kHdBurnTime = 66,      // frame in cui una tile resta in fiamme
  kHdScorchTime = 420,   // frame in cui l'annerimento passa da fresco a definitivo
  kHdMaxBurning = 512,   // tile in fiamme insieme (il Medaglione della Fiamma brucia lo schermo)
};
typedef struct HdBurnTile {
  uint16 x, y;   // angolo in alto a sinistra della tile 16x16, in pixel del mondo
  uint16 age;    // frame passati da quando ha preso fuoco (o da quando si e' spenta o congelata)
  uint8 seed;    // varia la forma delle fiamme da una tile all'altra
  uint8 area;    // area esterna della tile
  uint8 remote;  // bruciata in un'area vicina: il segreto si scopre entrando nell'area
} HdBurnTile;
// Tile in fiamme, tile gia' bruciate (annerite) e tile congelate di tutte le
// aree: quelle da disegnare sono quelle per cui HdBurn_TileShown e' vero
int HdBurn_GetBurning(const HdBurnTile **tiles);
int HdBurn_GetScorched(const HdBurnTile **tiles);
int HdBurn_GetFrozen(const HdBurnTile **tiles);
// Col mondo unico tutte le tile dello stesso mondo, altrimenti quelle dell'area
bool HdBurn_TileShown(const HdBurnTile *t);

#endif  // ZELDA3_HD_BURN_H_
