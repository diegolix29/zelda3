// Acqua come in Four Swords Adventures: profondita', schiuma, superficie
// che si muove, cascate che scorrono con nebbia (opzione FxWater)
#ifndef ZELDA3_HD_WATER_H_
#define ZELDA3_HD_WATER_H_

#include "types.h"

extern bool g_fx_water;

struct Ppu;
// Prima e dopo il disegno della PPU: raccoglie gli sprite trasparenti sommati
// allo schermo secondario (Ppu.objMathOut), che si vedono sopra l'acqua HD
void HdWater_BeforeRender(struct Ppu *ppu, int width, int height);
void HdWater_AfterRender(struct Ppu *ppu);
// Prima di ombre e luci: copia dell'immagine, per tenere le luci sull'acqua nuova
void HdWater_BeforeEffects(uint8 *pixels, int pitch, int width, int height);
// Dopo HdFx_MarkWater: ridisegna l'acqua (layer 7) e le cascate
void HdWater_Apply(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);

// Acqua della chiusa (stanza della diga): il gioco la disegna sul piano di
// sopra senza cambiare gli attributi delle caselle. Si segnano qui le caselle
// 8x8 (posizione nella mappa 64x64 della stanza) per l'acqua HD
void HdWater_ClearFlood(void);
void HdWater_MarkFlood(int pos);
bool HdWater_IsFlood(int pos);
// Attributo della casella i (0-0x1fff: pavimento, poi piano di sopra) della
// stanza: durante le transizioni la tabella si ricarica a pezzi e fino alla
// fine ha ancora gli attributi della stanza di prima. wx, wy: punto del
// mondo (nello scorrimento fra due stanze conta in quale delle due e')
uint8 HdWater_DungAttr(int i, int wx, int wy);
// Da chiamare una volta per frame prima di HdWater_DungAttr (HdFx_MarkWater)
void HdWater_PrepareAttrs(void);
// Vasche blu dentro le case (caselle BG2 0x1a0-0x1a3, 0x194-0x195 con la
// palette 4): per il gioco sono muro (0x02), l'acqua HD le riempie. pos come
// HdWater_IsFlood; c (0xRRGGBB): solo i pixel blu dell'interno sono acqua
bool HdWater_IsTubCell(int pos);
bool HdWater_IsTubColor(uint32 c);
// Sul Monte della Morte (chiaro e oscuro) il vuoto oltre i dirupi e' "acqua
// profonda" per il gioco, ma sullo schermo e' il cielo con le nuvole (marcate
// anche loro come layer 7): li' l'acqua HD non si disegna e non si congela.
// Vale anche per le aree vicine viste col mondo continuo. wx, wy: punto del mondo
bool HdWater_IsSky(int wx, int wy);

#endif  // ZELDA3_HD_WATER_H_
