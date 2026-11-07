#ifndef ZELDA3_HD_PIT_H_
#define ZELDA3_HD_PIT_H_

#include "types.h"


// Prima del disegno della PPU: costruisce l'immagine della stanza di sotto
// entrando in una stanza con buche
void HdPit_BeforeRender(void);
// Dopo il disegno: i pixel "vuoto" delle buche prendono la stanza di sotto
void HdPit_Apply(uint8 *pixels, int pitch, int width, int height);

// After loading a quick save (zelda_rtl.c LoadSnesState)
void HdPit_ResetAfterLoad(void);

#endif  // ZELDA3_HD_PIT_H_
