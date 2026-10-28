// Effetti visivi aggiuntivi: braci, raggi dalle finestre, bagliore degli
// oggetti ricevuti, calore sopra la lava, pioggia e lampi.
#ifndef ZELDA3_HD_FX_H_
#define ZELDA3_HD_FX_H_

#include "types.h"

extern bool g_fx_heat, g_fx_window_rays, g_fx_item_glow, g_fx_rain;
extern bool g_fx_menu_blur, g_fx_sprite_shadows, g_fx_soft_iris, g_fx_button_hints;
// Bagliori magici bianchi sopra i vortici di teletrasporto ("Distingui vortici da Zora")
extern bool g_fx_whirlpool;
// Nebbia del Bosco Perduto migliorata (prima della Spada Suprema)
extern bool g_fx_fog;
// Frame di gioco dall'avvio (per le animazioni: frame_counter si azzera ogni 256)
extern uint32 g_fx_frames;

// Prima dell'illuminazione: riconosce la lava e fa tremolare l'aria sopra
void HdFx_BeforeLight(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
// Dopo l'illuminazione: particelle, raggi e bagliori, pioggia e lampi
void HdFx_AfterLight(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);

// Modulo della scena che si vede sullo schermo (per lo shader e per gli
// effetti sul fondale): 7 = dentro, 9 = fuori, 14 = un menu che copre il gioco
int HdFx_SceneModule(void);
// Marca i pixel d'acqua (layer 7) per lo shader
void HdFx_MarkWater(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
bool HdFx_FloodWindowShows(int sx, int sy);
// Disegna l'acqua ghiacciata dalla Bacchetta del Ghiaccio (hd_burn.c), sopra
// l'acqua di hd_water.c
void HdFx_FrozenWater(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
// Schiarimento del lampo, dopo l'acqua HD e il ghiaccio
void HdFx_Lightning(uint8 *pixels, int pitch, int width, int height);
// Vista 3D: pioggia HD e lampo disegnati piatti dopo la proiezione (hd_3d.c)
extern bool g_fx_flat_weather;
void HdFx_FlatWeather(uint8 *pixels, int pitch, int width, int height);
// Quanto e' acceso il lampo (0-1), per lo shader
float HdFx_LightningWhite(void);
// All'aperto con il sole (per le ombre delle nuvole dello shader)
bool HdFx_SunnyOutdoors(void);
// Sfoca e scurisce il gioco dietro inventario, dialoghi e menu
void HdFx_MenuBlur(uint8 *pixels, int pitch, int width, int height);
// Sfocatura in corso: lo shader non si applica finche' c'e'
bool HdFx_MenuBlurActive(void);
// Cerchio di transizione (entrate e uscite) morbido e su tutta la larghezza
typedef struct Ppu Ppu;
void HdFx_IrisBeforeRender(Ppu *ppu);
void HdFx_IrisAfterRender(Ppu *ppu);
void HdFx_SoftIris(uint8 *pixels, int pitch, int width, int height);
// Mosaico del gioco anche sulle aggiunte HD (stessi quadrati della PPU)
void HdFx_Mosaic(uint8 *pixels, int pitch, int width, int height);
// Cielo e nuvole dietro la Piramide e sul Monte della Morte: lo shader non li dipinge
void HdFx_MarkBackdrop(uint8 *pixels, int pitch, int width, int height);
// Ombre morbide sotto Link e i nemici che camminano a terra
void HdFx_SpriteShadows(uint8 *pixels, int pitch, int width, int height);
// Prima e dopo il disegno della PPU: sceglie gli sprite che fanno ombra e
// nasconde l'ombra del gioco dei nemici a terra
void HdFx_ShadowsBeforeRender(Ppu *ppu, int width, int height);
void HdFx_ShadowsAfterRender(Ppu *ppu);
// Cespugli congelati sollevati o lanciati e i loro pezzi (hd_burn.c): la PPU
// li disegna con i blu dei nemici congelati
void HdFx_IceBeforeRender(Ppu *ppu);
// Super bomba che si gonfia e si sgonfia come su GBA (scala della sua voce OAM)
void HdFx_SuperBombBeforeRender(Ppu *ppu);
// Nebbia del Bosco Perduto migliorata: prima del disegno della PPU toglie
// quella del gioco, dopo la rimette nei registri; HdFx_Fog la disegna
void HdFx_FogBeforeRender(Ppu *ppu);
void HdFx_FogAfterRender(Ppu *ppu);
void HdFx_Fog(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
// Vista 3D: la nebbia dell'ultimo HdFx_Fog sui pixel che non l'hanno avuta
// (kind 2 = terreno oltre lo schermo, 3 = sprite), nel punto uv dello schermo 2D
void HdFx_FogProjected(uint8 *pixels, int pitch, int width, int height, const uint32 *uv, const uint8 *kind);
// In questo frame la nebbia del gioco e' sostituita (hd_world.c non la somma)
bool HdFx_FogReplacesOverlay(void);
// Nebbia del bosco sullo schermo (anche quella del gioco): lo shader Four
// Swords non si applica, rovinerebbe l'atmosfera
bool HdFx_ForestFogScene(void);
// Piove nell'area esterna attuale
bool HdFx_IsRainingOutdoors(void);
// Sulla Montagna della Morte: la casella e (della mappa) puo' essere nuvola,
// e il suo colore v e' nuvola
bool HdFx_IsCloudTile(uint16 e);
bool HdFx_IsCloudColor(uint16 e, int v);

#endif  // ZELDA3_HD_FX_H_
