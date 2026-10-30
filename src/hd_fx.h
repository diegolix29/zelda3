// Effetti visivi aggiuntivi: braci, raggi dalle finestre, bagliore degli
// oggetti ricevuti, calore sopra la lava, pioggia e lampi.
#ifndef ZELDA3_HD_FX_H_
#define ZELDA3_HD_FX_H_

#include "types.h"

extern bool g_fx_heat, g_fx_window_rays, g_fx_item_glow, g_fx_rain;
extern bool g_fx_menu_blur, g_fx_sprite_shadows, g_fx_soft_iris;
// Bagliori magici bianchi sopra i vortici di teletrasporto ("Distingui vortici da Zora")
extern bool g_fx_whirlpool;
// Nebbia del Bosco Perduto migliorata (prima della Spada Suprema)
extern bool g_fx_fog;
// Frame di gioco dall'avvio (per le animazioni: frame_counter si azzera ogni 256)
extern uint32 g_fx_frames;

// ---------------------------------------------------------------------------
// Colori dei pixel dello schermo: 0xLLRRGGBB, LL = strato (2 = HUD, 4 e 6 =
// sprite...). Le funzioni tengono lo strato del pixel

static inline int HdFx_ClampByte(int v) {
  return v < 0 ? 0 : v > 255 ? 255 : v;
}

// Sposta il colore c verso (r, g, b) della frazione a (troncando)
static inline uint32 HdFx_MixRgb(uint32 c, int r, int g, int b, float a) {
  int cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
  cr += (int)((r - cr) * a), cg += (int)((g - cg) * a), cb += (int)((b - cb) * a);
  return (c & 0xff000000) | HdFx_ClampByte(cr) << 16 | HdFx_ClampByte(cg) << 8 | HdFx_ClampByte(cb);
}

// Somma (r, g, b) * a al colore c (luce additiva)
static inline uint32 HdFx_AddRgb(uint32 c, float r, float g, float b, float a) {
  int cr = HdFx_ClampByte(((c >> 16) & 0xff) + (int)(r * a));
  int cg = HdFx_ClampByte(((c >> 8) & 0xff) + (int)(g * a));
  int cb = HdFx_ClampByte((c & 0xff) + (int)(b * a));
  return (c & 0xff000000) | cr << 16 | cg << 8 | cb;
}

// Colore BGR555 dello SNES in 0xRRGGBB come la PPU (5 bit -> 8: c<<3 | c>>2)
static inline uint32 HdFx_Bgr15ToRgb(uint16 c) {
  uint32 r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
  return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

// Luminosita' bri (0-15) come INIDISP: ogni canale * bri / 15
static inline uint32 HdFx_DimRgb(uint32 c, int bri) {
  uint32 r = (c >> 16 & 255) * bri / 15, g = (c >> 8 & 255) * bri / 15, b = (c & 255) * bri / 15;
  return (c & 0xff000000) | r << 16 | g << 8 | b;
}

// Colore (0-3) del pixel (riga 0-15, colonna 0-7) del simbolo c nel font dei
// dialoghi del gioco (blocco 0 di dialogue_font_blk: due tile 2bpp una sopra
// l'altra, a 16 tile di distanza)
static inline int HdFx_GameGlyphPixel(const uint8 *font, int c, int row, int col) {
  int tile = (c & 0x70) * 2 + (c & 0xf) + (row < 8 ? 0 : 16);
  const uint8 *p = font + tile * 16 + (row & 7) * 2;
  return (p[0] >> (7 - col) & 1) | (p[1] >> (7 - col) & 1) << 1;
}

// Colore (0-15) di un pixel di una tile 4bpp: p01 e p23 sono le due parole
// della riga (piani 0-1 e 2-3, come nella VRAM), bit = 7 - colonna
static inline int HdFx_Pixel4bpp(uint16 p01, uint16 p23, int bit) {
  return (p01 >> bit & 1) | (p01 >> (8 + bit) & 1) << 1 | (p23 >> bit & 1) << 2 | (p23 >> (8 + bit) & 1) << 3;
}

// Prima dell'illuminazione: riconosce la lava e fa tremolare l'aria sopra
void HdFx_BeforeLight(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
// Dopo l'illuminazione: particelle, raggi e bagliori, pioggia e lampi
void HdFx_AfterLight(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);

// Modulo della scena che si vede sullo schermo (per lo shader e per gli
// effetti sul fondale): 7 = dentro, 9 = fuori, 14 = un menu che copre il gioco.
// Unica regola per shader, effetti HD, schermo largo, HUD moderno e mondo
// continuo: un modulo in cui il gioco resta in vista va aggiunto solo qui
int HdFx_SceneModule(void);
// Morte di Link con il mondo ancora (o di nuovo) sullo schermo
bool HdFx_DeathShowsWorld(void);
// Gioco in vista ma fermo sotto menu, dialoghi, pozioni, dissolvenze...
// (HdFx_SceneModule): telecamere e vista non si muovono
bool HdFx_GameHeldInView(void);
// Si gioca all'aperto (9/0): niente menu, transizioni o scene
bool HdFx_LiveOverworld(void);
// Una partita e' caricata: non titolo, scelta del file o dimostrazione
bool HdFx_GameLoaded(void);
// Labirinto in una fase diversa dal gioco (passaggi fra stanze, scale...)
bool HdFx_DungeonPassing(void);
// Scorrimento verso un'altra stanza (7/2) o l'altra meta' della stanza (7/1)
bool HdFx_RoomScroll(void);
// Si vede il gioco (HdFx_SceneModule 7, 9 o 11): unico controllo per acqua,
// luci, particelle e ombre. *mod_out: il modulo della scena
bool HdFx_InGameplay(int *mod_out);
// Marca i pixel d'acqua (layer 7) per lo shader
void HdFx_MarkWater(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
bool HdFx_FloodWindowShows(int sx, int sy);
// Disegna l'acqua ghiacciata dalla Bacchetta del Ghiaccio (hd_burn.c), sopra
// l'acqua di hd_water.c
void HdFx_FrozenWater(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
// Schiarimento del lampo, dopo l'acqua HD e il ghiaccio
void HdFx_Lightning(uint8 *pixels, int pitch, int width, int height);
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
// Blocchi spinti e lapidi in movimento: le n voci OAM da oam in poi si
// disegnano con il layer del fondale (Ppu.objAsBg), lo shader le dipinge
void HdFx_OamAsBackground(const void *oam, int n);
void HdFx_AsBgBeforeRender(Ppu *ppu);
void HdFx_IrisBeforeRender(Ppu *ppu);
void HdFx_IrisAfterRender(Ppu *ppu);
void HdFx_SoftIris(uint8 *pixels, int pitch, int width, int height);
// Dissolvenze delle palette: la PPU disegna il fondale con i colori veri, le
// aggiunte HD lavorano su quelli e alla fine si scurisce tutto come il gioco
void HdFx_FadeBeforeRender(Ppu *ppu);
void HdFx_FadeAfterRender(Ppu *ppu);
bool HdFx_FadeActive(void);
// colore della palette per gli effetti: durante la dissolvenza quello vero
uint16 HdFx_RealBg(const uint16 *pal, int i);
void HdFx_FadeMask(uint8 *pixels, int pitch, int width, int height);
void HdFx_FadePost(uint8 *pixels, int pitch, int width, int height);
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
// Link sott'acqua al chiuso: la PPU lo mescola con l'acqua come su GBA
void HdFx_DiveBeforeRender(Ppu *ppu);
// Super bomba che si gonfia e si sgonfia come su GBA (scala della sua voce OAM)
void HdFx_SuperBombBeforeRender(Ppu *ppu);
// Nebbia del Bosco Perduto migliorata: prima del disegno della PPU toglie
// quella del gioco, dopo la rimette nei registri; HdFx_Fog la disegna
void HdFx_FogBeforeRender(Ppu *ppu);
void HdFx_FogAfterRender(Ppu *ppu);
void HdFx_Fog(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
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
