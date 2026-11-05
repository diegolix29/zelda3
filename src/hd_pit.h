#ifndef ZELDA3_HD_PIT_H_
#define ZELDA3_HD_PIT_H_

#include "types.h"

// Stanza di sotto, sfocata e in profondita', dentro le buche dei labirinti
extern bool g_fx_pit;

// Prima del disegno della PPU: costruisce l'immagine della stanza di sotto
// entrando in una stanza con buche
void HdPit_BeforeRender(void);
// Dopo il disegno: i pixel "vuoto" delle buche prendono la stanza di sotto
void HdPit_Apply(uint8 *pixels, int pitch, int width, int height);
// Fine di Dungeon_LoadRoom: le caselle in memoria sono della stanza attuale
void HdPit_RoomLoaded(void);

#endif  // ZELDA3_HD_PIT_H_
