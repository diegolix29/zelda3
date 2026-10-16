// Effetti visivi aggiuntivi: braci, raggi dalle finestre, bagliore degli
// oggetti ricevuti, calore sopra la lava, pioggia e lampi.
#ifndef ZELDA3_HD_FX_H_
#define ZELDA3_HD_FX_H_

#include "types.h"

extern bool g_fx_embers, g_fx_window_rays, g_fx_item_glow, g_fx_heat_haze, g_fx_rain;
extern bool g_fx_menu_blur, g_fx_sprite_shadows, g_fx_soft_iris, g_fx_button_hints;

// Prima dell'illuminazione: riconosce la lava e fa tremolare l'aria sopra
void HdFx_BeforeLight(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
// Dopo l'illuminazione: particelle, raggi e bagliori, pioggia e lampi
void HdFx_AfterLight(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);

// Marca i pixel d'acqua (layer 7) per lo shader
void HdFx_MarkWater(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
// All'aperto con il sole (per le ombre delle nuvole dello shader)
bool HdFx_SunnyOutdoors(void);
// Sfoca e scurisce il gioco dietro inventario, dialoghi e menu
void HdFx_MenuBlur(uint8 *pixels, int pitch, int width, int height);
// Cerchio di transizione (entrate e uscite) morbido e su tutta la larghezza
typedef struct Ppu Ppu;
void HdFx_IrisBeforeRender(Ppu *ppu);
void HdFx_IrisAfterRender(Ppu *ppu);
void HdFx_SoftIris(uint8 *pixels, int pitch, int width, int height);
// Cielo e nuvole dietro la Piramide e sul Monte della Morte: lo shader non li dipinge
void HdFx_MarkBackdrop(uint8 *pixels, int pitch, int width, int height);
// Ombre morbide sotto personaggi, nemici e oggetti
void HdFx_SpriteShadows(uint8 *pixels, int pitch, int width, int height);

#endif  // ZELDA3_HD_FX_H_
