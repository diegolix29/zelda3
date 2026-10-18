// Mondo continuo all'esterno (opzione ContinuousWorld): oltre il bordo
// dell'area si vedono le aree vicine e la telecamera segue Link fino al bordo.
#ifndef ZELDA3_HD_WORLD_H_
#define ZELDA3_HD_WORLD_H_

#include "types.h"

// Attivo in questo frame (all'esterno, nel Mondo della Luce o delle Tenebre)
bool HdWorld_Active(void);
// Una volta per frame di gioco: posizione della telecamera "libera", cioe'
// quella che il gioco avrebbe se non si fermasse ai bordi dell'area
void HdWorld_UpdateCamera(int *cam_x, int *cam_y);
// Ai quattro bordi della mappa del mondo la vista si ferma come nel gioco:
// riporta dentro la posizione (x, y) della telecamera se mostrerebbe il vuoto
void HdWorld_ClampView(int *cam_x, int *cam_y);
// Dopo il disegno della PPU: il fondale fuori dall'area corrente (e durante
// il passaggio fra aree, tutto il fondale) viene preso dalle immagini delle aree
void HdWorld_Composite(uint8 *pixels, int pitch, int width, int height);
// Vero se il punto del mondo e' dentro l'area corrente
bool HdWorld_InsideCurrentArea(int wx, int wy);
// Vero se in questo frame tutto il fondale viene dalle immagini delle aree
// (passaggio fra aree): gli strati del gioco li' sono solo da coprire
bool HdWorld_BackgroundFromImages(void);

// Mondo persistente (cespugli, sassi, buche, nemici sconfitti per area)
void HdWorld_ResetStates(void);
// Dopo aver decompresso l'area corrente: riscrive le caselle ricordate
void HdWorld_EnterAreaTiles(bool memorize);
// Dopo che il gioco ha azzerato la sua lista: la riempie di nuovo
void HdWorld_RememberTiles(void);
// Dopo aver letto i nemici dell'area: quelli sconfitti non ricompaiono
void HdWorld_RestoreKilled(void);
// Erba alta tagliata e buche della pala (che il gioco non memorizza)
void HdWorld_RememberExtraTile(uint16 pos, uint16 value);

#endif  // ZELDA3_HD_WORLD_H_
