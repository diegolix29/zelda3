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
// Solo l'acqua: congela (una lastra per tile, tre affiancate sotto il colpo della
// Bacchetta del Ghiaccio) o scioglie la tile d'acqua nel punto (x, y)
void HdBurn_FreezeWater(uint16 x, uint16 y);
void HdBurn_MeltWater(uint16 x, uint16 y);
// Brucia (Medaglione della Fiamma) o congela (Medaglione dell'Etere) insieme
// tutti i cespugli, l'erba e l'acqua che si vedono sullo schermo, senza
// propagarsi
void HdBurn_IgniteScreen(void);
void HdBurn_FreezeScreen(void);
// Il cespuglio o l'erba appena tagliati o sollevati, con i resti nel punto
// (x, y), erano congelati? (allora vanno in pezzi come un vaso, in blu)
bool HdBurn_TakeFrozen(uint16 x, uint16 y);
// C'e' erba, un cespuglio o acqua congelati nel punto (x, y) del mondo esterno?
bool HdBurn_IsFrozenAt(uint16 x, uint16 y);
// Celle 8x8 d'acqua della tile 16x16 in (x, y): bit 0 alto a sinistra, 1 alto
// a destra, 2 basso a sinistra, 3 basso a destra
uint8 HdBurn_WaterCells(uint16 x, uint16 y);
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
  // Acqua ghiacciata: dalla Bacchetta del Ghiaccio 7 secondi, dall'Etere 14
  // (cespugli, erba e nemici congelati invece non si sciolgono da soli)
  // kHdIceMelting: preso dal fuoco, si scioglie a chiazze come a fine tempo
  // ma in kHdBurnTime frame, quanto dura una fiamma
  kHdIceRod = 1, kHdIceEther = 2, kHdIceMelting = 3,
  kHdRodIceTime = 420,
  kHdEtherIceTime = 840,
  kHdThawTime = 120,       // negli ultimi 2 secondi il ghiaccio si scioglie a vista
  kHdMeltStart = 8,        // eta' da cui parte lo scioglimento col fuoco (lastra gia' formata)
};
typedef struct HdBurnTile {
  uint16 x, y;   // angolo in alto a sinistra della tile 16x16, in pixel del mondo
  uint16 age;    // frame passati da quando ha preso fuoco (o da quando si e' spenta o congelata)
  uint8 seed;    // varia la forma delle fiamme da una tile all'altra
  uint8 area;    // area esterna della tile
  uint8 remote;  // bruciata in un'area vicina: il segreto si scopre entrando nell'area
  uint8 water;   // acqua ghiacciata (kHdIceRod, kHdIceEther o kHdIceMelting; 0 = cespuglio o erba)
} HdBurnTile;
// Frame che restano all'acqua ghiacciata prima di tornare acqua
static inline int HdBurn_IceLeft(const HdBurnTile *t) {
  return (t->water == kHdIceMelting ? kHdMeltStart + kHdBurnTime :
          t->water == kHdIceEther ? kHdEtherIceTime : kHdRodIceTime) - t->age;
}
// Frame in cui il ghiaccio si scioglie a vista (a fine tempo o col fuoco)
static inline int HdBurn_ThawTime(const HdBurnTile *t) {
  return t->water == kHdIceMelting ? kHdBurnTime : kHdThawTime;
}
// Tile in fiamme, tile gia' bruciate (annerite) e tile congelate di tutte le
// aree: quelle da disegnare sono quelle per cui HdBurn_TileShown e' vero
int HdBurn_GetBurning(const HdBurnTile **tiles);
int HdBurn_GetScorched(const HdBurnTile **tiles);
int HdBurn_GetFrozen(const HdBurnTile **tiles);
// Col mondo unico tutte le tile dello stesso mondo, altrimenti quelle dell'area
bool HdBurn_TileShown(const HdBurnTile *t);

#endif  // ZELDA3_HD_BURN_H_
