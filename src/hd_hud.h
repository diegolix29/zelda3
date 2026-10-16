// HUD moderno: cuori e magia a sinistra, tasti Y/A/B a destra come sul
// controller, rupie e chiavi in basso a sinistra, scritta dell'azione del
// tasto A (Parla, Leggi, Apri...). Ridisegna i pezzi dell'HUD originale.
#ifndef ZELDA3_HD_HUD_H_
#define ZELDA3_HD_HUD_H_

#include "types.h"

typedef struct Ppu Ppu;

// Prima del disegno della PPU: nasconde l'HUD originale quando serve
void HdHud_BeforeRender(Ppu *ppu);
// Disegna l'HUD moderno nel frame (alla risoluzione SNES moltiplicata per scale)
void HdHud_Draw(uint8 *pixels, int pitch, int width, int height, int scale);
// Chiamata dai personaggi che parlano quando Link e' nella posizione giusta
void HdHud_CanTalk(void);

#endif  // ZELDA3_HD_HUD_H_
