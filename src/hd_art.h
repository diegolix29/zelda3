// Aspetto dei disegni ufficiali: colori di Agahnim e del re come in A Link to
// the Past Redux, medaglione giapponese; Link biondo a parte (hd_art_data.h)
#ifndef ZELDA3_HD_ART_H_
#define ZELDA3_HD_ART_H_

#include "types.h"

// Applica o toglie Link biondo (caselle e palette in memoria) secondo
// g_config.blond_link; non tocca un Link personalizzato (LinkGraphics)
void HdArt_Apply(void);
// Da Decomp_bg: sostituisce le caselle cambiate della tavola (il medaglione)
void HdArt_PatchBgSheet(uint8 *dst, int gfx);
// Palette di Agahnim (kPalette_SpriteAux1 numero 14) da usare
const uint16 *HdArt_AgahnimPalette(const uint16 *src);
// Attributo OAM del re di Hyrule nella prima scena del finale
uint8 HdArt_KingOamFlags(uint8 flags);

#endif  // ZELDA3_HD_ART_H_
