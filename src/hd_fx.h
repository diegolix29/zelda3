// Effetti visivi aggiuntivi: braci, raggi dalle finestre, bagliore degli
// oggetti ricevuti, calore sopra la lava, pioggia e lampi.
#ifndef ZELDA3_HD_FX_H_
#define ZELDA3_HD_FX_H_

#include "types.h"
#include "snes/ppu.h"
#include "config.h"

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

// Scales every channel of c by k (truncating), keeping the layer
static inline uint32 HdFx_ScaleRgb(uint32 c, float k) {
  return (c & 0xff000000) | (int)(((c >> 16) & 0xff) * k) << 16 |
         (int)(((c >> 8) & 0xff) * k) << 8 | (int)((c & 0xff) * k);
}

// Luminance weight of a 0xRRGGBB color: r * 3 + g * 6 + b
static inline int HdFx_Luma(uint32 c) {
  return (int)(c >> 16 & 0xff) * 3 + (int)(c >> 8 & 0xff) * 6 + (int)(c & 0xff);
}

// Red-orange like lava (not water or anything else)
static inline bool HdFx_IsLavaRgb(int r, int g, int b) {
  return r > 120 && r > g + 30 && b < r / 2;
}

// HUD pixel (layer 2) or sprite pixel (layers 4 and 6)
static inline bool HdFx_IsHudOrSprite(uint32 c) {
  uint32 l = c >> 24;
  return l == 2 || l == 4 || l == 6;
}

static inline float HdFx_Clamp01(float v) {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

// Smoothstep of v from a to b (0-1)
static inline float HdFx_Smooth(float a, float b, float v) {
  float t = HdFx_Clamp01((v - a) / (b - a));
  return t * t * (3 - 2 * t);
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
// Indirizzo in VRAM della casella (tx, ty) di 0..63 nella mappa di uno strato
// (BgLayer *bg): mappe da 32x32 caselle affiancate come dicono Wider/Higher
#define HdFx_VramMapAdr(bg, tx, ty) \
  ((uint16)((bg)->tilemapAdr + (((ty) & 31) << 5 | ((tx) & 31)) + \
            (((tx) & 32) && (bg)->tilemapWider ? 0x400 : 0) + \
            (((ty) & 32) && (bg)->tilemapHigher ? ((bg)->tilemapWider ? 0x800 : 0x400) : 0)))

static inline int HdFx_Pixel4bpp(uint16 p01, uint16 p23, int bit) {
  return (p01 >> bit & 1) | (p01 >> (8 + bit) & 1) << 1 | (p23 >> bit & 1) << 2 | (p23 >> (8 + bit) & 1) << 3;
}

// Color (palette * 16 + index, 0 = transparent) of the 4bpp layer bg at map
// point (px, py), as the PPU draws it (tile flips included)
static inline int HdFx_BgTilePixel(const Ppu *ppu, const BgLayer *bg, int px, int py) {
  px &= 0x3ff, py &= 0x3ff;
  uint16 e = ppu->vram[HdFx_VramMapAdr(bg, px >> 3 & 63, py >> 3 & 63) & 0x7fff];
  int row = (e & 0x8000) ? 7 - (py & 7) : (py & 7);
  int bit = (e & 0x4000) ? (px & 7) : 7 - (px & 7);
  int base = bg->tileAdr + (e & 0x3ff) * 16 + row;
  int v = HdFx_Pixel4bpp(ppu->vram[base & 0x7fff], ppu->vram[(base + 8) & 0x7fff], bit);
  return v ? (e >> 10 & 7) * 16 + v : 0;
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
bool HdFx_DungeonRoomRuns(void);
// Si gioca all'aperto (9/0): niente menu, transizioni o scene
bool HdFx_LiveOverworld(void);
// Una partita e' caricata: non titolo, scelta del file o dimostrazione
bool HdFx_GameLoaded(void);
// Labirinto in una fase diversa dal gioco (passaggi fra stanze, scale...)
bool HdFx_DungeonPassing(void);
// Stanza le cui caselle sono in dung_bg2/dung_bg1 (e torce, oggetti): nei
// passaggi (scale, scorrimenti, buche) il gioco cambia dungeon_room_index
// prima di caricare la stanza nuova, e sullo schermo resta quella di prima.
// Unica regola per tutto cio' che legge le caselle della stanza (luci, raggi,
// buche, acqua): non usare dungeon_room_index per questo
int HdFx_LoadedRoom(void);
// Scritta a fine Dungeon_LoadRoom (dungeon.c); -1 = non si sa
extern int g_hd_loaded_room;
// Scorrimento verso un'altra stanza (7/2) o l'altra meta' della stanza (7/1)
bool HdFx_RoomScroll(void);
// Si vede il gioco (HdFx_SceneModule 7, 9 o 11): unico controllo per acqua,
// luci, particelle e ombre. *mod_out: il modulo della scena
bool HdFx_InGameplay(int *mod_out);
int HdFx_EndingScene(void);
// Marca i pixel d'acqua (layer 7) per lo shader
void HdFx_MarkWater(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
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
// How much of the menu blur is on screen (0 = none, 1 = full)
float HdFx_MenuBlurAmount(void);
// Cerchio di transizione (entrate e uscite) morbido e su tutta la larghezza
typedef struct Ppu Ppu;
// Blocchi spinti e lapidi in movimento: le n voci OAM da oam in poi si
// disegnano con il layer kPpuLayerObjAsBg (Ppu.objAsBg), lo shader le dipinge
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
// all'aperto senza mondo continuo: casella map8 che la PPU mostra nel punto del
// mondo fuori dall'area corrente (nel cambio d'area e' la parte vecchia, che
// la mappa del gioco non ha piu'); -1 se il punto e' nell'area o non si usa
int HdFx_OwShownEntry(int wx, int wy);
void HdFx_FadeMask(uint8 *pixels, int pitch, int width, int height);
void HdFx_FadePost(uint8 *pixels, int pitch, int width, int height);
// Mosaico del gioco anche sulle aggiunte HD (stessi quadrati della PPU)
void HdFx_Mosaic(uint8 *pixels, int pitch, int width, int height);
// Cielo e nuvole dietro la Piramide e sul Monte della Morte: lo shader non li dipinge
void HdFx_MarkBackdrop(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);
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

// After loading a quick save (zelda_rtl.c LoadSnesState)
void HdFx_ResetAfterLoad(void);

// Screen brightness of the frame being drawn (INIDISP, 0 = black or forced
// blank): the PPU and the HD effects draw at 15 and the picture is dimmed at
// the end (ZeldaDrawPpuFrame). For decisions, not for colours
extern int g_hd_screen_bri;

#endif  // ZELDA3_HD_FX_H_
