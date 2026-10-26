// Mondo continuo all'esterno (opzione ContinuousWorld).
//
// Il gioco tiene in memoria una sola area alla volta e ferma la telecamera ai
// suoi bordi. Qui la logica del gioco resta intatta:
// - la telecamera "libera" segue Link con la stessa regola del gioco ma senza
//   fermarsi ai bordi; la differenza rispetto alla telecamera del gioco viene
//   applicata solo al disegno, come per la telecamera fluida;
// - le aree vicine vengono disegnate in immagini: si salva lo stato (RAM e
//   memoria video), si carica l'area con le funzioni del gioco (grafica,
//   colori, mappa delle caselle), si disegna il terreno e si ripristina tutto;
// - nel frame disegnato, il fondale fuori dall'area corrente viene preso da
//   quelle immagini. Durante il passaggio fra due aree, quando la memoria
//   video contiene un misto delle due, tutto il fondale viene dalle immagini;
// - i nemici delle aree vicine visibili si muovono dal vivo, ognuno in una
//   copia dello stato del gioco preparata per la sua area (piu' sotto).
// I labirinti e le case restano a schermate, come nel gioco.
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "hd_world.h"
#include "zelda_rtl.h"
#include "config.h"
#include "variables.h"
#include "features.h"
#include "assets.h"
#include "overworld.h"
#include "load_gfx.h"
#include "snes/ppu.h"
#include "hd_fx.h"
#include "hd_burn.h"
#include "sprite.h"
#include "ancilla.h"
#include "misc.h"
#include "nmi.h"
#include "hd_menu.h"
#include "hd_water.h"

uint8 Overworld_AreaHead(int square);
extern int g_cam_dx, g_cam_dy;          // vista rispetto alla telecamera del gioco (zelda_rtl.c)
extern uint8 g_sprite_oam_owner[128];   // sprite.c
extern uint8 g_link_oam_mask[128];      // player_oam.c

static bool FeatureOn(void) {
  return (enhanced_features0 & kFeatures0_ContinuousWorld) != 0;
}

// Dialoghi, inventario, pozioni e menu di salvataggio aperti all'aperto: il
// mondo resta visibile dietro e la vista libera resta ferma dov'era
static bool InMenuOverWorld(void) {
  int sub = submodule_index;
  // mappa (7) e Ocarina (10): nella fase 0 il mondo si scurisce ancora visibile
  return main_module_index == 14 && saved_module_for_menu == 9 &&
         (sub == 1 || sub == 2 || sub == 4 || sub == 8 || sub == 9 || sub == 11 ||
          ((sub == 7 || sub == 10) && overworld_map_state == 0));
}

// Arrivo con l'Ocarina: dalla fase 7 l'area di arrivo e' caricata e compare
// schiarendosi, poi l'uccello posa Link. La vista libera deve essere gia' al
// suo posto, non spostarsi dopo
static bool InFluteArrival(void) {
  return main_module_index == 14 && submodule_index == 10 && overworld_map_state >= 7;
}

// Cerchio che si chiude entrando in una casa (15) o si apre uscendone (16).
// Con il cerchio morbido (hd_fx.c), disegnato sopra a tutto, il mondo resta
// continuo anche li': la vista e' gia' al suo posto quando il cerchio si apre
// e resta dov'era quando si chiude, invece di scivolare o saltare.
static bool InSoftIris(void) {
  return g_fx_soft_iris && (main_module_index == 15 || main_module_index == 16) &&
         !player_is_indoors;
}

// Ritorno dalla mappa del mondo: il gioco ricarica le sovrapposizioni e
// schiarisce lo schermo (sottomoduli 32-34) prima di ridare il controllo
static bool InMapReturn(void) {
  return main_module_index == 9 && submodule_index >= 32 && submodule_index <= 34;
}

bool HdWorld_Active(void) {
  if (!FeatureOn() || overworld_screen_index >= 0x80)
    return false;
  if (main_module_index != 9 && !InMenuOverWorld() && !InFluteArrival() && !InSoftIris())
    return false;
  return g_zenv.ppu->mode != 7;
}

// Luminosita' dello schermo (registro INIDISP, 0-15): il gioco la usa per le
// dissolvenze, per esempio all'arrivo con l'Ocarina. La PPU la applica ai
// suoi pixel; a quelli presi dalle immagini delle aree si applica qui.
static int g_bright = 15;

// Filtro dei colori del mulinello (Module09_2E_Whirlpool): il gioco aggiunge
// blu a tutta la palette, poi toglie rosso e verde e li rimette. Non e' una
// dissolvenza uniforme: per ogni canale si confrontano i colori della palette
// (aux_palette_buffer, quelli dell'area) con quelli filtrati
// (main_palette_buffer) e si applica la stessa differenza ai pixel presi dalle
// immagini delle aree, altrimenti nello schermo largo le colonne oltre il
// bordo dell'area restano con i colori normali
static bool g_pal_map_on;
static int16 g_pal_add[3][32];  // per canale e valore a 5 bit: da aggiungere (0-255)

static void PreparePaletteMap(void) {
  g_pal_map_on = false;
  if (main_module_index != 9 || submodule_index != 0x2e)
    return;
  int sum[3][32] = { { 0 } }, cnt[3][32] = { { 0 } };
  bool same = true;
  for (int i = 0x20; i < 0x100; i++) {
    uint16 c = main_palette_buffer[i], a = aux_palette_buffer[i];
    if (c != a)
      same = false;
    for (int k = 0; k < 3; k++) {
      int av = a >> (5 * k) & 31;
      sum[k][av] += (c >> (5 * k) & 31) - av;
      cnt[k][av]++;
    }
  }
  if (same)
    return;
  for (int k = 0; k < 3; k++) {
    // differenza media per ogni valore; i valori senza colori nella palette
    // prendono quella dei vicini (interpolata)
    int prev = -1;
    for (int v = 0; v < 32; v++) {
      if (!cnt[k][v])
        continue;
      int d = sum[k][v] * 255 / (31 * cnt[k][v]);
      g_pal_add[k][v] = d;
      if (prev < 0) {
        for (int j = 0; j < v; j++)
          g_pal_add[k][j] = d;
      } else {
        for (int j = prev + 1; j < v; j++)
          g_pal_add[k][j] = g_pal_add[k][prev] + (d - g_pal_add[k][prev]) * (j - prev) / (v - prev);
      }
      prev = v;
    }
    for (int j = prev + 1; j < 32; j++)
      g_pal_add[k][j] = prev < 0 ? 0 : g_pal_add[k][prev];
  }
  g_pal_map_on = true;
}

static inline uint32 Dim(uint32 c) {
  if (g_pal_map_on) {
    int ch[3] = { c & 255, c >> 8 & 255, c >> 16 & 255 };  // blu, verde, rosso
    for (int k = 0; k < 3; k++) {
      // canale k della palette SNES: 0 rosso, 1 verde, 2 blu
      int *p = &ch[2 - k];
      int v = *p + g_pal_add[k][*p >> 3];
      *p = v < 0 ? 0 : v > 255 ? 255 : v;
    }
    c = (c & 0xff000000) | ch[2] << 16 | ch[1] << 8 | ch[0];
  }
  if (g_bright >= 15)
    return c;
  uint32 r = (c >> 16 & 255) * g_bright / 15, g = (c >> 8 & 255) * g_bright / 15, b = (c & 255) * g_bright / 15;
  return (c & 0xff000000) | r << 16 | g << 8 | b;
}

static bool InTransition(void) {
  return main_module_index == 9 && submodule_index >= 1 && submodule_index <= 8;
}

// Passaggio con mosaico e dissolvenza (aree con la nebbia, come il Bosco
// Perduto): il numero dell'area cambia subito, mentre la memoria video resta
// quella vecchia fino al buio
static bool InMosaicTransition(void) {
  // 31: fine dell'uscita da un'area speciale (vedi InSpecialExit)
  return main_module_index == 9 && (submodule_index >= 13 && submodule_index <= 22 || submodule_index == 31);
}

// Uscita da un'area speciale (Dominio di Zora...), modulo 11: l'area nuova
// arriva a schermo nero (37-40), poi il mosaico si scioglie (41). Il mondo
// continuo e' ancora spento, ma la vista va gia' dove sara' nell'area
static bool InSpecialExit(void) {
  return main_module_index == 11 && submodule_index >= 37 && submodule_index <= 41 &&
         FeatureOn() && BYTE(overworld_screen_index) < 0x80 && g_zenv.ppu->mode != 7;
}

bool HdWorld_BackgroundFromImages(void) {
  return HdWorld_Active() && (InTransition() || InMosaicTransition());
}

// ---------------------------------------------------------------------------
// Specchio e caselle di teletrasporto (Module09_MirrorWarp, sottomoduli 0x23
// e 0x2c). Il gioco fa ondeggiare il fondale riga per riga (HDMA sullo
// scorrimento orizzontale di BG1 e BG2) e lo schiarisce fino al bianco
// avvicinando i colori delle palette al bianco di un passo per volta; il
// mondo nuovo arriva a schermo tutto bianco (AnimateMirrorWarp, fase 8).
// Regola fissa per le aggiunte HD: la PPU disegna il frame senza onda e con i
// colori veri, tutte le aggiunte (mondo continuo, acqua, luci, ombre) lavorano
// su quello come sempre, e alla fine HdWorld_WarpPost applica onda e bianco a
// tutto insieme, come avrebbe fatto il gioco (HUD escluso, sprite non
// ondeggiano). Lo shader riceve l'onda di ogni riga e quanto e' bianco lo
// schermo (HdWorld_WarpRowDx, HdWorld_WarpWhite): le aggiunte restano
// visibili per tutta l'animazione e la seguono.
static int16 g_warp_line_hofs[241];
static int8 g_warp_dx[240];   // spostamento di ogni riga dello schermo
static float g_warp_dxf[240]; // lo stesso per lo shader
static int g_warp_white;      // 0-31: quanto il gioco ha aggiunto ai colori
static int g_warp_floor;      // 0-31: livello sotto cui nessun colore scende
static bool g_warp_old_world; // prima del cambio: lo schermo mostra il mondo di prima
static bool g_warp_on;        // animazione in corso in questo frame
static uint16 g_warp_real_pal[256];  // colori di prima dell'animazione
static uint16 g_warp_cgram[256];     // colori della PPU durante il disegno
static uint16 g_warp_bg1, g_warp_bg2;
static uint8 *g_warp_mask;    // 0 fondale, 1 sprite, 2 HUD (dal frame della PPU)
static uint32 *g_warp_row;
static int g_warp_cap;

// Solo con l'acqua realistica o con uno shader: sono loro che ridisegnano lo
// schermo dopo la PPU. Senza, onda e bianco li fa la PPU come nell'originale.
static bool InMirrorWarp(void) {
  return (g_fx_water || Host_HasShader()) && main_module_index == 9 && (submodule_index == 0x23 || submodule_index == 0x2c) &&
         subsubmodule_index >= 1 && !player_is_indoors && BYTE(overworld_screen_index) < 0x80;
}

static bool WarpFiltered(int i) {
  return (i >= 0x20 && i < 0xd8) || (i >= 0xe0 && i < 0xf0);
}

void HdWorld_WarpBeforeRender(void) {
  Ppu *ppu = g_zenv.ppu;
  g_warp_white = g_warp_floor = 0, g_warp_old_world = false;
  g_warp_on = InMirrorWarp();
  memset(g_warp_dx, 0, sizeof(g_warp_dx));
  memset(g_warp_dxf, 0, sizeof(g_warp_dxf));
  if (!g_warp_on) {
    memcpy(g_warp_real_pal, main_palette_buffer, sizeof(g_warp_real_pal));
    return;
  }
  bool loaded = overworld_map_state >= 9;
  if (darkening_or_lightening_screen == 2) {
    // verso il bianco: ogni colore sale di un passo per palette_filter_countdown
    g_warp_white = palette_filter_countdown > 31 ? 31 : palette_filter_countdown;
  } else if (!loaded) {
    g_warp_floor = 31;  // tutto bianco finche' non arrivano i colori nuovi
  } else {
    // dal bianco ai colori nuovi (aux_palette_buffer): ogni colore scende di
    // un passo e si ferma al suo valore, quindi il colore e' il piu' alto fra
    // il suo e un livello comune (lo schermo passa per il grigio)
    for (int i = 0x20; i < 0xd8; i++) {
      uint16 c = main_palette_buffer[i], a = aux_palette_buffer[i];
      for (int k = 0; k < 15; k += 5) {
        int v = c >> k & 31;
        if (v != (a >> k & 31) && v > g_warp_floor)
          g_warp_floor = v;
      }
    }
  }
  g_warp_old_world = overworld_map_state < 7;
  // la PPU disegna con i colori veri: quelli di prima, poi quelli nuovi
  const uint16 *real = loaded ? aux_palette_buffer : g_warp_real_pal;
  memcpy(g_warp_cgram, ppu->cgram, sizeof(g_warp_cgram));
  for (int i = 0; i < 256; i++)
    if (WarpFiltered(i))
      ppu->cgram[i] = real[i];
  ppu->cgram[0] = real[0x20];  // il fondale segue il colore 0x20 (PaletteFilter_StartBlindingWhite)
  g_warp_bg1 = BG1HOFS_copy, g_warp_bg2 = BG2HOFS_copy;
}

// Dopo l'HDMA di ogni riga: si ricorda l'onda e la si toglie
void HdWorld_WarpLine(int line) {
  Ppu *ppu = g_zenv.ppu;
  if ((unsigned)line < 241)
    g_warp_line_hofs[line] = ppu->bgLayer[1].hScroll;
  if (g_warp_on && (HDMAEN_copy & 0xc0)) {
    ppu->bgLayer[0].hScroll = g_warp_bg1;
    ppu->bgLayer[1].hScroll = g_warp_bg2;
  }
}

void HdWorld_WarpAfterRender(void) {
  if (!g_warp_on)
    return;
  memcpy(g_zenv.ppu->cgram, g_warp_cgram, sizeof(g_warp_cgram));
  if (!(HDMAEN_copy & 0xc0))
    return;
  // la riga y dello schermo e' la riga y + 1 della PPU (la riga 0 non si vede)
  for (int y = 0; y < 240; y++) {
    int d = (g_warp_line_hofs[y + 1] - BG2HOFS_copy2) & 0x3ff;
    d = d >= 0x200 ? d - 0x400 : d;
    d = d < -127 ? -127 : d > 127 ? 127 : d;
    g_warp_dx[y] = (int8)d;
    g_warp_dxf[y] = (float)d;
  }
}

static bool WarpBuffers(int n, int width) {
  if (n + width > g_warp_cap) {
    free(g_warp_mask), free(g_warp_row);
    g_warp_mask = malloc(n);
    g_warp_row = malloc((n + width) * 4);
    g_warp_cap = g_warp_mask && g_warp_row ? n + width : 0;
  }
  return g_warp_cap != 0;
}

// Prima delle aggiunte HD: quali pixel sono sprite e HUD (alcuni effetti poi
// segnano i loro pixel come sprite, per lo shader)
void HdWorld_WarpMask(uint8 *pixels, int pitch, int width, int height) {
  if (!g_warp_on || !WarpBuffers(width * height, width))
    return;
  for (int y = 0; y < height; y++) {
    const uint32 *row = (const uint32 *)(pixels + y * pitch);
    uint8 *m = g_warp_mask + y * width;
    for (int x = 0; x < width; x++) {
      uint32 l = row[x] >> 24;
      m[x] = l == 2 ? 2 : (l == 4 || l == 6) ? 1 : 0;
    }
  }
}

static inline uint32 WarpWhiten(uint32 c, int add, int lo) {
  int r = (c >> 16 & 255) + add, g = (c >> 8 & 255) + add, b = (c & 255) + add;
  r = r > 255 ? 255 : r < lo ? lo : r;
  g = g > 255 ? 255 : g < lo ? lo : g;
  b = b > 255 ? 255 : b < lo ? lo : b;
  return (c & 0xff000000) | r << 16 | g << 8 | b;
}

void HdWorld_WarpPost(uint8 *pixels, int pitch, int width, int height) {
  if (!g_warp_on || !g_warp_cap)
    return;
  int add = g_warp_white * 255 / 31, lo = g_warp_floor * 255 / 31;
  bool white = add || lo;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    const uint8 *m = g_warp_mask + y * width;
    int dx = y < 240 ? g_warp_dx[y] : 0;
    if (dx)
      memcpy(g_warp_row, row, width * 4);
    for (int x = 0; x < width; x++) {
      if (m[x] == 2)
        continue;  // l'HUD non ondeggia e non diventa bianco
      uint32 c = row[x];
      if (dx && !m[x]) {
        // il fondale si sposta; dietro gli sprite resta il suo pixel
        int sx = x + dx;
        if (sx >= 0 && sx < width && !m[sx])
          c = g_warp_row[sx];
      }
      row[x] = white ? WarpWhiten(c, add, lo) : c;
    }
  }
}

float HdWorld_WarpWhite(void) {
  int w = g_warp_white > g_warp_floor ? g_warp_white : g_warp_floor;
  return g_warp_on ? w / 31.0f : 0.0f;
}

const float *HdWorld_WarpRowDx(void) {
  return g_warp_dxf;
}

// ---------------------------------------------------------------------------
// Area corrente

static void AreaRect(int head, int *x, int *y, int *size) {
  int i = head & 0x3f;
  *x = kOverworld_OffsetBaseX[i];
  *y = kOverworld_OffsetBaseY[i];
  *size = kOverworldMapIsSmall[i] ? 512 : 1024;
}

bool HdWorld_InsideCurrentArea(int wx, int wy) {
  int x, y, s;
  AreaRect(overworld_screen_index, &x, &y, &s);
  return wx >= x && wx < x + s && wy >= y && wy < y + s;
}

// ---------------------------------------------------------------------------
// Mondo persistente: il gioco dimentica cespugli tagliati, sassi sollevati,
// buche e nemici a ogni cambio d'area. Qui si ricordano per area:
// - le caselle modificate (la lista del gioco memorized_tile_*, che contiene
//   la casella nuova) si copiano a ogni frame e si riscrivono rientrando,
//   prima che il gioco costruisca l'immagine dell'area. Restano finche' Link
//   resta all'aperto;
// - i nemici: quelli sconfitti non ricompaiono, quelli vivi tornano dove
//   erano, con la loro vita e il loro stato (tutti i dati del loro posto nelle
//   tabelle del gioco). Si dimenticano quando Link si allontana di 3 aree.
// - l'aspetto dei nemici vivi l'ultima volta che erano sullo schermo: cosi'
//   si vedono, fermi, oltre il bordo dell'area quando la loro area non si
//   muove dal vivo;
// - le frecce rimaste piantate nei muri (Ancilla0A_ArrowInTheWall): restano
//   come le caselle e si disegnano qui.
// Entrando in una casa o in un labirinto, e caricando un salvataggio, tutto
// il mondo esterno torna com'era.

// Tutte le caselle cambiate (la lista del gioco ha posto solo per 256): erba
// tagliata, buche della pala, cespugli, sassi...
enum { kMaxExtra = 2048 };
enum { kMaxTiles = 256, kMaxKilled = 256, kMaxAlive = 16, kEnemyResetGap = 3 };  // 3 quadrati in mezzo = 4 aree
enum { kMaxArrows = 64 };  // per area; oltre, la piu' vecchia sparisce
enum { kPatchW = 64, kPatchH = 72 };

// Tabelle del gioco con un valore per ogni nemico (indirizzo in g_ram, byte)
typedef struct SpriteField { uint32 addr; uint8 size; } SpriteField;
static const SpriteField kSpriteFields[] = {
  {0xB58,1},{0xB6B,1},{0xB89,1},{0xBA0,1},{0xBB0,1},{0xBC0,2},{0xBE0,1},
  {0xC9A,1},{0xCAA,1},{0xCBA,1},{0xCD2,1},{0xCE2,1},
  {0xD00,1},{0xD10,1},{0xD20,1},{0xD30,1},{0xD40,1},{0xD50,1},{0xD60,1},{0xD70,1},
  {0xD80,1},{0xD90,1},{0xDA0,1},{0xDB0,1},{0xDC0,1},{0xDD0,1},{0xDE0,1},{0xDF0,1},
  {0xE00,1},{0xE10,1},{0xE20,1},{0xE30,1},{0xE40,1},{0xE50,1},{0xE60,1},{0xE70,1},
  {0xE80,1},{0xE90,1},{0xEA0,1},{0xEB0,1},{0xEC0,1},{0xED0,1},{0xEE0,1},{0xEF0,1},
  {0xF00,1},{0xF10,1},{0xF20,1},{0xF30,1},{0xF40,1},{0xF50,1},{0xF60,1},{0xF70,1},
  {0xF80,1},{0xF90,1},
  {0x1F9C2,1},{0x1FA1C,1},{0x1FA2C,1},{0x1FA3C,1},{0x1FA4C,1},
  {0x1F580,1},  // spr_disturbed (vedi "La schermata di ogni nemico")
};
enum { kRecSize = 64 };

typedef struct EnemyRec {
  uint16 blk;
  int x, y, z;
  bool hidden;  // non si vede (Zora sott'acqua): niente aspetto da fermo
  uint8 data[kRecSize];
} EnemyRec;

// Freccia piantata: i pezzi del suo disegno (sprite 8x8 della prima tabella
// OBJ), rispetto alla sua posizione nel mondo
typedef struct ArrowRec {
  uint16 x, y;
  uint8 n, chr[2], flags[2];
  int8 dx[2], dy[2];
} ArrowRec;

typedef struct AreaState {
  bool used;
  uint32 version;  // cambia solo con le caselle (l'immagine va ridisegnata)
  int n_tiles, n_killed, n_alive;
  uint16 tile_pos[kMaxTiles], tile_val[kMaxTiles];
  // tutte le caselle cambiate, anche quelle che il gioco non mette nella sua
  // lista (erba alta tagliata, buche della pala) o che non ci stanno piu'
  int n_extra;
  uint16 extra_pos[kMaxExtra], extra_val[kMaxExtra];
  uint16 killed[kMaxKilled];
  EnemyRec alive[kMaxAlive];
  int n_arrows;
  ArrowRec arrows[kMaxArrows];
} AreaState;

// Aspetto di un nemico, catturato dallo schermo
typedef struct EnemyPatch {
  bool valid;
  uint16 blk;
  int ox, oy;  // angolo rispetto alla posizione del nemico (y meno l'altezza da terra)
  uint32 pix[kPatchW * kPatchH];
} EnemyPatch;

static AreaState g_states[128];
static EnemyPatch g_patches[128][kMaxAlive];
static uint32 g_state_epoch = 1;
static bool g_any_state;

// Nemici che seguono Link fuori dalla loro area (vedi "Nemici che seguono
// Link"): area d'origine e posto nella sua tabella. Stanno nella RAM libera
// dopo le tabelle degli oggetti in volo, cosi' finiscono anche nei
// salvataggi: caricandone uno, o quando l'area d'origine dimentica i suoi
// nemici (Link lontano), l'area non deve rimetterli al loro posto, altrimenti
// ce ne sarebbero due.
#define carried_area ((uint8*)(g_ram+0x1F520))   // 0x80 | area, 0 = nessuna
#define carried_type ((uint8*)(g_ram+0x1F530))
#define carried_blk ((uint16*)(g_ram+0x1F540))

// Frame da cui ogni sprite non guarda il terreno (Sprite_CheckTileProperty),
// nella RAM libera cosi' ogni copia delle aree vicine ha i suoi. Chi non lo
// guarda (corvi, api, pietre degli Octorok...) va dove vuole e passa da
// un'area all'altra; chi cammina resta nella sua area
#define untouched_frames ((uint8*)(g_ram+0x1F560))
enum { kFreeMoverFrames = 16 };

void HdWorld_SpriteFrame(int k) {
  if (untouched_frames[k] < 255)
    untouched_frames[k]++;
}

void HdWorld_SpriteTouchedTiles(int k) {
  untouched_frames[k] = 0;
}

static bool FreeMover(int k) {
  return untouched_frames[k] >= kFreeMoverFrames;
}

// La schermata di ogni nemico. Sullo SNES un nemico viveva solo nella
// schermata di Link; nel mondo unico ognuno "vede" solo attorno a se':
// - la sua "schermata" e' quella del gioco originale, dove un nemico restava
//   attivo fino a 64 pixel oltre lo schermo (256x224) con Link al centro:
//   128 + 64 = 192 pixel in orizzontale, 112 + 64 = 176 in verticale;
// - se Link e' fuori dalla sua schermata non lo vede (durante il suo passo
//   crede che Link sia lontanissimo) e non si allontana oltre la stessa
//   distanza dal suo punto di partenza (fa da muro, o torna dov'era);
// - come con le tessere del gioco originale vede Link solo se Link e' nella
//   sua area. Eccezioni: chi lo ha visto e lo sta seguendo (Chasing, lo segue
//   anche fuori dalla sua area) e, con Link nell'area accanto, chi e' a meno di
//   kBorderSight pixel da lui (3 volte Link: un nemico vicino al confine);
// - chi ha perso Link (lo vedeva e ora non piu') non lo insegue piu': per
//   lui Link e' lontanissimo in basso a destra, e prima ci scivolava contro.
//   I soldati tornano a guardarsi intorno e a camminare come prima di
//   vederlo; gli altri restano dove sono (finche' Link non torna o non e' a
//   3 schermate, quando tornano a casa);
// - chi non vede Link non va verso il Link finto: un passo in basso a destra
//   si annulla (il soldato rosso 0x45 corre sempre verso Link: prima
//   scendeva da solo dal castello verso la casa di Link e, avvicinandosi,
//   contava come inseguitore);
// - chi non vede Link non sente neanche i rumori (sprite_alert_flag, per
//   esempio una freccia contro un muro), che nell'originale arrivavano solo
//   ai nemici sullo schermo;
// - un nemico disturbato (ha visto Link o ha perso vita) torna al punto di
//   partenza con la vita piena quando Link e' a piu' di 3 schermate: il
//   gioco lo toglie e lo ricarica dalla tabella dell'area. Lo stesso per i
//   nemici sconfitti, con Link a 3 schermate dal loro punto di partenza;
// - chi ha perso Link fuori dalla sua schermata ed e' fuori dalla vista
//   sparisce e ricompare al suo posto, come nel gioco originale (prima
//   restava dove aveva perso Link, per esempio i fantasmi del cimitero).
enum { kSightX = 192, kSightY = 176, kResetDistance = 3 * 256, kBorderSight = 48 };
// bit 0: disturbato; bit 1: ha visto Link (finche' non lo perde)
#define spr_disturbed ((uint8*)(g_ram+0x1F580))

static int g_sight_k = -1;
static bool g_sight_sees, g_sight_lost;
static uint16 g_sight_lx, g_sight_ly;
static int g_sight_px, g_sight_py;
static uint8 g_sight_hp, g_sight_state, g_sight_alert;

static bool IsAreaEnemy(int k);
static int SpriteWorldX(int k);
static int SpriteWorldY(int k);
static int HeadAt(int wx, int wy);
static bool Chasing(int k);

static bool SightOn(int k) {
  return FeatureOn() && BYTE(overworld_screen_index) < 0x80 && !player_is_indoors &&
         main_module_index == 9 && IsAreaEnemy(k);
}

// Punto di partenza (centro) del nemico, dalla tabella dell'area caricata
static void HomeOf(int k, int *hx, int *hy) {
  uint16 blk = sprite_N_word[k];
  *hx = (sprcoll_x_base & 0xff00) + ((blk >> 8 & 3) << 8) + (blk << 4 & 0xf0) + 8;
  *hy = (sprcoll_y_base & 0xff00) + ((blk >> 10) << 8) + (blk & 0xf0) + 8;
}

// Distanza "a rettangolo": vale kSightLimit sul bordo della schermata
enum { kSightLimit = kSightX * kSightY };
static int Dist2(int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0) * kSightY, dy = abs(y1 - y0) * kSightX;
  return dx > dy ? dx : dy;
}

void HdWorld_SpriteSpawned(int k) {
  spr_disturbed[k] = 0;
}

// Nemici tolti da ResetEnemy: tornano a casa solo quando la telecamera non
// inquadra la casa (prima ricomparivano subito, anche sotto gli occhi di
// Link: il corvo che scappa tornava al suo posto e attaccava di nuovo)
static uint8 g_hold[128][0x200];

// Toglie il nemico: Sprite_OverworldLoadAllEnemies lo ricarica al suo posto
// con la vita piena
static void ResetEnemy(int k) {
  uint16 blk = sprite_N_word[k];
  uint8 bit = 0x80 >> (blk & 7);
  sprite_state[k] = 0;
  spr_disturbed[k] = 0;
  overworld_sprite_was_loaded[blk >> 3] &= ~bit;
  g_hold[BYTE(overworld_screen_index) & 0x7f][blk >> 3] |= bit;
}

// Sprite_OverworldLoadAllEnemies: vero se il nemico tolto deve ancora
// aspettare (la sua casa e' nella vista, con 32 pixel di margine)
bool HdWorld_HoldReload(uint16 blk) {
  uint8 *hold = &g_hold[BYTE(overworld_screen_index) & 0x7f][blk >> 3], bit = 0x80 >> (blk & 7);
  if (!(*hold & bit))
    return false;
  int hx = (sprcoll_x_base & 0xff00) + ((blk >> 8 & 3) << 8) + (blk << 4 & 0xf0);
  int hy = (sprcoll_y_base & 0xff00) + ((blk >> 10) << 8) + (blk & 0xf0);
  int ext = g_zenv.ppu->extraLeftRight;
  if (hx + 16 + 32 > g_view_wx - ext && hx - 32 < g_view_wx + 256 + ext &&
      hy + 16 + 32 > g_view_wy && hy - 32 < g_view_wy + 240)
    return true;
  *hold &= ~bit;
  return false;
}

// Corvo che scappa e avvoltoio sveglio: come nell'originale volano finche'
// escono dalla vista, poi spariscono (l'avvoltoio sveglio lontano da Link
// sparisce subito e torna a dormire a casa)
static bool FliesUntilOffscreen(int k) {
  return (sprite_type[k] == 0x00 && sprite_ai_state[k] == 3) ||
         (sprite_type[k] == 0x01 && sprite_ai_state[k] == 1);
}

void HdWorld_SpriteBegin(int k) {
  g_sight_k = -1;
  g_sight_lost = false;
  if (!sprite_state[k] || !SightOn(k))
    return;
  g_sight_k = k;
  g_sight_px = SpriteWorldX(k), g_sight_py = SpriteWorldY(k);
  g_sight_hp = sprite_health[k], g_sight_state = sprite_state[k];
  int lx = link_x_coord + 8, ly = link_y_coord + 12;
  g_sight_sees = Dist2(g_sight_px + 8, g_sight_py + 8, lx, ly) <= kSightLimit;
  if (g_sight_sees && HeadAt(lx, ly) != BYTE(overworld_screen_index) && !((spr_disturbed[k] & 2) && Chasing(k))) {
    int dx = abs(g_sight_px + 8 - lx), dy = abs(g_sight_py + 8 - ly);
    g_sight_sees = (dx > dy ? dx : dy) <= kBorderSight;
  }
  // il corvo che scappa (Sprite_Raven, stato 3) continua a volare via da
  // Link e l'avvoltoio sveglio (Sprite_01_Vulture_bounce, stato 1)
  // continua a girargli attorno, anche fuori dalla loro schermata, finche'
  // escono dalla vista
  if (FliesUntilOffscreen(k))
    g_sight_sees = true;
  if (g_sight_sees) {
    spr_disturbed[k] = 3;
    return;
  }
  if (spr_disturbed[k] & 2) {
    uint8 t = sprite_type[k];
    if (t >= 0x41 && t <= 0x43 && !sprite_C[k]) {
      // soldato (Guard_Main): smette di inseguire e si guarda intorno
      if (sprite_ai_state[k] == 3 || sprite_ai_state[k] == 4) {
        sprite_ai_state[k] = 2;
        sprite_delay_main[k] = 160;
        sprite_x_vel[k] = sprite_y_vel[k] = 0;
      }
      spr_disturbed[k] &= ~2;
    } else {
      g_sight_lost = true;
    }
  }
  g_sight_alert = sprite_alert_flag;
  sprite_alert_flag = 0;
  // Link lontano: per questo nemico sta molto piu' in la' (anche il byte
  // basso della distanza e' grande, per chi guarda solo quello)
  g_sight_lx = link_x_coord, g_sight_ly = link_y_coord;
  link_x_coord = g_sight_px + 0x780;
  link_y_coord = g_sight_py + 0x780;
}

void HdWorld_SpriteEnd(int k, bool offscreen) {
  if (g_sight_k != k)
    return;
  g_sight_k = -1;
  if (!g_sight_sees) {
    link_x_coord = g_sight_lx, link_y_coord = g_sight_ly;
    sprite_alert_flag = g_sight_alert;
  }
  if (!sprite_state[k])
    return;
  // l'avvoltoio si sveglia 160 fotogrammi dopo essere comparso: qui conta
  // solo il tempo con Link nella sua schermata
  if (sprite_type[k] == 0x01 && sprite_ai_state[k] == 0 && !g_sight_sees)
    sprite_subtype2[k] = 0;
  // corvo o avvoltoio fuori dalla vista: sparisce come nell'originale e
  // torna al suo posto quando la telecamera non lo inquadra (HdWorld_HoldReload)
  if (offscreen && FliesUntilOffscreen(k)) {
    ResetEnemy(k);
    return;
  }
  if (g_sight_state == 9 && sprite_state[k] == 9 && sprite_health[k] < g_sight_hp)
    spr_disturbed[k] |= 1;
  // (non il primo passo: SpritePrep sposta molti nemici di 8 pixel in basso a
  // destra, per esempio le statue del Deserto della Pazzia)
  if (!g_sight_sees && (g_sight_lost || (g_sight_state != 8 && SpriteWorldX(k) > g_sight_px && SpriteWorldY(k) > g_sight_py))) {
    // ha perso Link (resta dov'e') o va verso il Link finto
    Sprite_SetX(k, g_sight_px);
    Sprite_SetY(k, g_sight_py);
  }
  if (g_sight_sees || !IsAreaEnemy(k))
    return;
  // oltre il raggio da casa si puo' solo tornare indietro
  int hx, hy;
  HomeOf(k, &hx, &hy);
  int now = Dist2(SpriteWorldX(k) + 8, SpriteWorldY(k) + 8, hx, hy);
  int before = Dist2(g_sight_px + 8, g_sight_py + 8, hx, hy);
  if (now > kSightLimit && now > before) {
    Sprite_SetX(k, g_sight_px);
    Sprite_SetY(k, g_sight_py);
    now = before;
  }
  if (offscreen && now > kSightLimit)
    ResetEnemy(k);
}

// Sprite_CheckTileProperty: il punto (x, y) oltre il raggio da casa, e piu'
// lontano del nemico, fa da muro (cosi' chi cammina si gira come davanti a
// un ostacolo)
bool HdWorld_LeashWall(int k, uint16 x, uint16 y) {
  if (g_sight_k != k || g_sight_sees)
    return false;
  int hx, hy;
  HomeOf(k, &hx, &hy);
  int d = Dist2(x, y, hx, hy);
  return d > kSightLimit &&
         d > Dist2(SpriteWorldX(k) + 8, SpriteWorldY(k) + 8, hx, hy);
}

// Sprite_SpawnProbeAlways: le sonde dei soldati (che "vedono" Link quando lo
// toccano) usano solo i posti 0-9. Col mondo tutto attivo un'area con molti
// nemici li occupa tutti (0x75 nel Mondo delle Tenebre ne ha 14) e i soldati
// non vedevano mai Link: chi lo vede puo' usare anche i posti 10-14 (il 15
// resta ai colpi dei nemici)
bool HdWorld_ProbeMoreSlots(int k) {
  return g_sight_k == k && g_sight_sees;
}

// Nemici sconfitti gia' fatti ricomparire mentre Link era lontano: alcuni
// (legati agli eventi) il gioco li toglie appena caricati, e senza questo
// ricomparirebbero a ogni frame. Si libera quando Link torna vicino.
static uint8 g_revived[128][0x200];

// Nemici che hanno perso Link fuori dalla loro area e aspettano di tornare a
// casa (HdWorld_CarriedLost): restano sconfitti finche' Link e' nella loro area
enum { kMaxHoming = 32 };
static struct { uint8 area; uint16 blk; } g_homing[kMaxHoming];
static int g_n_homing;
static void ReturnHome(int a, uint16 blk);
static void ReturnHomeWhenAway(void);

static bool IsHoming(int a, uint16 blk) {
  for (int i = 0; i < g_n_homing; i++)
    if (g_homing[i].area == a && g_homing[i].blk == blk)
      return true;
  return false;
}

static bool FarFromLink(int x, int y, int lx, int ly) {
  int dx = abs(x - (lx + 8)), dy = abs(y - (ly + 12));
  return (dx > dy ? dx : dy) > kResetDistance;
}

// Vero se il nemico (area, posto nella tabella) segue Link nel gioco vero:
// la sua area lo conta sconfitto, ma non deve ricomparire (main_ram = RAM
// del gioco vero, anche durante il passo di una copia)
static bool CarriedFrom(const uint8 *main_ram, int area, uint16 blk) {
  for (int k = 0; k < 16; k++)
    if (main_ram[0x1F520 + k] == (0x80 | area) &&
        (main_ram[0x1F540 + k * 2] | main_ram[0x1F541 + k * 2] << 8) == blk)
      return true;
  return false;
}

// Con Link a piu' di 3 schermate i nemici disturbati tornano come all'inizio
// e quelli sconfitti ricompaiono (Sprite_OverworldLoadAllEnemies li ricarica
// dalla tabella dell'area)
static void ResetFarEnemies(int lx, int ly, const uint8 *main_ram) {
  for (int k = 0; k < 16; k++)
    if (spr_disturbed[k] && IsAreaEnemy(k) && FarFromLink(SpriteWorldX(k) + 8, SpriteWorldY(k) + 8, lx, ly))
      ResetEnemy(k);
  int area = BYTE(overworld_screen_index) & 0x7f;
  int bx = sprcoll_x_base & 0xff00, by = sprcoll_y_base & 0xff00;
  uint8 *revived = g_revived[area];
  for (int blk = 0; blk < 0x1000; blk++) {
    uint8 t = sprite_where_in_overworld[blk], bit = 0x80 >> (blk & 7);
    if (!t || t >= 0xf4)
      continue;
    int hx = bx + ((blk >> 8 & 3) << 8) + (blk << 4 & 0xf0) + 8;
    int hy = by + ((blk >> 10) << 8) + (blk & 0xf0) + 8;
    if (!FarFromLink(hx, hy, lx, ly)) {
      revived[blk >> 3] &= ~bit;
      continue;
    }
    if (!(overworld_sprite_was_loaded[blk >> 3] & bit) || (revived[blk >> 3] & bit))
      continue;
    bool active = false;
    for (int k = 0; k < 16 && !active; k++)
      active = sprite_state[k] && sprite_N_word[k] == blk;
    if (!active && !CarriedFrom(main_ram, area, blk) && !IsHoming(area, blk)) {
      overworld_sprite_was_loaded[blk >> 3] &= ~bit;
      revived[blk >> 3] |= bit;
    }
  }
}

// Le tabelle del gioco hanno 16 posti e i nemici dell'area ne usano al
// massimo 14 (0-13); alcune aree, soprattutto nel Mondo delle Tenebre, ne
// hanno di piu' (0x75 ne ha 15) e con tutti i nemici caricati qualcuno
// restava fuori. Quando non c'e' posto, il nemico non caricato con la casa
// piu' vicina a Link, dentro la schermata di Link, prende il posto del nemico
// tranquillo (non disturbato) piu' lontano, oltre una volta e mezza la
// schermata: quello tolto torna a casa quando c'e' di nuovo posto. Uno
// scambio per frame, dopo Sprite_OverworldLoadAllEnemies.
static void MakeRoomNearLink(void) {
  int lx = link_x_coord + 8, ly = link_y_coord + 12;
  int bx = sprcoll_x_base & 0xff00, by = sprcoll_y_base & 0xff00;
  int best = -1, best_d = kSightLimit + 1;
  for (int blk = 0; blk < 0x1000; blk++) {
    uint8 t = sprite_where_in_overworld[blk];
    if (!t || t >= 0xf4 || (overworld_sprite_was_loaded[blk >> 3] & (0x80 >> (blk & 7))))
      continue;
    int hx = bx + ((blk >> 8 & 3) << 8) + (blk << 4 & 0xf0) + 8;
    int hy = by + ((blk >> 10) << 8) + (blk & 0xf0) + 8;
    int d = Dist2(hx, hy, lx, ly);
    if (d < best_d)
      best = blk, best_d = d;
  }
  if (best < 0)
    return;
  // stessi posti di Overworld_AllocSprite
  uint8 type = sprite_where_in_overworld[best] - 1;
  int last = type == 0x58 ? 4 : type == 0xd0 ? 5 : (type == 0xeb || type == 0x53 || type == 0xf3) ? 14 : 13;
  int far = -1, far_d = kSightLimit * 3 / 2;
  for (int k = 0; k <= last; k++) {
    if (sprite_state[k] != 9 || spr_disturbed[k] || !IsAreaEnemy(k))
      continue;
    int d = Dist2(SpriteWorldX(k) + 8, SpriteWorldY(k) + 8, lx, ly);
    if (d > far_d)
      far = k, far_d = d;
  }
  if (far < 0)
    return;
  ResetEnemy(far);
  Overworld_LoadProximaSpriteIfAlive(best);
}

// Con tutti i nemici dell'area caricati i posti 0-13 possono essere tutti
// occupati, e i colpi che nascono solo li' non partivano mai (la palla di
// fuoco degli Zora, Sprite_SpawnFireball). Sprite_SpawnDynamicallyEx senza
// posto libero: si toglie il nemico tranquillo piu' lontano da Link, oltre
// una volta e mezza la schermata, fra i posti 0-last (torna a casa quando
// c'e' di nuovo posto). -1 se non ce n'e'.
int HdWorld_FreeSlot(int k, int last) {
  if (!FeatureOn() || BYTE(overworld_screen_index) >= 0x80 || player_is_indoors || main_module_index != 9)
    return -1;
  int lx = link_x_coord + 8, ly = link_y_coord + 12;
  int far = -1, far_d = kSightLimit * 3 / 2;
  for (int j = 0; j <= last; j++) {
    if (j == k || sprite_state[j] != 9 || spr_disturbed[j] || !IsAreaEnemy(j))
      continue;
    int d = Dist2(SpriteWorldX(j) + 8, SpriteWorldY(j) + 8, lx, ly);
    if (d > far_d)
      far = j, far_d = d;
  }
  if (far >= 0)
    ResetEnemy(far);
  return far;
}

static void MarkCarried(int k, int area, uint16 blk) {
  carried_area[k] = 0x80 | (area & 0x7f);
  carried_type[k] = sprite_type[k];
  carried_blk[k] = blk;
}

static bool IsMarkedCarried(int k) {
  return (carried_area[k] & 0x80) && sprite_state[k] && sprite_type[k] == carried_type[k] &&
         sprite_N_word[k] == 0xffff;
}

// L'area a conta come sconfitti i suoi nemici che ora seguono Link
static void KeepCarriedKilled(int a) {
  AreaState *st = &g_states[a];
  for (int k = 0; k < 16; k++) {
    if (!IsMarkedCarried(k) || (carried_area[k] & 0x7f) != a)
      continue;
    int i = 0;
    while (i < st->n_killed && st->killed[i] != carried_blk[k])
      i++;
    if (i == st->n_killed && st->n_killed < kMaxKilled)
      st->killed[st->n_killed++] = carried_blk[k];
    st->used = true;
    g_any_state = true;
  }
}

// Zora sott'acqua: invisibile, e intanto prova punti a caso (anche a terra)
static bool IsSubmerged(int k) {
  return sprite_type[k] == 0x55 && !sprite_E[k] && sprite_ai_state[k] == 0;
}

static bool PersistOn(void) {
  return FeatureOn() && BYTE(overworld_screen_index) < 0x80;
}

// Gioco normale (senza il mondo continuo), all'aperto: frecce piantate e
// oggetti caduti restano anche qui, fino al cambio d'area (NormalGameArrows)
static bool KeepInNormalGame(void) {
  return !FeatureOn() && BYTE(overworld_screen_index) < 0x80 && !player_is_indoors;
}

// Oggetto caduto che sparisce da solo (cuori, rupie, bombe, frecce, magia,
// fata: sprite_stunned e' il suo tempo), non messo dalla tabella dell'area
static bool IsTimedDrop(int k) {
  return sprite_type[k] >= 0xd8 && sprite_type[k] <= 0xe6 && sprite_stunned[k] && sprite_N_word[k] >= 0x1000;
}

// Al chiuso (dungeon e case) le frecce piantate restano nella stanza
// corrente e si dimenticano cambiando stanza (HdWorld_Composite): sono solo
// un disegno, senza effetti sul gioco. Non nelle stanze buie, dove
// resterebbero illuminate
static ArrowRec g_room_arrows[kMaxArrows];
static int g_room_n_arrows, g_room_arrows_room = -1;

static void ForgetEnemies(int i) {
  g_states[i].n_killed = g_states[i].n_alive = 0;
  for (int j = 0; j < kMaxAlive; j++)
    g_patches[i][j].valid = false;
}

static void LiveDropAll(void);
static void LiveDropArea(int a);

void HdWorld_ResetStates(void) {
  // anche i cespugli bruciati ricrescono: niente macchie all'uscita
  HdBurn_Reset();
  LiveDropAll();
  g_n_homing = 0;  // il mondo riparte: tutti i nemici tornano a casa
  if (!g_any_state)
    return;
  memset(g_states, 0, sizeof(g_states));
  memset(g_hold, 0, sizeof(g_hold));
  for (int i = 0; i < 128; i++)
    ForgetEnemies(i);
  g_state_epoch++;
  g_any_state = false;
}

static uint32 StateVersion(int head) {
  return g_state_epoch * 65536 + g_states[head & 0x7f].version;
}

// Distanza fra due aree in quadrati di mappa (0 = si toccano); altro mondo = lontanissimo
static int AreaGap(int a, int b) {
  if ((a ^ b) & 0x40)
    return 99;
  int ax, ay, as, bx, by, bs;
  AreaRect(a, &ax, &ay, &as);
  AreaRect(b, &bx, &by, &bs);
  int gx = bx >= ax + as ? bx - (ax + as) : ax >= bx + bs ? ax - (bx + bs) : 0;
  int gy = by >= ay + as ? by - (ay + as) : ay >= by + bs ? ay - (by + bs) : 0;
  return (gx > gy ? gx : gy) / 512;
}

static int SpriteWorldX(int k) { return sprite_x_hi[k] << 8 | sprite_x_lo[k]; }
static int SpriteWorldY(int k) { return sprite_y_hi[k] << 8 | sprite_y_lo[k]; }

// Nemico messo dal gioco leggendo la tabella dell'area (non un proiettile o
// un oggetto comparso durante il gioco)
static bool IsAreaEnemy(int k) {
  uint16 blk = sprite_N_word[k];
  if (!sprite_state[k] || blk >= 0x1000)
    return false;
  if (!(overworld_sprite_was_loaded[blk >> 3] & (0x80 >> (blk & 7))))
    return false;
  uint8 t = sprite_where_in_overworld[blk];
  return t && t < 0xf4 && (uint8)(t - 1) == sprite_type[k];
}

// Mondo vivo: un nemico dell'area che esce dalla vista resta fermo dov'e'
// (sprite_pause) invece di sparire e ricomparire al punto di partenza.
// Anche gli oggetti caduti che spariscono da soli (IsTimedDrop) restano
// finche' il tempo non scade, invece di sparire appena escono dalla vista
bool HdWorld_KeepOffscreen(int k) {
  if (!PersistOn() || player_is_indoors)
    return false;
  return IsTimedDrop(k) || IsAreaEnemy(k);
}

// Lo stesso per gli oggetti caduti nel gioco normale (fuori dalla telecamera
// del gioco)
bool HdWorld_KeepDropNormal(int k) {
  return KeepInNormalGame() && IsTimedDrop(k);
}

// I nemici dell'area di Link restano nella loro area: il bordo fa da muro
// come nel gioco originale (chi segue Link ha sprite_N_word 0xffff). Nelle
// copie delle aree vicine ci pensa LiveTick
bool HdWorld_ConfinedToArea(int k) {
  return PersistOn() && !player_is_indoors && !g_hd_neighbor_tick && IsAreaEnemy(k) && !FreeMover(k);
}

// Riporta dentro la sua area un nemico che ne e' uscito: dov'era prima, o
// (se anche li' era fuori, per esempio in un salvataggio) appena dentro il bordo
static void KeepInArea(int k, int ox, int oy, bool old_valid, int ax, int ay, int as) {
  int x = SpriteWorldX(k), y = SpriteWorldY(k);
  if (x + 8 >= ax && x + 8 < ax + as && y + 8 >= ay && y + 8 < ay + as)
    return;
  if (old_valid && ox + 8 >= ax && ox + 8 < ax + as && oy + 8 >= ay && oy + 8 < ay + as) {
    x = ox, y = oy;
  } else {
    x = x < ax ? ax : x > ax + as - 16 ? ax + as - 16 : x;
    y = y < ay ? ay : y > ay + as - 16 ? ay + as - 16 : y;
  }
  Sprite_SetX(k, x);
  Sprite_SetY(k, y);
}

// Tutti i dati di un nemico (le tabelle di kSpriteFields), da e verso un record
static void SaveFields(int k, uint8 *d) {
  for (int f = 0; f < countof(kSpriteFields); f++) {
    const SpriteField *sf = &kSpriteFields[f];
    memcpy(d, g_ram + sf->addr + k * sf->size, sf->size);
    d += sf->size;
  }
}

static void LoadFields(int k, const uint8 *d) {
  for (int f = 0; f < countof(kSpriteFields); f++) {
    const SpriteField *sf = &kSpriteFields[f];
    memcpy(g_ram + sf->addr + k * sf->size, d, sf->size);
    d += sf->size;
  }
}

// Copia i nemici dell'area caricata in questo momento (quella corrente, o
// un'area vicina nella sua copia dello stato del gioco)
static void SnapshotEnemies(void) {
  int a = BYTE(overworld_screen_index) & 0x7f;
  AreaState *st = &g_states[a];
  // nemici vivi
  st->n_alive = 0;
  for (int k = 0; k < 16 && st->n_alive < kMaxAlive; k++) {
    if (!IsAreaEnemy(k))
      continue;
    EnemyRec *r = &st->alive[st->n_alive++];
    r->blk = sprite_N_word[k];
    r->x = SpriteWorldX(k), r->y = SpriteWorldY(k), r->z = sprite_z[k];
    r->hidden = IsSubmerged(k);
    SaveFields(k, r->data);
  }
  // nemici sconfitti: gia' comparsi (il gioco lo segna) e non piu' attivi.
  // Quelli usciti dallo schermo il gioco li segna di nuovo come da caricare.
  st->n_killed = 0;
  for (int blk = 0; blk < 0x1000 && st->n_killed < kMaxKilled; blk++) {
    if (!(overworld_sprite_was_loaded[blk >> 3] & (0x80 >> (blk & 7))))
      continue;
    uint8 t = sprite_where_in_overworld[blk];
    if (!t || t >= 0xf4)
      continue;  // niente da caricare, o un generatore di nemici
    bool alive = false;
    for (int i = 0; i < st->n_alive && !alive; i++)
      alive = st->alive[i].blk == blk;
    // l'oggetto lasciato dal nemico sconfitto (cuore, rupia...) prende il suo
    // posto e il suo numero: conta solo uno sprite del tipo del nemico,
    // altrimenti uscendo dall'area il nemico tornava subito
    bool active = alive;
    for (int k = 0; k < 16 && !active; k++)
      active = sprite_state[k] && sprite_N_word[k] == blk && sprite_type[k] == (uint8)(t - 1);
    if (!active)
      st->killed[st->n_killed++] = blk;
  }
  st->used = true;
  g_any_state = true;
}

// Copia lo stato dell'area corrente (una volta per frame di gioco)
static void SnapshotArea(void) {
  int a = BYTE(overworld_screen_index) & 0x7f;
  AreaState *st = &g_states[a];
  bool changed = !st->used;
  int n = num_memorized_tiles >> 1;
  if (n > kMaxTiles) n = kMaxTiles;
  if (n != st->n_tiles ||
      memcmp(st->tile_pos, memorized_tile_addr, n * 2) || memcmp(st->tile_val, memorized_tile_value, n * 2)) {
    st->n_tiles = n;
    memcpy(st->tile_pos, memorized_tile_addr, n * 2);
    memcpy(st->tile_val, memorized_tile_value, n * 2);
    changed = true;
  }
  SnapshotEnemies();
  if (changed)
    st->version++;
}

static bool RememberExtraIn(AreaState *st, uint16 pos, uint16 value);

void HdWorld_RememberExtraTile(uint16 pos, uint16 value) {
  if (!PersistOn() || player_is_indoors)
    return;
  RememberExtraIn(&g_states[BYTE(overworld_screen_index) & 0x7f], pos, value);
}

// Vero se la casella e' cambiata
static bool RememberExtraIn(AreaState *st, uint16 pos, uint16 value) {
  int i = 0;
  while (i < st->n_extra && st->extra_pos[i] != pos)
    i++;
  if (i == kMaxExtra || (i < st->n_extra && st->extra_val[i] == value))
    return false;  // lista piena, o casella gia' ricordata cosi'
  if (i == st->n_extra)
    st->n_extra++;
  st->extra_pos[i] = pos, st->extra_val[i] = value;
  st->used = true;
  st->version++;
  g_any_state = true;
  return true;
}

// Il gioco mostra la stanza: dungeon, o dialoghi e menu aperti sopra di essa
static bool ShowsRoom(void) {
  int sub = submodule_index;
  return main_module_index == 7 ||
         (main_module_index == 14 && saved_module_for_menu == 7 &&
          (sub == 1 || sub == 2 || sub == 4 || sub == 8 || sub == 9 || sub == 11));
}

// Le frecce piantate restano solo con "Frecce piantate nei muri" (StuckArrows,
// prima dentro BurnAndFreezeInteraction); senza, spariscono come nel gioco originale
static bool ArrowsKept(void) {
  if (!g_config.stuck_arrows)
    return false;
  if (player_is_indoors)
    return main_module_index == 7 && !dung_want_lights_out;
  return PersistOn() || KeepInNormalGame();
}

// Lista delle frecce (di Link e dei nemici insieme) della stanza o dell'area
// corrente
static ArrowRec *ArrowList(int **n) {
  if (player_is_indoors) {
    if (g_room_arrows_room != dungeon_room_index)
      g_room_n_arrows = 0, g_room_arrows_room = dungeon_room_index;
    *n = &g_room_n_arrows;
    return g_room_arrows;
  }
  AreaState *st = &g_states[BYTE(overworld_screen_index) & 0x7f];
  *n = &st->n_arrows;
  return st->arrows;
}

// Posto per una freccia nella lista: la piu' vecchia lascia il posto
static ArrowRec *NewArrow(void) {
  int *n;
  ArrowRec *list = ArrowList(&n);
  if (*n == kMaxArrows) {
    memmove(list, list + 1, (kMaxArrows - 1) * sizeof(ArrowRec));
    (*n)--;
  }
  return &list[*n];
}

static void AddArrow(void) {
  int *n;
  ArrowList(&n);
  (*n)++;
  if (!player_is_indoors) {
    g_states[BYTE(overworld_screen_index) & 0x7f].used = true;
    g_any_state = true;
  }
}

bool HdWorld_KeepArrow(int k) {
  if (!ArrowsKept() || g_hd_neighbor_tick)
    return false;
  ArrowRec *r = NewArrow();
  r->n = Arrow_StuckPieces(k, r->chr, r->flags, r->dx, r->dy);
  if (!r->n)
    return false;
  r->x = Ancilla_GetX(k), r->y = Ancilla_GetY(k);
  AddArrow();
  return true;
}

// Freccia di un nemico (Sprite_1B_Arrow) piantata in un muro: invece di
// sparire resta come quelle di Link, nella stessa lista. Anche nelle copie
// dal vivo delle aree vicine (g_states non e' nella RAM del gioco)
bool HdWorld_KeepEnemyArrow(int k, const uint8 *chr, const uint8 *flags, const int8 *dx, const int8 *dy) {
  if (!ArrowsKept())
    return false;
  ArrowRec *r = NewArrow();
  r->n = 2;
  for (int p = 0; p < 2; p++)
    r->chr[p] = chr[p], r->flags[p] = flags[p], r->dx[p] = dx[p], r->dy[p] = dy[p];
  r->x = Sprite_GetX(k), r->y = Sprite_GetY(k) - sprite_z[k];
  AddArrow();
  return true;
}

static void ApplyTiles(int head, bool memorize) {
  AreaState *st = &g_states[head & 0x7f];
  if (!st->used)
    return;
  for (int i = 0; i < st->n_extra; i++)
    dung_bg2[st->extra_pos[i] >> 1] = st->extra_val[i];
  for (int i = 0; i < st->n_tiles; i++) {
    dung_bg2[st->tile_pos[i] >> 1] = st->tile_val[i];
    if (memorize)
      Overworld_Memorize_Map16_Change(st->tile_pos[i], st->tile_val[i]);
  }
}

void HdWorld_EnterAreaTiles(bool memorize) {
  if (!PersistOn())
    return;
  int a = BYTE(overworld_screen_index);
  // i nemici delle aree lontane tornano quelli di partenza
  for (int i = 0; i < 128; i++)
    if (g_states[i].used && i != (a & 0x7f) && AreaGap(i, a) >= kEnemyResetGap) {
      ForgetEnemies(i);
      KeepCarriedKilled(i);  // chi segue Link non torna a casa
      LiveDropArea(i);  // la sua copia dal vivo riparte dai nemici di partenza
    }
  ApplyTiles(a, memorize);
}

void HdWorld_RememberTiles(void) {
  if (!PersistOn())
    return;
  AreaState *st = &g_states[BYTE(overworld_screen_index) & 0x7f];
  if (!st->used)
    return;
  for (int i = 0; i < st->n_tiles; i++)
    Overworld_Memorize_Map16_Change(st->tile_pos[i], st->tile_val[i]);
}

// Dopo che il gioco ha letto i nemici dell'area appena raggiunta
void HdWorld_RestoreKilled(void) {
  if (!PersistOn())
    return;
  AreaState *st = &g_states[BYTE(overworld_screen_index) & 0x7f];
  if (!st->used)
    return;
  for (int i = 0; i < st->n_killed; i++)
    overworld_sprite_was_loaded[st->killed[i] >> 3] |= 0x80 >> (st->killed[i] & 7);
  // i nemici vivi tornano com'erano, ognuno in un posto libero
  for (int i = 0; i < st->n_alive; i++) {
    EnemyRec *r = &st->alive[i];
    int k = 15;
    while (k >= 0 && sprite_state[k])
      k--;
    if (k < 0)
      break;  // niente posto: ricomparira' come nuovo
    LoadFields(k, r->data);
    overworld_sprite_was_loaded[r->blk >> 3] |= 0x80 >> (r->blk & 7);
  }
}

// Aspetto dei nemici sullo schermo (dal frame appena disegnato dalla PPU)
static void CaptureEnemies(uint8 *pixels, int pitch, int width, int height) {
  Ppu *ppu = g_zenv.ppu;
  int a = BYTE(overworld_screen_index) & 0x7f;
  int ext = (width - 256) / 2;
  // +1 come per il fondale: la riga 0 dello schermo e' la riga 1 della PPU
  int cam_x = BG2HOFS_copy2 + ppu->objBiasX - ext, cam_y = BG2VOFS_copy2 + ppu->objBiasY + 1;
  // Link colpito salta: il suo disegno sale di link_z_coord
  int lx = link_x_coord, ly = link_y_coord, lz = (int16)link_z_coord > 0 ? (int16)link_z_coord : 0;
  for (int k = 0; k < 16; k++) {
    if (!IsAreaEnemy(k) || IsSubmerged(k))
      continue;
    int wx = SpriteWorldX(k), wy = SpriteWorldY(k), z = sprite_z[k];
    // riquadro largo: lance, elmi, scudi e la spada abbassata dei soldati
    // (fino a 28 pixel sotto) non vanno tagliati
    int ox = -24, oy = -36;
    int sx = wx + ox - cam_x, sy = wy - z + oy - cam_y;
    if (sx < 0 || sy < 0 || sx + kPatchW > width || sy + kPatchH > height)
      continue;  // non tutto sullo schermo: si tiene l'aspetto precedente
    uint16 blk = sprite_N_word[k];
    EnemyPatch *pt = NULL;
    for (int j = 0; j < kMaxAlive && !pt; j++)
      if (g_patches[a][j].valid && g_patches[a][j].blk == blk)
        pt = &g_patches[a][j];
    for (int j = 0; j < kMaxAlive && !pt; j++)
      if (!g_patches[a][j].valid)
        pt = &g_patches[a][j];
    if (!pt)
      continue;
    pt->valid = true, pt->blk = blk, pt->ox = ox, pt->oy = oy;
    for (int y = 0; y < kPatchH; y++) {
      const uint32 *row = (const uint32 *)(pixels + (sy + y) * pitch) + sx;
      for (int x = 0; x < kPatchW; x++) {
        uint32 c = row[x], l = c >> 24;
        int px = wx + ox + x, py = wy - z + oy + y;
        bool mine = (l == 4 || l == 6);
        // niente Link (con spada e scudo) ne' altri nemici che passano davanti
        if (mine && px >= lx - 12 && px < lx + 28 && py >= ly - lz - 16 && py < ly + 24)
          mine = false;
        for (int j = 0; j < 16 && mine; j++)
          if (j != k && sprite_state[j]) {
            int jx = SpriteWorldX(j), jy = SpriteWorldY(j) - sprite_z[j];
            if (px >= jx && px < jx + 16 && py >= jy && py < jy + 16 &&
                !(px >= wx && px < wx + 16 && py >= wy - z && py < wy - z + 16))
              mine = false;
          }
        pt->pix[y * kPatchW + x] = mine ? c : 0;
      }
    }
  }
}

static bool LiveShown(int a);

// Nemici delle aree vicine, fermi come l'ultima volta che si sono visti
// (quelle che non si muovono dal vivo)
static void DrawFrozenEnemies(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y) {
  int cur = BYTE(overworld_screen_index) & 0x7f;
  for (int a = 0; a < 128; a++) {
    AreaState *st = &g_states[a];
    // l'area appena raggiunta si salta, tranne nel primo frame del passaggio,
    // quando il gioco non ha ancora caricato i suoi nemici (sparivano per un
    // frame)
    if (!st->used || (a == cur && !InTransition()) || ((a ^ cur) & 0x40) || !st->n_alive || LiveShown(a))
      continue;
    for (int i = 0; i < st->n_alive; i++) {
      EnemyRec *r = &st->alive[i];
      if (r->hidden)
        continue;
      if (a == cur) {
        bool loaded = false;
        for (int k = 0; k < 16 && !loaded; k++)
          loaded = sprite_state[k] && sprite_N_word[k] == r->blk;
        if (loaded)
          continue;
      }
      EnemyPatch *pt = NULL;
      for (int j = 0; j < kMaxAlive && !pt; j++)
        if (g_patches[a][j].valid && g_patches[a][j].blk == r->blk)
          pt = &g_patches[a][j];
      if (!pt)
        continue;
      int x0 = r->x + pt->ox - cam_x, y0 = r->y - r->z + pt->oy - cam_y;
      if (x0 + kPatchW <= 0 || y0 + kPatchH <= 0 || x0 >= width || y0 >= height)
        continue;
      for (int y = 0; y < kPatchH; y++) {
        int sy = y0 + y;
        if ((unsigned)sy >= (unsigned)height)
          continue;
        uint32 *row = (uint32 *)(pixels + sy * pitch);
        for (int x = 0; x < kPatchW; x++) {
          int sx = x0 + x;
          uint32 c = pt->pix[y * kPatchW + x];
          if (!c || (unsigned)sx >= (unsigned)width)
            continue;
          uint32 l = row[sx] >> 24;
          if (l == 2 || l == 4 || l == 6)
            continue;  // HUD e sprite veri restano davanti
          if (HdWorld_InsideCurrentArea(cam_x + sx, cam_y + sy) && !InTransition())
            continue;
          row[sx] = Dim(c);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Telecamera libera

static int g_world_dx, g_world_dy;
static float g_vis_x, g_vis_y;     // posizione della vista libera
static int g_last_lx, g_last_ly;   // Link nel frame precedente
static bool g_vis_valid;
static int g_prev_mode;
static int g_prev_module;
static int g_prev_area = -1;      // area esterna nel frame precedente
static bool g_vis_glide;          // uscita da una casa: la vista scivola al suo posto

// Avvicina v a t, muovendosi al massimo quanto Link (m) piu' un quarto di
// pixel; mentre Link cammina la vista non va mai nel verso opposto al suo,
// altrimenti invertendo la direzione la vista "rimbalzerebbe" prima di
// seguirlo
static float Approach(float v, float t, int m) {
  float step = abs(m) + 0.25f;
  float d = t > v + step ? step : t < v - step ? -step : t - v;
  if ((m > 0 && d < 0) || (m < 0 && d > 0))
    d = 0;
  return v + d;
}

static void ClampViewArea(int *cam_x, int *cam_y);

void HdWorld_UpdateCamera(int *cam_x, int *cam_y) {
  int gx = BG2HOFS_copy2, gy = BG2VOFS_copy2;
  // titolo, selezione file, caricamento, e dentro case e labirinti (6, 7):
  // il mondo esterno torna com'era
  if (main_module_index <= 7) {
    HdWorld_ResetStates();
    memset(carried_area, 0, 16);  // nessuno segue piu' Link
  } else if (main_module_index == 9 && submodule_index == 0 && PersistOn())
    SnapshotArea();
  // Mappa del mondo aperta: la vista si conserva per il ritorno, cosi' alla
  // chiusura l'inquadratura e' la stessa di prima
  if (main_module_index == 14 && submodule_index == 7 && saved_module_for_menu == 9 && g_vis_valid) {
    *cam_x = gx + g_world_dx;
    *cam_y = gy + g_world_dy;
    return;
  }
  if (HdWorld_Active() && InMenuOverWorld() && g_vis_valid) {
    // menu o dialogo aperto: la vista non si muove (e alla chiusura riprende
    // a seguire Link da dove era)
    *cam_x = gx + g_world_dx;
    *cam_y = gy + g_world_dy;
    return;
  }
  int prev_module = g_prev_module;
  g_prev_module = main_module_index;
  // fase 10: Link esce dalla porta dopo il cerchio che si apre; fase 9: prima
  // si apre il portone (chiesa, castello): la vista resta su Link
  int mode = !HdWorld_Active() ? 0 : InFluteArrival() || InSoftIris() ? 1 : main_module_index != 9 ? 0 :
             submodule_index == 0 || submodule_index == 9 || submodule_index == 10 || InMapReturn() ? 1 : InTransition() ? 2 :
             InMosaicTransition() ? 3 : 0;
  int prev_mode = g_prev_mode;
  g_prev_mode = mode;
  // Col mondo continuo il passaggio fra aree si fa tutto in un frame di gioco
  // (Overworld_RunScrollTransition): la vista non vede mai la modalita' 2,
  // il cambio d'area si riconosce dal numero dell'area
  int area = main_module_index == 9 ? BYTE(overworld_screen_index) : -1;
  bool area_changed = area >= 0 && g_prev_area >= 0 && area != g_prev_area;
  g_prev_area = area;
  if (mode == 0) {
    g_vis_valid = false;
    g_world_dx = g_world_dy = 0;
    if (InSpecialExit()) {
      int cx = gx, cy = gy;
      ClampViewArea(&cx, &cy);
      g_world_dx = cx - gx, g_world_dy = cy - gy;
      *cam_x = cx, *cam_y = cy;
    }
    return;
  }
  int z = (allow_scroll_z && link_z_coord != 0xffff) ? link_z_coord : 0;
  int lx = link_x_coord + 8, ly = (uint16)(link_y_coord - z + 12);
  int mx = g_vis_valid ? lx - g_last_lx : 0, my = g_vis_valid ? ly - g_last_ly : 0;
  g_last_lx = lx, g_last_ly = ly;
  if (mode == 3) {
    // Mosaico e dissolvenza: la vista resta ferma nel mondo mentre il gioco
    // sposta la sua telecamera nell'area nuova
    if (!g_vis_valid)
      g_vis_x = (float)gx, g_vis_y = (float)gy;
    // gia' entro i limiti della vista (bordo del mondo, montagna): uscendo
    // dal Dominio di Zora la telecamera del gioco e' oltre e, finita la
    // dissolvenza, la vista saltava di 37 pixel
    if (BYTE(overworld_screen_index) < 0x80) {
      int cx = (int)floorf(g_vis_x + 0.5f), cy = (int)floorf(g_vis_y + 0.5f), vx = cx, vy = cy;
      HdWorld_ClampView(&cx, &cy);
      if (cx != vx) g_vis_x = (float)cx;
      if (cy != vy) g_vis_y = (float)cy;
    }
  } else if (mode == 2) {
    // Durante il passaggio fra aree la vista si muove esattamente come Link
    if (g_vis_valid)
      g_vis_x += mx, g_vis_y += my;
    else
      g_vis_x = (float)gx, g_vis_y = (float)gy;
  } else {
    // Appena finito il passaggio il gioco fissa il punto dello schermo in cui
    // tiene Link (camera_*_coord_scroll_hi) a modo suo, anche 20-30 pixel
    // lontano da dove Link e' nella vista. Lo si sposta dove Link e' davvero:
    // la vista continua a seguirlo senza recuperi, e la telecamera del gioco
    // riparte quando la vista la raggiunge.
    if ((prev_mode == 2 || area_changed) && g_vis_valid) {
      int px = lx - (int)floorf(g_vis_x + 0.5f), py = ly - (int)floorf(g_vis_y + 0.5f);
      if (px > 16 && px < 240) {
        camera_x_coord_scroll_hi = gx + px;
        camera_x_coord_scroll_low = camera_x_coord_scroll_hi + 2;
      }
      if (py > 16 && py < 208) {
        camera_y_coord_scroll_hi = gy + py;
        camera_y_coord_scroll_low = camera_y_coord_scroll_hi + 2;
      }
    }
    // Dove sarebbe la telecamera del gioco se non si fermasse ai bordi: tiene
    // Link al punto di scorrimento (camera_*_coord_scroll_hi), che resta fermo
    // quando la telecamera e' bloccata al bordo
    int tx = lx - (int16)(camera_x_coord_scroll_hi - gx);
    int ty = ly - (int16)(camera_y_coord_scroll_hi - gy);
    // Oltre il bordo solo dal lato in cui quella del gioco e' ferma
    if (gx != ow_scroll_vars0.xstart && tx < gx) tx = gx;
    if (gx != ow_scroll_vars0.xend && tx > gx) tx = gx;
    if (gy != ow_scroll_vars0.ystart && ty < gy) ty = gy;
    if (gy != ow_scroll_vars0.yend && ty > gy) ty = gy;
    if (!g_vis_valid && prev_module == 16) {
      // Uscita da una casa: durante il cerchio che si apre la vista e' quella
      // del gioco (le aree vicine comparirebbero fuori dal cerchio); da li'
      // scivola dolcemente al suo posto invece di saltare
      g_vis_x = (float)gx, g_vis_y = (float)gy;
      g_vis_glide = true;
    } else if (!g_vis_valid || InFluteArrival()) {
      // Arrivo con l'uccello: il punto di scorrimento si sistema durante la
      // discesa, la vista lo segue subito (prima lo recuperava a 1/4 di
      // pixel per frame e scivolava per secondi dopo l'atterraggio)
      g_vis_x = (float)tx, g_vis_y = (float)ty;
      g_vis_glide = false;
    } else if (g_vis_glide) {
      float dx = tx - g_vis_x, dy = ty - g_vis_y;
      if (fabsf(dx) < 0.5f && fabsf(dy) < 0.5f) {
        g_vis_x = (float)tx, g_vis_y = (float)ty;
        g_vis_glide = false;
      } else {
        g_vis_x += dx * 0.12f, g_vis_y += dy * 0.12f;
      }
    } else {
      // La vista insegue quel punto muovendosi al massimo quanto Link piu'
      // un quarto di pixel per frame: segue sempre Link, e le differenze
      // (dopo un passaggio il gioco ricalcola il punto di scorrimento anche
      // di 30 pixel) si recuperano piano, senza scatti ne' tremolii
      g_vis_x = Approach(g_vis_x, (float)tx, mx);
      g_vis_y = Approach(g_vis_y, (float)ty, my);
    }
    int vx = (int)floorf(g_vis_x + 0.5f), vy = (int)floorf(g_vis_y + 0.5f);
    int cx = vx, cy = vy;
    HdWorld_ClampView(&cx, &cy);
    if (cx != vx) g_vis_x = (float)cx;
    if (cy != vy) g_vis_y = (float)cy;
  }
  g_vis_valid = true;
  g_world_dx = (int)floorf(g_vis_x + 0.5f) - gx;
  g_world_dy = (int)floorf(g_vis_y + 0.5f) - gy;
  *cam_x = gx + g_world_dx;
  *cam_y = gy + g_world_dy;
}

// ---------------------------------------------------------------------------
// Immagini delle aree

enum { kMaxImages = 32 };  // 32: la vista 3D guarda lontano (hd_3d.c)

// Il gioco ferma la telecamera in modo che delle aree si vedano al massimo le
// prime 510 righe su 512 (o 1022 su 1024): le ultime 2 non si vedono mai e
// contengono pezzi di caselle non pensati per essere visti. Qui al loro posto
// si ripete la riga sopra, e il bordo sud della mappa finisce 2 righe prima.
enum { kHiddenRows = 2, kWorldBottom = 4096 - kHiddenRows };

// Oltre i quattro bordi della mappa del mondo non c'e' nessuna area: la
// vista si ferma come quella del gioco, e con lo schermo largo si ferma
// prima, cosi' neanche le colonne in piu' escono dalla mappa (anche se la
// telecamera del gioco, ferma a 0, le mostrerebbe). In verticale, se il
// gioco mostra qualche riga in piu' (gy), la vista puo' arrivare fin li';
// in fondo pero' il gioco si ferma contando 224 righe: con 240 la vista sale
// delle 16 in piu', cosi' l'ultima riga visibile resta quella del gioco.
// Fra la montagna (0x03, 0x05, 0x07 e le loro parti, anche nel Mondo Oscuro)
// e la pianura non si passa: il bordo dell'area verso l'altro livello fa da
// fine del mondo, come per il gioco (nord per le aree sotto la montagna, sud
// per quelle di montagna, e i lati dove accanto c'e' l'altro livello). Prima la vista mostrava l'altro livello nella
// nebbia e, uscendo dal Dominio di Zora, partiva dalla telecamera del gioco
// e poi saltava su
static bool MountainAbove(int head, int *top) {
  static const uint8 kMountain[] = { 0x03, 0x04, 0x0b, 0x0c, 0x05, 0x06, 0x0d, 0x0e, 0x07 };
  int a = head & 0x3f, x, y, s;
  AreaRect(a, &x, &y, &s);
  if (y == 0)
    return false;
  for (int cx = x; cx < x + s; cx += 512) {
    int up = ((y - 1) >> 9) * 8 + (cx >> 9);
    for (int i = 0; i < (int)sizeof(kMountain); i++)
      if (kMountain[i] == up) {
        *top = y;
        return true;
      }
  }
  return false;
}

static bool IsMountainCell(int cell) {
  static const uint8 kMountain[] = { 0x03, 0x04, 0x0b, 0x0c, 0x05, 0x06, 0x0d, 0x0e, 0x07 };
  for (int i = 0; i < (int)sizeof(kMountain); i++)
    if (kMountain[i] == cell)
      return true;
  return false;
}

// Colonna accanto all'area (cx) con almeno una casella dell'altro livello
static bool OtherLevelAt(int cx, int y, int s, bool mountain) {
  if (cx < 0 || cx >= 4096)
    return false;
  for (int cy = y; cy < y + s; cy += 512)
    if (IsMountainCell((cy >> 9) * 8 + (cx >> 9)) != mountain)
      return true;
  return false;
}

static bool PlainBelow(int head, int *bottom) {
  int a = head & 0x3f, x, y, s;
  if (a != 0x03 && a != 0x05 && a != 0x07)
    return false;
  AreaRect(a, &x, &y, &s);
  *bottom = y + s;
  return true;
}

void HdWorld_ClampView(int *cam_x, int *cam_y) {
  if (HdWorld_Active())
    ClampViewArea(cam_x, cam_y);
}

static void ClampViewArea(int *cam_x, int *cam_y) {
  Ppu *ppu = g_zenv.ppu;
  int gx = BG2HOFS_copy2, gy = BG2VOFS_copy2;
  int ext = ppu->extraLeftRight;
  // righe dello schermo (240 con extend_y); la riga 0 dello schermo e' la
  // riga 1 del fondale (HdWorld_Composite)
  int rows = (ppu->renderFlags & kPpuRenderFlags_Height240) ? 240 : 224;
  int min_x = ext, max_x = 4096 - 256 - ext;
  int min_y = 0, max_y = kWorldBottom - 1 - rows;
  int bottom;
  if (BYTE(overworld_screen_index) < 0x80 && PlainBelow(overworld_screen_index, &bottom))
    max_y = bottom - kHiddenRows - 1 - rows;
  if (gy < min_y) min_y = gy;
  if (gy - (rows - 224) > max_y) max_y = gy - (rows - 224);
  int top;
  if (BYTE(overworld_screen_index) < 0x80 && MountainAbove(overworld_screen_index, &top) && top > min_y && gy >= top)
    min_y = top;
  if (BYTE(overworld_screen_index) < 0x80) {
    int a = overworld_screen_index & 0x3f, x, y, s;
    AreaRect(a, &x, &y, &s);
    bool mountain = IsMountainCell(a);
    if (OtherLevelAt(x - 1, y, s, mountain) && gx >= x && x + ext > min_x)
      min_x = x + ext;
    if (OtherLevelAt(x + s, y, s, mountain) && gx <= x + s - 256 && x + s - 256 - ext < max_x)
      max_x = x + s - 256 - ext;
  }
  if (*cam_x < min_x) *cam_x = min_x;
  if (*cam_x > max_x) *cam_x = max_x;
  if (*cam_y < min_y) *cam_y = min_y;
  if (*cam_y > max_y) *cam_y = max_y;
}

// Casella animata (fiori, acqua...): nell'immagine sarebbe ferma, quindi la
// si ridisegna a ogni frame con il fotogramma attuale del gioco
typedef struct AnimCell {
  uint16 x, y;       // posizione nell'immagine
  uint16 entry;      // casella 8x8 (numero, palette, ribaltamenti)
  uint32 color[16];  // colori della sua palette (0 = fondo), con il layer
} AnimCell;

typedef struct AreaImage {
  uint32 key, version;
  int x, y, size;
  uint32 *pix;     // size * size pixel 0xLLRRGGBB
  uint32 last_use;
  AnimCell *anim;
  int n_anim, cap_anim;
} AreaImage;

// Caselle animate dell'esterno: la memoria video da 0x3C00 a 0x3FFF, che il
// gioco riscrive a ogni frame
enum { kAnimTileFirst = 0x1c0, kAnimTileEnd = 0x200 };

static AreaImage g_images[kMaxImages];
static uint32 g_use_counter;
static int g_renders_this_frame;

static uint32 AreaKey(int head) {
  return 1u << 31 | (uint32)sram_progress_indicator << 16 |
         (uint32)save_ow_event_info[head & 0x7f] << 8 | (head & 0x7f);
}

static inline uint32 Bgr15ToPixel(uint16 c, uint32 layer) {
  uint32 r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
  return layer << 24 | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

// Disegna il terreno (BG2) dell'area caricata in questo momento. Come fa il
// gioco con l'area corrente (hd_fx.c), l'acqua si marca come layer 7 (niente
// pennellate) e, sulla Montagna della Morte e alla Piramide, lo sfondo come
// layer 3 (fondale che lo shader non tocca).
static void DrawLoadedArea(AreaImage *img) {
  Ppu *ppu = g_zenv.ppu;
  const uint16 *map8 = GetMap16toMap8Table();
  const uint8 *attr8 = GetMap8toTileAttr();
  uint16 chr_base = ppu->bgLayer[1].tileAdr;
  int n16 = img->size / 16;
  int a = BYTE(overworld_screen_index), sc = a & 0x3f;
  bool backdrop_scene = sc == 0x03 || sc == 0x05 || sc == 0x07 || a == 0x5b;
  // sulla montagna le nuvole non hanno pennellate, come l'acqua (hd_fx.c,
  // HdFx_IsCloudColor)
  bool mountain = sc == 0x03 || sc == 0x05 || sc == 0x07;
  uint32 backdrop = Bgr15ToPixel(main_palette_buffer[0], backdrop_scene ? 3 : 5);
  for (int r = 0; r < n16; r++) {
    for (int c = 0; c < n16; c++) {
      uint16 m16 = dung_bg2[r * 64 + c];
      for (int q = 0; q < 4; q++) {
        uint16 e = map8[m16 * 4 + q];
        int tile = e & 0x3ff, pal = e >> 10 & 7;
        bool hf = (e & 0x4000) != 0, vf = (e & 0x8000) != 0;
        int ox = c * 16 + (q & 1) * 8, oy = r * 16 + (q >> 1) * 8;
        const uint16 *t = &ppu->vram[(chr_base + tile * 16) & 0x7fff];
        uint8 at = attr8[e & 0x1ff];
        bool water = (at == 0x08 || at == 0x09);
        bool cloud_tile = mountain && HdFx_IsCloudTile(e);
        if (tile >= kAnimTileFirst && tile < kAnimTileEnd) {
          if (img->n_anim == img->cap_anim) {
            int cap = img->cap_anim ? img->cap_anim * 2 : 256;
            AnimCell *p = realloc(img->anim, cap * sizeof(AnimCell));
            if (p)
              img->anim = p, img->cap_anim = cap;
          }
          if (img->n_anim < img->cap_anim) {
            AnimCell *ac = &img->anim[img->n_anim++];
            ac->x = ox, ac->y = oy, ac->entry = e;
            ac->color[0] = water && !backdrop_scene ? (backdrop & 0xffffff) | 7u << 24 : backdrop;
            for (int v = 1; v < 16; v++)
              ac->color[v] = Bgr15ToPixel(main_palette_buffer[pal * 16 + v],
                                          water || (cloud_tile && HdFx_IsCloudColor(e, v)) ? 7 : 1);
          }
        }
        for (int row = 0; row < 8; row++) {
          int rr = vf ? 7 - row : row;
          uint16 p01 = t[rr], p23 = t[rr + 8];
          uint32 *dst = img->pix + (oy + row) * img->size + ox;
          for (int col = 0; col < 8; col++) {
            int bit = hf ? col : 7 - col;
            int v = (p01 >> bit & 1) | (p01 >> (8 + bit) & 1) << 1 |
                    (p23 >> bit & 1) << 2 | (p23 >> (8 + bit) & 1) << 3;
            // sulla montagna lo sfondo e' il cielo anche nelle caselle che per
            // il gioco sono acqua profonda (il vuoto oltre i dirupi)
            dst[col] = v ? Bgr15ToPixel(main_palette_buffer[pal * 16 + v],
                                        water || (cloud_tile && HdFx_IsCloudColor(e, v)) ? 7 : 1) :
                       water && !backdrop_scene ? (backdrop & 0xffffff) | 7u << 24 : backdrop;
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Mappa unica del mondo
//
// Il gioco ha in memoria le caselle (map16) della sola area corrente, e fuori
// dai suoi bordi la mappa "ricomincia" dal lato opposto. Qui si tengono le
// caselle di tutte le aree del mondo, con le modifiche del mondo persistente:
// fuori dall'area corrente Link, i nemici e gli oggetti in volo leggono il
// terreno da qui. Cosi' si puo' stare a cavallo del bordo, e al cambio d'area
// Link non si ferma e non cammina da solo (overworld.c).

typedef struct AreaMap {
  uint32 key, version;  // AreaKey e StateVersion di quando e' stata letta
  uint16 tiles[64 * 64];  // come dung_bg2: 64 caselle per riga
} AreaMap;

static AreaMap g_maps[128];
static uint8 g_map_saved_ram[0x20000];
static uint16 g_map_saved_vram[0x8000];
static bool g_map_building;

static int HeadAt(int wx, int wy);

// Caselle dell'area caricata in questo momento (dopo le modifiche ricordate)
static void StoreMap(int head) {
  AreaMap *m = &g_maps[head & 0x7f];
  memcpy(m->tiles, dung_bg2, sizeof(m->tiles));
  m->key = AreaKey(head), m->version = StateVersion(head);
}

static bool MapReady(int head) {
  AreaMap *m = &g_maps[head & 0x7f];
  return m->key == AreaKey(head) && m->version == StateVersion(head);
}

// Le caselle di un'area: se mancano si leggono subito, come fa il gioco
// entrando nell'area, salvando e ripristinando tutto (anche a meta' della
// logica del gioco, o nella copia di un'area vicina)
static AreaMap *GetMap(int head) {
  if (MapReady(head))
    return &g_maps[head & 0x7f];
  if (g_map_building)
    return NULL;
  g_map_building = true;
  Ppu *ppu = g_zenv.ppu;
  memcpy(g_map_saved_ram, g_ram, sizeof(g_map_saved_ram));
  memcpy(g_map_saved_vram, ppu->vram, sizeof(g_map_saved_vram));
  overworld_screen_index = head;
  overworld_area_index = head;
  Overworld_LoadGFXAndScreenSize();
  Overworld_DecompressAndDrawAllQuadrants();
  Overworld_HandleOverlaysAndBombDoors();
  ApplyTiles(head, false);
  StoreMap(head);
  memcpy(g_ram, g_map_saved_ram, sizeof(g_map_saved_ram));
  memcpy(ppu->vram, g_map_saved_vram, sizeof(g_map_saved_vram));
  g_map_building = false;
  return &g_maps[head & 0x7f];
}

static bool WorldMapOn(void) {
  return FeatureOn() && !player_is_indoors && overworld_screen_index < 0x80 && main_module_index == 9;
}

int HdWorld_WorldTile(int px, int py) {
  if (!WorldMapOn() || px < 0 || py < 0 || px >= 4096 || py >= 4096)
    return -1;
  int ax, ay, as;
  AreaRect(overworld_screen_index, &ax, &ay, &as);
  if (px >= ax && px < ax + as && py >= ay && py < ay + as)
    return -1;  // area corrente: vale la mappa del gioco
  int head = HeadAt(px, py);
  if (head < 0)
    return -1;
  AreaMap *m = GetMap(head);
  if (!m)
    return -1;
  AreaRect(head, &ax, &ay, &as);
  return m->tiles[((py - ay) >> 4) * 64 + ((px - ax) >> 4)];
}

bool HdWorld_SeamlessOn(void) {
  return WorldMapOn();
}

bool HdWorld_OutsideArea(int px, int py) {
  if (!WorldMapOn())
    return false;
  int ax, ay, as;
  AreaRect(overworld_screen_index, &ax, &ay, &as);
  return px < ax || px >= ax + as || py < ay || py >= ay + as;
}

void HdWorld_LeaveArea(void) {
  if (WorldMapOn())
    StoreMap(overworld_screen_index);
}

// Una volta per frame di gioco: l'area corrente (con le ultime modifiche) e
// una delle aree del mondo non ancora lette, finche' ci sono tutte
static void UpdateWorldMap(void) {
  StoreMap(overworld_screen_index);
  int world = overworld_screen_index & 0x40;
  for (int sq = 0; sq < 64; sq++) {
    int head = Overworld_AreaHead(sq) | world;
    if (!MapReady(head)) {
      GetMap(head);
      return;
    }
  }
}

static uint8 g_saved_ram[0x20000];
static uint16 g_saved_vram[0x8000];

static void RenderArea(AreaImage *img, int head) {
  Ppu *ppu = g_zenv.ppu;
  memcpy(g_saved_ram, g_ram, sizeof(g_saved_ram));
  memcpy(g_saved_vram, ppu->vram, sizeof(g_saved_vram));

  // Stessa preparazione di quando si esce all'aperto da una casa
  overworld_screen_index = head;
  Overworld_LoadGFXAndScreenSize();
  InitializeTilesets();
  OverworldLoadScreensPaletteSet();
  Overworld_LoadPalettes(kOverworldBgPalettes[head], overworld_sprite_palettes[head]);
  Palette_SetOwBgColor();
  Overworld_CopyPalettesToCache();
  Overworld_DecompressAndDrawAllQuadrants();
  Overworld_HandleOverlaysAndBombDoors();
  ApplyTiles(head, false);  // cespugli tagliati e simili (mondo persistente)
  StoreMap(head);
  DrawLoadedArea(img);

  memcpy(g_ram, g_saved_ram, sizeof(g_saved_ram));
  memcpy(ppu->vram, g_saved_vram, sizeof(g_saved_vram));
}

static bool IsCached(int head) {
  uint32 key = AreaKey(head), ver = StateVersion(head);
  for (int i = 0; i < kMaxImages; i++)
    if (g_images[i].key == key && g_images[i].version == ver)
      return true;
  return false;
}

static AreaImage *GetAreaImage(int head) {
  uint32 key = AreaKey(head), ver = StateVersion(head);
  AreaImage *free_img = NULL;
  for (int i = 0; i < kMaxImages; i++) {
    AreaImage *img = &g_images[i];
    if (img->key == key && img->version == ver) {
      img->last_use = ++g_use_counter;
      return img;
    }
    if (!free_img || img->last_use < free_img->last_use)
      free_img = img;
  }
  // al massimo due aree nuove per frame, per non rallentare
  if (g_renders_this_frame >= 2)
    return NULL;
  g_renders_this_frame++;
  AreaImage *img = free_img;
  int x, y, size;
  AreaRect(head, &x, &y, &size);
  if (!img->pix || img->size != size) {
    free(img->pix);
    img->pix = malloc(size * size * 4);
    if (!img->pix) {
      img->key = 0;
      return NULL;
    }
  }
  img->key = key, img->version = ver, img->x = x, img->y = y, img->size = size;
  img->n_anim = 0;
  img->last_use = ++g_use_counter;
  RenderArea(img, head);
  return img;
}

// Area (testa) che contiene il punto del mondo, o -1 fuori dalla mappa
static int HeadAt(int wx, int wy) {
  if (wx < 0 || wy < 0 || wx >= 4096 || wy >= kWorldBottom)
    return -1;
  // durante lo specchio, prima del cambio, lo schermo mostra il mondo di prima
  int world = (overworld_screen_index & 0x40) ^ (g_warp_old_world ? 0x40 : 0);
  return Overworld_AreaHead((wy >> 9) * 8 + (wx >> 9)) | world;
}

// Prepara in anticipo, una per frame, le aree che confinano con quella
// corrente: quando compaiono sullo schermo sono gia' pronte
static void PrefetchNeighbors(void) {
  if (g_renders_this_frame)
    return;
  int ax, ay, as;
  AreaRect(overworld_screen_index, &ax, &ay, &as);
  int sx0 = ax / 512 - 1, sy0 = ay / 512 - 1, sx1 = (ax + as) / 512, sy1 = (ay + as) / 512;
  for (int sy = sy0; sy <= sy1; sy++)
    for (int sx = sx0; sx <= sx1; sx++) {
      if (sx < 0 || sy < 0 || sx > 7 || sy > 7)
        continue;
      int head = HeadAt(sx * 512, sy * 512);
      if (head >= 0 && !IsCached(head)) {
        GetAreaImage(head);
        return;
      }
    }
}

// Strato sovrapposto dell'area corrente (nebbia del Bosco Perduto, pioggia e
// buio del temporale, nuvole): il gioco lo mette sullo schermo secondario
// (BG1) e lo somma a meta' al terreno e agli sprite. Sullo schermo copre
// tutto, ma dove il terreno viene dalle immagini delle aree il disegno della
// PPU viene sostituito: lo si somma qui allo stesso modo, leggendolo dalla
// memoria video, su tutto quello che si disegna al posto della PPU (immagini,
// caselle animate, nemici fermi), cosi' il tempo atmosferico continua oltre
// il bordo dell'area.
//
// Sulla Montagna della Morte e alla Piramide lo stesso schermo secondario
// porta invece il panorama sotto le nuvole, che scorre piu' lento del
// terreno: il gioco lo somma solo allo sfondo (dove il terreno e'
// trasparente). Le immagini delle aree hanno li' il colore di fondo, a cui
// si somma qui il panorama dal vivo: continua oltre il bordo dell'area e
// non cambia quando si passa da un'area all'altra.
typedef struct OverlayCtx {
  bool on;     // strato sommato a meta' a tutto (nebbia, pioggia, nuvole)
  bool sky;    // strato sommato solo allo sfondo (panorama della montagna)
  bool half;   // somma dimezzata
  int x0, y0;  // posizione nello strato del pixel 0,0 dello schermo
} OverlayCtx;

// Vista 3D (hd_3d.c): il terreno si disegna senza lo strato, che poi si somma
// piatto su tutta la vista (HdWorld_OverlayFlat)
static bool g_overlay_suppressed;

static void OverlayBegin(OverlayCtx *o, int ext) {
  Ppu *ppu = g_zenv.ppu;
  bool sub = (ppu->screenEnabled[1] & 1) && !(ppu->screenEnabled[0] & 1) && ppu->addSubscreen &&
             !ppu->subtractColor && ppu->mode != 7 && !g_overlay_suppressed;
  o->on = sub && (ppu->mathEnabled & 2) && ppu->halfColor;
  o->sky = sub && !o->on && (ppu->mathEnabled & 0x20);
  o->half = ppu->halfColor;
  o->x0 = ppu->bgLayer[0].hScroll + ppu->bgBiasX - ext;
  o->y0 = ppu->bgLayer[0].vScroll + ppu->bgBiasY + 1;
  // nebbia del Bosco Perduto migliorata: la disegna hd_fx.c su tutto
  if (HdFx_FogReplacesOverlay())
    o->on = o->sky = false;
}

// Colore (BGR a 15 bit) dello strato BG1 nel pixel sx, sy dello schermo; -1
// dove e' trasparente
static int OverlayColor(const OverlayCtx *o, int sx, int sy) {
  Ppu *ppu = g_zenv.ppu;
  const BgLayer *bg = &ppu->bgLayer[0];
  int px = (o->x0 + sx) & 0x3ff, py = (o->y0 + sy) & 0x3ff;
  int tx = px >> 3 & 63, ty = py >> 3 & 63;
  uint16 adr = bg->tilemapAdr + ((ty & 31) << 5 | (tx & 31));
  if ((tx & 32) && bg->tilemapWider) adr += 0x400;
  if ((ty & 32) && bg->tilemapHigher) adr += bg->tilemapWider ? 0x800 : 0x400;
  uint16 e = ppu->vram[adr & 0x7fff];
  int row = (e & 0x8000) ? 7 - (py & 7) : (py & 7);
  int bit = (e & 0x4000) ? (px & 7) : 7 - (px & 7);
  const uint16 *t = &ppu->vram[(bg->tileAdr + (e & 0x3ff) * 16) & 0x7fff];
  uint16 p01 = t[row], p23 = t[row + 8];
  int v = (p01 >> bit & 1) | (p01 >> (8 + bit) & 1) << 1 | (p23 >> bit & 1) << 2 | (p23 >> (8 + bit) & 1) << 3;
  return v ? ppu->cgram[(e >> 10 & 7) * 16 + v] : -1;
}

int HdWorld_OverlayColorAt(int sx, int sy) {
  OverlayCtx o;
  OverlayBegin(&o, g_zenv.ppu->extraLeftRight);
  return OverlayColor(&o, sx, sy);
}

static inline uint32 OverlayBlend(const OverlayCtx *o, uint32 c, int sx, int sy) {
  if (o->sky) {
    uint32 l = c >> 24;
    if (l != 3 && l != 5)
      return c;  // solo lo sfondo
    // come la PPU: colore di fondo attuale piu' lo strato (somma dimezzata
    // se richiesto), o piu' il colore fisso dove lo strato e' trasparente.
    // Colori a 5 bit, R nei bit bassi.
    Ppu *ppu = g_zenv.ppu;
    int col = OverlayColor(o, sx, sy);
    bool half = o->half && col >= 0;
    if (col < 0)
      col = ppu->fixedColorR | ppu->fixedColorG << 5 | ppu->fixedColorB << 10;
    int base = ppu->cgram[0], ch[3];
    for (int k = 0; k < 3; k++) {
      int v = (base >> (5 * k) & 31) + (col >> (5 * k) & 31);
      if (half) v >>= 1;
      if (v > 31) v = 31;
      ch[k] = v << 3 | v >> 2;
    }
    return Dim((c & 0xff000000) | ch[0] << 16 | ch[1] << 8 | ch[2]);
  }
  if (!o->on)
    return c;
  int col = OverlayColor(o, sx, sy);
  if (col < 0)
    return c;  // trasparente: il terreno resta com'e'
  int r = col & 31, g = col >> 5 & 31, b = col >> 10 & 31;
  r = r << 3 | r >> 2, g = g << 3 | g >> 2, b = b << 3 | b >> 2;
  // anche lo strato segue la luminosita' del gioco (dissolvenze): senza, con la
  // pioggia le colonne oltre il bordo dell'area restavano chiare mentre il
  // resto dello schermo andava al nero (aprendo e chiudendo la mappa)
  uint32 oc = Dim((uint32)r << 16 | g << 8 | b);
  r = oc >> 16 & 255, g = oc >> 8 & 255, b = oc & 255;
  return (c & 0xff000000) | (((c >> 16 & 255) + r) >> 1) << 16 | (((c >> 8 & 255) + g) >> 1) << 8 | ((c & 255) + b) >> 1;
}

// Ridisegna le caselle animate delle immagini visibili con il fotogramma
// d'animazione attuale (stessa memoria video per tutte le aree dello stesso
// mondo), dove il fondale viene dalle immagini
static void DrawAnimatedCells(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y, bool all,
                              const OverlayCtx *ov) {
  Ppu *ppu = g_zenv.ppu;
  uint16 chr_base = ppu->bgLayer[1].tileAdr;
  // colonne in cui disegna il gioco (come in HdWorld_Composite): con la
  // telecamera morbida la vista va qualche pixel oltre, e li' anche le celle
  // dell'area corrente vengono dall'immagine (con il disegno di quando e'
  // stata fatta: sui bordi della palude restavano pezzi marroni)
  int ext = (width - 256) / 2;
  int in_x0 = (int16)BG2HOFS_copy - ext, in_x1 = (int16)BG2HOFS_copy + 256 + ext;
  // mulinello: all'arrivo il gioco carica solo le 256 colonne normali, le
  // altre ripetono un pezzo di mappa finche' il passaggio non e' finito
  if (main_module_index == 9 && submodule_index == 0x2e)
    in_x0 += ext, in_x1 -= ext;
  for (int i = 0; i < kMaxImages; i++) {
    AreaImage *img = &g_images[i];
    if (!img->key || !img->n_anim || ((img->key ^ BYTE(overworld_screen_index)) & 0x40) ||
        img->version != StateVersion(img->key & 0x7f) || img->key != AreaKey(img->key & 0x7f))
      continue;  // vuota, dell'altro mondo, o una versione vecchia dell'area (o di prima di un evento)
    if (img->x + img->size <= cam_x || img->x >= cam_x + width ||
        img->y + img->size <= cam_y || img->y >= cam_y + height)
      continue;
    for (int n = 0; n < img->n_anim; n++) {
      AnimCell *ac = &img->anim[n];
      int sx0 = img->x + ac->x - cam_x, sy0 = img->y + ac->y - cam_y;
      if (sx0 + 8 <= 0 || sy0 + 8 <= 0 || sx0 >= width || sy0 >= height)
        continue;
      // ultime righe nascoste: ripetono l'ultima riga visibile della cella,
      // con il fotogramma d'animazione attuale (come fa HdWorld_Composite con
      // il resto dell'immagine; prima restava il fotogramma dell'immagine, che
      // sulle sponde dei fiumi faceva una riga diversa sul confine)
      int last = img->size - kHiddenRows - 1 - ac->y;
      bool hidden = last < 7;
      // celle allineate a 8 pixel e aree a 512: questi controlli valgono per
      // tutta la cella
      int cwx = img->x + ac->x, cwy = img->y + ac->y;
      bool by_game = !all && HdWorld_InsideCurrentArea(cwx, cwy);
      bool part = by_game && (cwx < in_x0 || cwx + 8 > in_x1);  // in parte oltre le colonne del gioco
      if (by_game && !hidden && !part)
        continue;  // qui disegna gia' il gioco (tranne le righe nascoste)
      if (HeadAt(cwx, cwy) != (img->key & 0x7f))
        continue;  // coperta da un'altra area (o fuori dalla mappa)
      int e = ac->entry, tile = e & 0x3ff;
      bool hf = (e & 0x4000) != 0, vf = (e & 0x8000) != 0;
      const uint16 *t = &ppu->vram[(chr_base + tile * 16) & 0x7fff];
      for (int row = 0; row < 8; row++) {
        int sy = sy0 + row;
        if ((unsigned)sy >= (unsigned)height)
          continue;
        int src = row;
        bool game_row = by_game;
        if (hidden && row > last)
          src = last, game_row = false;
        else if (by_game && !part)
          continue;
        uint32 *dst = (uint32 *)(pixels + sy * pitch);
        int rr = vf ? 7 - src : src;
        uint16 p01 = t[rr], p23 = t[rr + 8];
        for (int col = 0; col < 8; col++) {
          int sx = sx0 + col;
          if ((unsigned)sx >= (unsigned)width)
            continue;
          if (game_row && cwx + col >= in_x0 && cwx + col < in_x1)
            continue;
          uint32 l = dst[sx] >> 24;
          if (l == 2 || l == 4 || l == 6)
            continue;  // HUD e sprite restano davanti
          int bit = hf ? col : 7 - col;
          int v = (p01 >> bit & 1) | (p01 >> (8 + bit) & 1) << 1 |
                  (p23 >> bit & 1) << 2 | (p23 >> (8 + bit) & 1) << 3;
          dst[sx] = OverlayBlend(ov, Dim(ac->color[v]), sx, sy);
        }
      }
    }
  }
}

// Il gioco scurisce lo schermo abbassando i colori delle palette (rispetto
// alla copia intatta) e poi spegnendolo: le immagini delle aree hanno i
// colori pieni, quindi si scuriscono qui nella stessa misura
static void FadeLikeGame(uint8 *pixels, int pitch, int width, int height) {
  int cur[3] = { 0 }, full[3] = { 0 };
  for (int i = 0x20; i < 0x80; i++) {
    uint16 c = main_palette_buffer[i], a = aux_palette_buffer[i];
    for (int k = 0; k < 3; k++)
      cur[k] += c >> (5 * k) & 31, full[k] += a >> (5 * k) & 31;
  }
  int f[3];  // in 256esimi: rosso, verde, blu
  for (int k = 0; k < 3; k++)
    f[k] = g_zenv.ppu->forcedBlank ? 0 : full[k] ? (cur[k] * 256 / full[k] > 256 ? 256 : cur[k] * 256 / full[k]) : 256;
  if (f[0] == 256 && f[1] == 256 && f[2] == 256)
    return;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++) {
      uint32 c = row[x], l = c >> 24;
      if (l == 2 || l == 4 || l == 6)
        continue;  // HUD e sprite li disegna gia' la PPU
      row[x] = l << 24 | ((c >> 16 & 255) * f[0] >> 8) << 16 | ((c >> 8 & 255) * f[1] >> 8) << 8 | ((c & 255) * f[2] >> 8);
    }
  }
}

// ---------------------------------------------------------------------------
// Nemici che seguono Link da un'area all'altra
//
// Un nemico che insegue Link non si ferma al bordo della sua area. Quando
// Link passa in un'altra area, o quando il nemico di un'area vicina entra in
// quella di Link, diventa un nemico del gioco vero (puo' ferire ed essere
// colpito) che non appartiene piu' a nessuna area: la sua area lo conta come
// sconfitto finche' il mondo non torna com'era. Se la grafica o i colori dei
// nemici dell'area di Link sono diversi da quelli della sua area, lo si
// disegna a parte con i suoi, in una copia della PPU.

enum { kMaxGuests = 16, kMaxPending = 8 };
// Inseguimento: cresce mentre il nemico si avvicina a Link e cala se si
// allontana; da kChaseFollow in su (e non troppo lontano) il nemico lo segue
enum { kChaseMax = 120, kChaseFollow = 16, kChaseNear = 48, kFollowDist = 192 };
// al cambio d'area segue Link anche chi gli e' cosi' vicino, inseguendolo o no
enum { kNearFollow = 128 };

// Grafica dei nemici: seconda tabella degli sprite e loro colori
typedef struct GuestGfx {
  uint16 vram[0x1000];
  uint16 cgram[0x80];
} GuestGfx;

typedef struct Guest {
  bool used;
  uint8 k, type;
  GuestGfx gfx;
} Guest;

// Nemico che lascia la sua area vicina per entrare in quella di Link
typedef struct Pending {
  EnemyRec rec;
  uint8 area;  // area d'origine, 0xff = colpo o altro oggetto senza area
  GuestGfx gfx;
} Pending;

static Guest g_guests[kMaxGuests];
static Pending g_pending[kMaxPending];
static int g_n_pending, g_pending_room;
static GuestGfx g_area_gfx;  // grafica dell'area corrente all'ultimo frame di gioco
static bool g_area_gfx_valid;
static int g_last_head = -1;
static uint8 g_chase[16], g_chase_type[16];
static const uint8 *g_live_chase;  // inseguimento nella copia che fa il passo

// Vero se il nemico sta inseguendo Link (vedi ChaseStep), nel gioco vero o
// nella copia dell'area vicina
static bool Chasing(int k) {
  return (g_hd_neighbor_tick ? (g_live_chase ? g_live_chase[k] : 0) : g_chase[k]) >= kChaseFollow;
}
static bool g_carried[16];  // nemico che segue Link fuori dalla sua area
static uint16 g_chase_x[16], g_chase_y[16];
static Ppu *g_guest_ppu;
static uint32 *g_guest_pix, *g_guest_tmp;
static int g_guest_w, g_guest_h;
static bool g_guest_drawn;
// Voci OAM del gioco nascoste per questo disegno (si rimettono dopo: con
// piu' disegni per frame di gioco servono ancora)
static uint16 g_hidden_oam[128];
static int16 g_hidden_y[128];
static uint8 g_hidden_f0[128];
static bool g_hidden[128];

static void UnhideObj(Ppu *p, int i) {
  p->oam[i * 2] = g_hidden_oam[i], p->objY[i] = g_hidden_y[i], p->objShowF0[i] = g_hidden_f0[i];
  g_hidden[i] = false;
}

static void SaveGfx(GuestGfx *g, const Ppu *p) {
  for (int i = 0; i < 0x1000; i++)
    g->vram[i] = p->vram[(p->objTileAdr2 + i) & 0x7fff];
  memcpy(g->cgram, p->cgram + 0x80, sizeof(g->cgram));
}

static bool SameGfx(const GuestGfx *g, const Ppu *p) {
  if (memcmp(g->cgram, p->cgram + 0x80, sizeof(g->cgram)))
    return false;
  for (int i = 0; i < 0x1000; i++)
    if (g->vram[i] != p->vram[(p->objTileAdr2 + i) & 0x7fff])
      return false;
  return true;
}

static void LoadGfx(Ppu *p, const GuestGfx *g) {
  for (int i = 0; i < 0x1000; i++)
    p->vram[(p->objTileAdr2 + i) & 0x7fff] = g->vram[i];
  memcpy(p->cgram + 0x80, g->cgram, sizeof(g->cgram));
}

static Guest *GuestFind(int k) {
  for (int i = 0; i < kMaxGuests; i++)
    if (g_guests[i].used && g_guests[i].k == k)
      return &g_guests[i];
  return NULL;
}

static void GuestAdd(int k, const GuestGfx *gfx) {
  Guest *g = GuestFind(k);
  for (int i = 0; i < kMaxGuests && !g; i++)
    if (!g_guests[i].used)
      g = &g_guests[i];
  if (!g)
    return;  // si disegna con la grafica dell'area di Link
  g->used = true, g->k = k, g->type = sprite_type[k];
  memcpy(&g->gfx, gfx, sizeof(GuestGfx));
}

// Via quelli sconfitti, o il cui posto e' passato a un altro nemico
static void GuestsCheck(void) {
  for (int i = 0; i < kMaxGuests; i++) {
    Guest *g = &g_guests[i];
    if (g->used && (!sprite_state[g->k] || sprite_type[g->k] != g->type || sprite_N_word[g->k] != 0xffff))
      g->used = false;
  }
}

static void ChaseStep(uint8 *chase, int ox, int oy, int nx, int ny, int lx, int ly) {
  if (ox == nx && oy == ny)
    return;
  int d0 = abs(ox - lx) + abs(oy - ly), d1 = abs(nx - lx) + abs(ny - ly);
  if (d1 < d0)
    *chase = *chase + 1 > kChaseMax ? kChaseMax : *chase + 1;
  else if (d1 > d0 && d1 >= kChaseNear)  // vicino a Link (colpi, rimbalzi) non cala
    *chase = *chase > 2 ? *chase - 2 : 0;
}

static int LinkDist(int k) {
  return abs(SpriteWorldX(k) - link_x_coord) + abs(SpriteWorldY(k) - link_y_coord);
}

static bool IsFollower(int k, uint8 chase) {
  if (sprite_type[k] == 0x55 && !sprite_E[k])
    return false;  // lo Zora resta nella sua acqua (sprite_E: la sua palla di fuoco)
  int d = LinkDist(k);
  return sprite_state[k] == 9 && d < kFollowDist && (chase >= kChaseFollow || d < kNearFollow);
}

// Una volta per frame di gioco, nell'area corrente
static void TrackMain(void) {
  GuestsCheck();
  int ax, ay, as;
  AreaRect(BYTE(overworld_screen_index), &ax, &ay, &as);
  for (int k = 0; k < 16; k++) {
    // i nemici dell'area che camminano (non chi segue Link) non ne escono,
    // neanche quando vengono respinti; chi vola va dove vuole
    if (IsAreaEnemy(k) && !g_carried[k] && !FreeMover(k))
      KeepInArea(k, g_chase_x[k], g_chase_y[k], sprite_type[k] == g_chase_type[k], ax, ay, as);
    int x = SpriteWorldX(k), y = SpriteWorldY(k);
    if (!sprite_state[k] || sprite_type[k] != g_chase_type[k])
      g_chase[k] = 0, g_chase_type[k] = sprite_state[k] ? sprite_type[k] : 0xff, g_carried[k] = false;
    else
      ChaseStep(&g_chase[k], g_chase_x[k], g_chase_y[k], x, y, link_x_coord, link_y_coord);
    g_chase_x[k] = x, g_chase_y[k] = y;
    if (!IsMarkedCarried(k))
      carried_area[k] = 0;
  }
  SaveGfx(&g_area_gfx, g_zenv.ppu);
  g_area_gfx_valid = true;
  g_last_head = BYTE(overworld_screen_index);
  ReturnHomeWhenAway();
  ResetFarEnemies(link_x_coord, link_y_coord, g_ram);
  UpdateWorldMap();
}

void HdWorld_BeforeNewSprites(void) {
  if (!PersistOn() || g_last_head < 0 || g_last_head == BYTE(overworld_screen_index))
    return;
  AreaState *st = &g_states[g_last_head & 0x7f];
  int ax, ay, as;
  AreaRect(BYTE(overworld_screen_index), &ax, &ay, &as);
  for (int k = 0; k < 16; k++) {
    if (!sprite_state[k])
      continue;
    bool area = IsAreaEnemy(k), guest = sprite_N_word[k] == 0xffff && GuestFind(k);
    if (!area && !guest && !(sprite_N_word[k] == 0xffff && IsFollower(k, g_chase[k]))) {
      // colpi, resti e altri oggetti senza area (pietre degli Octorok...)
      // continuano il loro volo: alla fine del passaggio il gioco li
      // toglierebbe (Sprite_InitializeSlots)
      sprite_room[k] = BYTE(overworld_area_index);
      continue;
    }
    // chi vola (corvi...) ed e' gia' nell'area nuova resta con Link
    int cx = SpriteWorldX(k) + 8, cy = SpriteWorldY(k) + 8;
    bool flies_in = FreeMover(k) && cx >= ax && cx < ax + as && cy >= ay && cy < ay + as;
    if (!IsFollower(k, g_chase[k]) && !flies_in) {
      if (!area)
        sprite_room[k] = BYTE(overworld_area_index);
      // resta nella sua area (mondo persistente): li' lo muove la sua copia
      if (area)
        sprite_state[k] = 0;
      continue;
    }
    if (area) {
      uint16 blk = sprite_N_word[k];
      int j = 0;
      for (int i = 0; i < st->n_alive; i++)
        if (st->alive[i].blk != blk)
          st->alive[j++] = st->alive[i];
      st->n_alive = j;
      if (st->n_killed < kMaxKilled)
        st->killed[st->n_killed++] = blk;
      sprite_N_word[k] = 0xffff;
      MarkCarried(k, g_last_head, blk);
      if (g_area_gfx_valid)
        GuestAdd(k, &g_area_gfx);
    }
    g_chase[k] = kChaseMax;
    g_carried[k] = true;
    // alla fine del passaggio il gioco toglie i nemici nati in un'altra area
    // (Sprite_InitializeSlots): questo ora e' dell'area nuova
    sprite_room[k] = BYTE(overworld_area_index);
  }
}

// I nemici arrivati dalle aree vicine entrano nel gioco vero
static void InsertPending(void) {
  for (int i = 0; i < g_n_pending; i++) {
    int k = 15;
    while (k >= 0 && sprite_state[k])
      k--;
    if (k < 0)
      break;
    LoadFields(k, g_pending[i].rec.data);
    sprite_N_word[k] = 0xffff;
    if (g_pending[i].area != 0xff)
      MarkCarried(k, g_pending[i].area, g_pending[i].rec.blk);
    else
      carried_area[k] = 0;
    g_chase[k] = kChaseMax, g_chase_type[k] = sprite_type[k];
    g_chase_x[k] = SpriteWorldX(k), g_chase_y[k] = SpriteWorldY(k);
    g_carried[k] = true;
    sprite_room[k] = BYTE(overworld_area_index);
    GuestAdd(k, &g_pending[i].gfx);
  }
  g_n_pending = 0;
}

// Dopo il caricamento di un salvataggio (il mondo persistente e' ripartito
// da zero): chi seguiva Link resta sconfitto nella sua area
void HdWorld_AfterLoad(void) {
  g_room_n_arrows = 0;  // le frecce della stanza non vengono dal salvataggio
  memset(g_guests, 0, sizeof(g_guests));
  memset(g_carried, 0, sizeof(g_carried));
  g_n_pending = 0;
  g_area_gfx_valid = false;
  g_last_head = BYTE(overworld_screen_index);
  for (int k = 0; k < 16; k++) {
    g_chase[k] = 0, g_chase_type[k] = sprite_state[k] ? sprite_type[k] : 0xff;
    g_chase_x[k] = SpriteWorldX(k), g_chase_y[k] = SpriteWorldY(k);
  }
  if (!PersistOn() || player_is_indoors)
    return;
  for (int k = 0; k < 16; k++) {
    if (!IsMarkedCarried(k))
      continue;
    KeepCarriedKilled(carried_area[k] & 0x7f);
    g_carried[k] = true;
    g_chase[k] = kChaseMax;
  }
}

bool HdWorld_Carried(int k) {
  return g_carried[k] && sprite_state[k] && sprite_type[k] == g_chase_type[k] && PersistOn();
}

static void HideObj(Ppu *p, int i) {
  p->oam[i * 2] = (p->oam[i * 2] & 0xff) | 0xf000;
  p->objShowF0[i] = 0;
  p->objY[i] = -1000;
}

// Disegna a parte (sopra al fondale, dietro all'HUD: DrawGuests) le voci OAM
// scelte, con la grafica gfx (NULL: quella del gioco); nel gioco spariscono
static void RenderApart(const bool *sel, const GuestGfx *gfx, int width, int height) {
  Ppu *mp = g_zenv.ppu;
  if (!g_guest_ppu && !(g_guest_ppu = malloc(sizeof(Ppu))))
    return;
  if (!g_guest_pix || g_guest_w != width || g_guest_h != height) {
    free(g_guest_pix), free(g_guest_tmp);
    g_guest_pix = malloc((size_t)width * height * 4);
    g_guest_tmp = malloc((size_t)width * height * 4);
    g_guest_w = width, g_guest_h = height;
    if (!g_guest_pix || !g_guest_tmp) {
      free(g_guest_pix), free(g_guest_tmp);
      g_guest_pix = g_guest_tmp = NULL;
      return;
    }
  }
  if (!g_guest_drawn)
    memset(g_guest_pix, 0, (size_t)width * height * 4);
  // nella copia solo i pezzi scelti; nel gioco spariscono
  Ppu *p = g_guest_ppu;
  memcpy(p, mp, sizeof(Ppu));
  for (int i = 0; i < 128; i++)
    if (sel[i]) {
      g_hidden_oam[i] = mp->oam[i * 2], g_hidden_y[i] = mp->objY[i], g_hidden_f0[i] = mp->objShowF0[i];
      g_hidden[i] = true;
      HideObj(mp, i);
    }
    else
      HideObj(p, i);
  if (gfx) {
    LoadGfx(p, gfx);
    memset(p->objIce, 0, sizeof(p->objIce));
  }
  p->brightness = 15;  // la luminosita' del gioco si applica dopo (Dim)
  p->forcedBlank = false;
  PpuBeginDrawing(p, (uint8 *)g_guest_tmp, width * 4, mp->renderFlags);
  p->screenEnabled[0] = 0x10, p->screenEnabled[1] = 0;  // solo gli sprite
  p->screenWindowed[0] = p->screenWindowed[1] = 0;
  p->mathEnabled = 0, p->addSubscreen = p->subtractColor = p->halfColor = false;
  p->clipMode = p->preventMathMode = 0;
  p->mosaicEnabled = 0, p->mosaicSize = 1;
  // l'ombra morbida dei suoi pezzi va nella stessa mappa delle ombre del
  // gioco (objShadowOut e objCastShadow copiati da HdFx_ShadowsBeforeRender,
  // che ha gia' nascosto la sua ombra del gioco)
  for (int i = 0; i <= height; i++)
    ppu_runLine(p, i);
  int n = 0;
  for (int i = 0; i < width * height; i++) {
    uint32 c = g_guest_tmp[i], l = c >> 24;
    if ((l == 4 || l == 6) && !g_guest_pix[i])
      g_guest_pix[i] = c, n++;
  }
  if (!n) {
    // niente disegnato (fuori dallo schermo o altro): lo disegna il gioco,
    // anche con la grafica sbagliata, piuttosto che sparire
    for (int i = 0; i < 128; i++)
      if (sel[i])
        UnhideObj(mp, i);
    return;
  }
  g_guest_drawn = true;
}

void HdWorld_BeforeRender(int width, int height) {
  g_guest_drawn = false;
  if (!HdWorld_Active() || !(main_module_index == 9 || InMenuOverWorld()))
    return;
  Ppu *mp = g_zenv.ppu;
  bool sel[128];
  if (PersistOn()) {
    GuestsCheck();
    for (int gi = 0; gi < kMaxGuests; gi++) {
      Guest *g = &g_guests[gi];
      if (!g->used || SameGfx(&g->gfx, mp))
        continue;
      bool any = false;
      for (int i = 0; i < 128; i++)
        any |= sel[i] = g_sprite_oam_owner[i] == g->k;
      if (any)
        RenderApart(sel, &g->gfx, width, height);
    }
  }
  // Oltre il bordo dell'area il fondale viene dalle immagini delle aree
  // (HdWorld_Composite), ma la PPU ha li' caselle che non c'entrano: uno
  // sprite poteva finire dietro a quelle con la priorita' alta e sparire (il
  // soldato che segue Link, respinto da una freccia oltre il bordo). I pezzi
  // che sporgono oltre il bordo si disegnano a parte, sopra alle immagini.
  // (non nel menu: i suoi sprite vanno sopra all'HUD)
  if (main_module_index != 9 || InTransition() || InMosaicTransition() || !mp->objXValid || !mp->objYValid ||
      (g_world_dx == 0 && g_world_dy == 0 && mp->extraLeftRight == 0))
    return;
  int ext = (width - 256) / 2;
  int cam_x = (int16)BG2HOFS_copy + mp->bgBiasX - ext;  // come HdWorld_Composite
  int cam_y = (int16)BG2VOFS_copy + mp->bgBiasY + 1;
  int ax, ay, as;
  AreaRect(overworld_screen_index, &ax, &ay, &as);
  int in_x0 = (int16)BG2HOFS_copy - ext, in_x1 = (int16)BG2HOFS_copy + 256 + ext;
  if (in_x0 < ax) in_x0 = ax;
  if (in_x1 > ax + as) in_x1 = ax + as;
  int in_y1 = ay + as - kHiddenRows;
  bool any = false;
  for (int i = 0; i < 128; i++) {
    sel[i] = false;
    if (g_hidden[i])
      continue;
    int yy = mp->oam[i * 2] >> 8;
    if (yy == 0xf0 && !mp->objShowF0[i])
      continue;
    int size = (mp->oam[0x100 + (i >> 3)] >> ((i & 7) * 2) & 2) ? 16 : 8;
    int sx = mp->objX[i] - mp->objBiasX + ext, sy = mp->objY[i] - mp->objBiasY - 1;
    if (sx + size <= 0 || sx >= width || sy + size <= 0 || sy >= height)
      continue;
    int wx = cam_x + sx, wy = cam_y + sy;
    any |= sel[i] = wx < in_x0 || wx + size > in_x1 || wy < ay || wy + size > in_y1;
  }
  if (!any)
    return;
  // con un pezzo si disegnano a parte tutti quelli dello stesso personaggio:
  // divisi tra il gioco e il disegno a parte perdevano l'ordine (Link sul
  // confine aveva lo scudo sopra la testa)
  bool link = false, owner[256] = { 0 };
  for (int i = 0; i < 128; i++)
    if (sel[i]) {
      if (g_link_oam_mask[i])
        link = true;
      else if (g_sprite_oam_owner[i] != 0xff)
        owner[g_sprite_oam_owner[i]] = true;
    }
  for (int i = 0; i < 128; i++)
    if (!sel[i] && !g_hidden[i] && (g_link_oam_mask[i] ? link : g_sprite_oam_owner[i] != 0xff && owner[g_sprite_oam_owner[i]]))
      sel[i] = true;
  RenderApart(sel, NULL, width, height);
}

static void GuestRestoreOam(void) {
  for (int i = 0; i < 128; i++)
    if (g_hidden[i])
      UnhideObj(g_zenv.ppu, i);
}

// I nemici disegnati a parte, sopra al fondale e dietro all'HUD
static void DrawGuests(uint8 *pixels, int pitch, int width, int height, const OverlayCtx *ov) {
  if (!g_guest_drawn || g_guest_w != width || g_guest_h != height)
    return;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    const uint32 *src = g_guest_pix + y * width;
    for (int x = 0; x < width; x++) {
      uint32 c = src[x], dl = row[x] >> 24;
      if (!c || dl == 2 || dl == 4 || dl == 6)
        continue;
      row[x] = OverlayBlend(ov, Dim(c), x, y);
    }
  }
}

// ---------------------------------------------------------------------------
// Aree vicine dal vivo
//
// Ogni area vicina che tocca la vista ha una copia tutta sua dello stato del
// gioco (RAM e PPU), preparata come quando Link ci arriva: grafica e colori
// dei suoi nemici, mappa delle caselle, nemici letti dalla sua tabella con
// quelli sconfitti o spostati del mondo persistente. A ogni frame di gioco,
// in quella copia, i nemici fanno un passo (Sprite_Main) con la telecamera
// messa sulla vista: si attivano e spariscono come quelli dell'area corrente
// e vedono Link, che li' pero' non puo' essere ferito ne' colpire. Non
// escono dalla loro area (un muro invisibile sul bordo), tranne chi insegue
// Link e entra nella sua area: passa al gioco vero. Il loro stato va nel
// mondo persistente: quando Link entra nell'area il gioco li ritrova dove
// erano. La PPU della copia disegna solo i suoi sprite, che si mettono sul
// fotogramma oltre il bordo.

// Tutto il mondo e' attivo: ogni area del mondo di Link ha la sua copia, che
// fa un passo a ogni frame. Solo quelle che toccano la vista (near) si
// disegnano. Si prepara al massimo una copia per frame, prima le vicine.
enum { kMaxLive = 64, kLiveMargin = 96 };

typedef struct LiveArea {
  bool used;
  int head;            // area (con il bit del mondo)
  uint32 key;          // AreaKey quando e' stata preparata
  uint32 wanted_at;    // ultimo frame in cui toccava la vista
  bool near;           // tocca la vista (con il margine): si disegna
  bool fresh;          // ha fatto un passo: gli sprite vanno ridisegnati
  bool drawn;          // pix contiene gli sprite dell'ultimo passo
  uint8 *ram;          // copia di g_ram
  Ppu *ppu;            // copia della PPU
  uint32 *pix;         // sprite disegnati dalla sua PPU
  int pix_w, pix_h;
  uint8 chase[16], chase_type[16];  // inseguimento di Link (vedi ChaseStep)
} LiveArea;

// Posizione e stato di Link (quello vero) da dare alle copie
typedef struct LiveLink {
  uint16 x, y, z;
  uint8 lower, facing, frame, freeze;
  uint8 cur;  // area di Link
} LiveLink;

static LiveArea g_live[kMaxLive];
static uint32 g_live_frame;
static Ppu *g_saved_ppu;
bool g_hd_neighbor_tick;

static void LiveDropAll(void) {
  for (int i = 0; i < kMaxLive; i++)
    g_live[i].used = false;
}

static void LiveDropArea(int a) {
  for (int i = 0; i < kMaxLive; i++)
    if ((g_live[i].head & 0x7f) == a)
      g_live[i].used = false;
}

// Un nemico che seguiva Link e lo ha perso di vista (il gioco lo toglie)
// torna alla sua area: li' non conta piu' come sconfitto e ricompare al
// punto di partenza, invece di sparire per sempre. Ma solo quando Link non
// e' nell'area della sua casa (ReturnHomeWhenAway): altrimenti ricomparirebbe
// davanti a Link, che lo ha appena lasciato in un'altra area
void HdWorld_CarriedLost(int k) {
  if (!PersistOn() || g_hd_neighbor_tick || !IsMarkedCarried(k))
    return;
  int a = carried_area[k] & 0x7f;
  uint16 blk = carried_blk[k];
  carried_area[k] = 0;
  g_carried[k] = false;
  if (IsHoming(a, blk))
    return;
  if (g_n_homing < kMaxHoming)
    g_homing[g_n_homing].area = a, g_homing[g_n_homing].blk = blk, g_n_homing++;
  else
    ReturnHome(a, blk);
}

// Ritorni a casa in attesa: si fanno quando Link e' in un'altra area
static void ReturnHomeWhenAway(void) {
  int cur = BYTE(overworld_screen_index) & 0x7f, j = 0;
  for (int i = 0; i < g_n_homing; i++) {
    if (g_homing[i].area != cur)
      ReturnHome(g_homing[i].area, g_homing[i].blk);
    else
      g_homing[j++] = g_homing[i];
  }
  g_n_homing = j;
}

static void ReturnHome(int a, uint16 blk) {
  AreaState *st = &g_states[a];
  int j = 0;
  for (int i = 0; i < st->n_killed; i++)
    if (st->killed[i] != blk)
      st->killed[j++] = st->killed[i];
  st->n_killed = j;
  uint8 bit = 0x80 >> (blk & 7);
  size_t off = (uint8 *)&overworld_sprite_was_loaded[blk >> 3] - g_ram;
  if (a == (BYTE(overworld_screen_index) & 0x7f))
    g_ram[off] &= ~bit;
  for (int i = 0; i < kMaxLive; i++)
    if (g_live[i].used && (g_live[i].head & 0x7f) == a)
      g_live[i].ram[off] &= ~bit;
}

int HdWorld_AreaAt(int px, int py) {
  return WorldMapOn() ? HeadAt(px, py) : -1;
}

// Vero se nell'area i sassi cadono dall'alto (Overlord_SpawnBoulder)
static bool AreaHasBoulders(int head) {
  for (const uint8 *src = GetOverworldSpritePtr(head); src[0] != 0xff; src += 3)
    if (src[2] == 0xf4)
      return true;
  return false;
}

// Solo nelle copie delle aree vicine: un sasso dell'area di Link che scende
// nell'area sotto (visibile col mondo continuo) cade finche' esce dalla vista
bool HdWorld_BoulderGone(int k) {
  if (!g_hd_neighbor_tick)
    return false;
  int head = HdWorld_AreaAt(SpriteWorldX(k) + 8, SpriteWorldY(k) + 8);
  return head >= 0 && !AreaHasBoulders(head);
}

// Una casella di un'area vicina cambia (fuoco che passa il bordo, hd_burn.c):
// la ricorda il mondo persistente (l'area la ritrova entrando), e si
// aggiornano subito la mappa del mondo e la copia dal vivo dell'area;
// l'immagine dell'area si ridisegna da sola (StateVersion)
bool HdWorld_SetWorldTile(int px, int py, uint16 value) {
  if (!PersistOn() || !WorldMapOn() || HdWorld_WorldTile(px, py) < 0)
    return false;
  int head = HeadAt(px, py), ax, ay, as;
  AreaRect(head, &ax, &ay, &as);
  int idx = ((py - ay) >> 4) * 64 + ((px - ax) >> 4);
  bool ready = MapReady(head);
  if (!RememberExtraIn(&g_states[head & 0x7f], idx * 2, value))
    return false;
  if (ready) {
    AreaMap *m = &g_maps[head & 0x7f];
    m->tiles[idx] = value;
    m->version = StateVersion(head);
  }
  size_t off = (uint8 *)&dung_bg2[idx] - g_ram;
  for (int i = 0; i < kMaxLive; i++)
    if (g_live[i].used && g_live[i].head == head)
      memcpy(g_live[i].ram + off, &value, 2);
  return true;
}

// Gli sprite delle copie si vedono solo mentre si gioca all'aperto o con un
// menu aperto sopra il mondo (fermi)
static bool LiveVisibleNow(void) {
  return (main_module_index == 9 && submodule_index == 0) || InMenuOverWorld();
}

static bool LiveShown(int a) {
  if (!LiveVisibleNow())
    return false;
  for (int i = 0; i < kMaxLive; i++)
    if (g_live[i].used && g_live[i].near && g_live[i].drawn && (g_live[i].head & 0x7f) == a)
      return true;
  return false;
}

static bool SaveMain(void) {
  if (!g_saved_ppu && !(g_saved_ppu = malloc(sizeof(Ppu))))
    return false;
  memcpy(g_saved_ram, g_ram, sizeof(g_saved_ram));
  memcpy(g_saved_ppu, g_zenv.ppu, sizeof(Ppu));
  return true;
}

static void RestoreMain(void) {
  memcpy(g_ram, g_saved_ram, sizeof(g_saved_ram));
  memcpy(g_zenv.ppu, g_saved_ppu, sizeof(Ppu));
}

// Telecamera della copia sulla vista. Una riga piu' su, e la PPU della copia
// sposta gli sprite di una riga in giu' (objBiasY = 1): con lo spostamento la
// PPU legge le posizioni verticali senza "fare il giro" in fondo allo schermo
static void SetLiveCamera(int vx, int vy) {
  BG2HOFS_copy2 = BG2HOFS_copy = vx;
  BG2VOFS_copy2 = BG2VOFS_copy = vy - 1;
}

// Prepara la copia di un'area partendo dallo stato del gioco (chi chiama
// salva e ripristina lo stato vero)
static bool LiveBuild(LiveArea *la, int head, int vx, int vy) {
  if (!la->ram && !(la->ram = malloc(sizeof(g_saved_ram))))
    return false;
  if (!la->ppu && !(la->ppu = malloc(sizeof(Ppu))))
    return false;
  // come all'uscita da una casa (RenderArea), con in piu' i nemici
  overworld_screen_index = head;
  overworld_area_index = head;
  Overworld_LoadGFXAndScreenSize();
  InitializeTilesets();
  OverworldLoadScreensPaletteSet();
  Overworld_LoadPalettes(kOverworldBgPalettes[head], overworld_sprite_palettes[head]);
  Palette_SetOwBgColor();
  Overworld_CopyPalettesToCache();
  Overworld_DecompressAndDrawAllQuadrants();
  Overworld_HandleOverlaysAndBombDoors();
  ApplyTiles(head, false);
  StoreMap(head);
  num_memorized_tiles = 0;
  follower_indicator = 0;  // il compagno di Link resta con Link
  SetLiveCamera(vx, vy);
  Sprite_DisableAll();  // nemici, oggetti in volo e scintille dell'area corrente
  for (int k = 0; k < 16; k++)
    sprite_state[k] = 0;
  Sprite_OverworldReloadAll_justLoad();
  memcpy(g_zenv.ppu->cgram, main_palette_buffer, 0x200);
  la->used = true;
  la->head = head;
  la->key = AreaKey(head);
  la->fresh = la->drawn = la->near = false;
  la->wanted_at = g_live_frame;
  memset(la->chase, 0, sizeof(la->chase));
  memset(la->chase_type, 0xff, sizeof(la->chase_type));
  memcpy(la->ram, g_ram, sizeof(g_saved_ram));
  memcpy(la->ppu, g_zenv.ppu, sizeof(Ppu));
  return true;
}

// Un passo dei nemici nella copia
static void LiveTick(LiveArea *la, int vx, int vy, const LiveLink *m) {
  memcpy(g_ram, la->ram, sizeof(g_saved_ram));
  memcpy(g_zenv.ppu, la->ppu, sizeof(Ppu));
  main_module_index = 9, submodule_index = 0;
  flag_unk1 = m->freeze;
  frame_counter = m->frame;
  link_x_coord = m->x, link_y_coord = m->y, link_z_coord = m->z;
  link_is_on_lower_level = m->lower;
  link_direction_facing = m->facing;
  // qui Link non puo' essere ferito ne' colpire, e non parla
  link_disable_sprite_damage = 1;
  player_oam_y_offset = 0x80;
  filtered_joypad_L = filtered_joypad_H = joypad1L_last = joypad1H_last = 0;
  SetLiveCamera(vx, vy);
  Sprite_ActivateAllProxima();
  Sprite_OverworldLoadAllEnemies();
  MakeRoomNearLink();
  uint8 was[16];
  uint16 px[16], py[16];
  for (int k = 0; k < 16; k++)
    was[k] = sprite_state[k], px[k] = SpriteWorldX(k), py[k] = SpriteWorldY(k);
  for (int i = 0; i < 128; i++)
    oam_buf[i].y = 0xf0;
  g_hd_neighbor_tick = true;
  g_live_chase = la->chase;
  Sprite_Main();
  NMI_PrepareSprites();
  g_hd_neighbor_tick = false;
  ResetFarEnemies(m->x, m->y, g_saved_ram);
  for (int k = 0; k < 16; k++) {
    if (!was[k] || !sprite_state[k] || sprite_type[k] != la->chase_type[k])
      la->chase[k] = 0, la->chase_type[k] = sprite_state[k] ? sprite_type[k] : 0xff;
    else
      ChaseStep(&la->chase[k], px[k], py[k], SpriteWorldX(k), SpriteWorldY(k), m->x, m->y);
  }
  // muro invisibile sul bordo dell'area per chi cammina: chi esce torna
  // dov'era. Chi insegue Link ed entra nella sua area passa al gioco vero (la
  // sua area lo conta come sconfitto). I colpi e gli altri oggetti nati
  // durante il gioco (pietre degli Octorok, palle di fuoco degli Zora...) e
  // i nemici che volano (FreeMover) passano il bordo: nell'area di Link
  // entrano nel gioco vero (possono colpirlo), altrove continuano qui
  int ax, ay, as, bx, by, bs;
  AreaRect(la->head, &ax, &ay, &as);
  AreaRect(m->cur, &bx, &by, &bs);
  for (int k = 0; k < 16; k++) {
    if (!sprite_state[k])
      continue;
    int cx = SpriteWorldX(k) + 8, cy = SpriteWorldY(k) + 8;
    if (cx >= ax && cx < ax + as && cy >= ay && cy < ay + as)
      continue;
    bool enemy = IsAreaEnemy(k);
    bool in_link = cx >= bx && cx < bx + bs && cy >= by && cy < by + bs;
    // sonda invisibile con cui un soldato cerca Link (Sprite_SpawnProbeAlways):
    // si riferisce al posto del suo soldato, quindi resta in questa copia
    if (!enemy && sprite_type[k] == 0x41) {
      sprite_state[k] = 0;
      continue;
    }
    bool free = FreeMover(k);
    if (in_link && (!enemy || free || (was[k] && IsFollower(k, la->chase[k]))) && g_n_pending < g_pending_room) {
      Pending *pe = &g_pending[g_n_pending++];
      SaveFields(k, pe->rec.data);
      SaveGfx(&pe->gfx, g_zenv.ppu);
      pe->rec.blk = sprite_N_word[k];
      pe->area = enemy ? la->head : 0xff;
      sprite_state[k] = 0;
      continue;
    }
    // colpi e nemici che volano continuano qui (anche nell'area di Link se
    // nel gioco vero non c'e' posto: riprovano al frame dopo)
    if (!enemy || free)
      continue;
    if (was[k]) {
      // (se era gia' fuori, per esempio uscito quando era nell'area di Link,
      // torna appena dentro il bordo invece di restare bloccato fuori)
      KeepInArea(k, px[k], py[k], true, ax, ay, as);
      // Zora che ha scelto dove riemergere oltre il bordo: riprova, altrimenti
      // riemergerebbe nel punto di prima, che puo' essere sulla riva
      if (sprite_type[k] == 0x55 && !sprite_E[k] && sprite_ai_state[k] == 1) {
        sprite_ai_state[k] = 0;
        sprite_delay_main[k] = 0;
        sprite_flags3[k] &= ~0x40;
      }
    } else {
      sprite_state[k] = 0;
    }
  }
  NMI_DoUpdates();  // sprite, colori e grafica nella PPU della copia
  SnapshotEnemies();
  memcpy(la->ram, g_ram, sizeof(g_saved_ram));
  memcpy(la->ppu, g_zenv.ppu, sizeof(Ppu));
  la->fresh = true;
}

static void GuestsFromCopies(void);

void HdWorld_TickNeighbors(void) {
  g_live_frame++;
  // fuori dal mondo esterno le copie non servono piu'; nei menu restano ferme
  if (!PersistOn() || (main_module_index != 9 && main_module_index != 14)) {
    LiveDropAll();
    return;
  }
  if (main_module_index != 9 || submodule_index != 0 || !HdWorld_Active())
    return;
  TrackMain();
  GuestsFromCopies();
  // anche nell'area di Link tutti i nemici sono presenti, non solo quelli
  // vicini alla telecamera
  Sprite_OverworldLoadAllEnemies();
  MakeRoomNearLink();
  int cur = BYTE(overworld_screen_index);
  int vx = BG2HOFS_copy2 + g_cam_dx, vy = BG2VOFS_copy2 + g_cam_dy;
  int ext = g_zenv.ppu->extraLeftRight;
  // tutte le aree del mondo di Link; vicine = toccano la vista, con un
  // margine (si disegnano, e si preparano per prime)
  int x0 = vx - ext - kLiveMargin, x1 = vx + 256 + ext + kLiveMargin;
  int y0 = vy - kLiveMargin, y1 = vy + 240 + kLiveMargin;
  int want[64], nw = 0;
  bool want_near[64];
  for (int sy = 0; sy < 8; sy++)
    for (int sx = 0; sx < 8; sx++) {
      int head = HeadAt(sx * 512, sy * 512), i = 0;
      if (head < 0 || head == cur)
        continue;
      bool near = sx * 512 + 512 > x0 && sx * 512 <= x1 && sy * 512 + 512 > y0 && sy * 512 <= y1;
      while (i < nw && want[i] != head)
        i++;
      if (i == nw)
        want[nw] = head, want_near[nw++] = near;
      else
        want_near[i] |= near;
    }
  // via le copie che non servono piu': l'area in cui e' entrato Link (ora la
  // muove il gioco), l'altro mondo, un evento che ha cambiato l'area
  for (int i = 0; i < kMaxLive; i++) {
    LiveArea *la = &g_live[i];
    if (!la->used)
      continue;
    int j = 0;
    while (j < nw && want[j] != la->head)
      j++;
    if (j == nw || la->head == cur || ((la->head ^ cur) & 0x40) || la->key != AreaKey(la->head)) {
      la->used = false;
      continue;
    }
    la->wanted_at = g_live_frame;
    la->near = want_near[j];
  }
  if (!nw)
    return;
  LiveLink m = { link_x_coord, link_y_coord, link_z_coord, link_is_on_lower_level,
                 link_direction_facing, frame_counter, flag_unk1, (uint8)cur };
  // posti liberi per i nemici che entrano nell'area di Link
  g_n_pending = g_pending_room = 0;
  for (int k = 0; k < 16; k++)
    g_pending_room += !sprite_state[k];
  if (g_pending_room > kMaxPending)
    g_pending_room = kMaxPending;
  uint8 owner[128];
  memcpy(owner, g_sprite_oam_owner, sizeof(owner));
  if (!SaveMain())
    return;
  // al massimo un'area nuova per frame, per non rallentare: prima le vicine
  for (int pass = 0, built = 0; pass < 2 && !built; pass++)
    for (int j = 0; j < nw && !built; j++) {
      if (want_near[j] != (pass == 0))
        continue;
      LiveArea *slot = NULL;
      bool present = false;
      for (int i = 0; i < kMaxLive && !present; i++)
        present = g_live[i].used && g_live[i].head == want[j];
      if (present)
        continue;
      for (int i = 0; i < kMaxLive && !slot; i++)
        if (!g_live[i].used)
          slot = &g_live[i];
      if (!slot)
        break;
      LiveBuild(slot, want[j], vx, vy);
      slot->near = want_near[j];
      RestoreMain();
      built = 1;
    }
  for (int i = 0; i < kMaxLive; i++) {
    LiveArea *la = &g_live[i];
    if (la->used && la->wanted_at == g_live_frame)
      LiveTick(la, vx, vy, &m);
  }
  RestoreMain();
  memcpy(g_sprite_oam_owner, owner, sizeof(owner));
  InsertPending();
}

// Chi segue Link prende la grafica della sua area dalla copia dal vivo di
// quell'area, di solito accanto: dopo il caricamento di un salvataggio i
// disegni a parte ripartono da zero, e quella presa dal gioco al passaggio
// d'area puo' non essere completa (il soldato restava senza corpo). Senza
// si disegnerebbe con le tile dell'area di Link, anche pezzi di Link
static void GuestsFromCopies(void) {
  static GuestGfx gfx;
  for (int k = 0; k < 16; k++) {
    if (!IsMarkedCarried(k))
      continue;
    int a = carried_area[k] & 0x7f;
    for (int i = 0; i < kMaxLive; i++) {
      LiveArea *la = &g_live[i];
      if (la->used && (la->head & 0x7f) == a) {
        SaveGfx(&gfx, la->ppu);
        GuestAdd(k, &gfx);
        break;
      }
    }
  }
}

// La PPU della copia disegna solo i suoi sprite, nella vista del frame
static void LiveRender(LiveArea *la, int width, int height) {
  Ppu *p = la->ppu, *mp = g_zenv.ppu;
  if (!la->pix || la->pix_w != width || la->pix_h != height) {
    free(la->pix);
    la->pix = malloc((size_t)width * height * 4);
    la->pix_w = width, la->pix_h = height;
    if (!la->pix) {
      la->drawn = false;
      return;
    }
  }
  p->brightness = 15;  // la luminosita' del gioco si applica dopo (Dim)
  p->forcedBlank = false;
  PpuBeginDrawing(p, (uint8 *)la->pix, width * 4, mp->renderFlags);
  p->extraLeftRight = mp->extraLeftRight;
  p->extraLeftCur = p->extraRightCur = mp->extraLeftRight;
  p->extraBottomCur = mp->extraBottomCur;
  p->screenEnabled[0] = 0x10, p->screenEnabled[1] = 0;  // solo gli sprite
  p->screenWindowed[0] = p->screenWindowed[1] = 0;
  p->mathEnabled = 0, p->addSubscreen = p->subtractColor = p->halfColor = false;
  p->clipMode = p->preventMathMode = 0;
  p->mosaicEnabled = 0, p->mosaicSize = 1;
  p->objBiasX = 0, p->objBiasY = 1;  // vedi SetLiveCamera
  p->bgBiasX = p->bgBiasY = 0;
  p->objYValid = p->objXValid = p->objOnTop = p->windowFullSides = false;
  p->objShadowOut = NULL;
  memset(p->objCastShadow, 0, sizeof(p->objCastShadow));
  memset(p->objIce, 0, sizeof(p->objIce));
  memset(p->objScale, 0, sizeof(p->objScale));
  for (int i = 0; i <= height; i++)
    ppu_runLine(p, i);
  la->fresh = false;
  la->drawn = true;
}

// Sprite delle aree vicine sul fotogramma, dietro all'HUD e agli sprite veri
static void DrawLive(uint8 *pixels, int pitch, int width, int height, const OverlayCtx *ov) {
  if (!LiveVisibleNow())
    return;
  int cur = BYTE(overworld_screen_index);
  for (int i = 0; i < kMaxLive; i++) {
    LiveArea *la = &g_live[i];
    if (!la->used || !la->near || la->head == cur || ((la->head ^ cur) & 0x40) || (!la->fresh && !la->drawn))
      continue;
    if (la->fresh || la->pix_w != width || la->pix_h != height)
      LiveRender(la, width, height);
    if (!la->drawn)
      continue;
    for (int y = 0; y < height; y++) {
      uint32 *row = (uint32 *)(pixels + y * pitch);
      const uint32 *src = la->pix + y * width;
      for (int x = 0; x < width; x++) {
        uint32 c = src[x], l = c >> 24, dl = row[x] >> 24;
        if ((l != 4 && l != 6) || dl == 2 || dl == 4 || dl == 6)
          continue;
        row[x] = OverlayBlend(ov, Dim(c), x, y);
      }
    }
  }
}

// Frecce piantate nei muri (mondo persistente), dietro all'HUD e agli sprite.
// Seguono il fondale (anche la sua posizione interpolata), come i muri.
static void DrawArrowList(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y,
                          const ArrowRec *list, int n) {
  Ppu *ppu = g_zenv.ppu;
  {
    for (int i = 0; i < n; i++) {
      const ArrowRec *r = &list[i];
      for (int p = 0; p < r->n; p++) {
        // gli sprite compaiono una riga sotto la loro coordinata
        int sx0 = r->x + r->dx[p] - cam_x, sy0 = r->y + r->dy[p] - cam_y + 1;
        if (sx0 + 8 <= 0 || sy0 + 8 <= 0 || sx0 >= width || sy0 >= height)
          continue;
        uint8 fl = r->flags[p];
        uint16 adr = (fl & 1) ? ppu->objTileAdr2 : ppu->objTileAdr1;
        const uint16 *t = &ppu->vram[(adr + r->chr[p] * 16) & 0x7fff];
        int pal = 0x80 + (fl >> 1 & 7) * 16;
        bool hf = (fl & 0x40) != 0, vf = (fl & 0x80) != 0;
        for (int row = 0; row < 8; row++) {
          int sy = sy0 + row;
          if ((unsigned)sy >= (unsigned)height)
            continue;
          uint32 *dst = (uint32 *)(pixels + sy * pitch);
          int rr = vf ? 7 - row : row;
          uint16 p01 = t[rr], p23 = t[rr + 8];
          for (int col = 0; col < 8; col++) {
            int sx = sx0 + col;
            if ((unsigned)sx >= (unsigned)width)
              continue;
            int bit = hf ? col : 7 - col;
            int v = (p01 >> bit & 1) | (p01 >> (8 + bit) & 1) << 1 |
                    (p23 >> bit & 1) << 2 | (p23 >> (8 + bit) & 1) << 3;
            uint32 l = dst[sx] >> 24;
            if (!v || l == 2 || l == 4 || l == 6)
              continue;
            dst[sx] = Dim(Bgr15ToPixel(ppu->cgram[pal + v], 4));
          }
        }
      }
    }
  }
}

static void DrawArrows(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y) {
  int cur = BYTE(overworld_screen_index);
  for (int a = 0; a < 128; a++) {
    AreaState *st = &g_states[a];
    if (st->n_arrows && !((a ^ cur) & 0x40))
      DrawArrowList(pixels, pitch, width, height, cam_x, cam_y, st->arrows, st->n_arrows);
  }
}

// Tutto quello che il mondo persistente aggiunge al fotogramma
static void DrawPersistent(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y,
                           const OverlayCtx *ov) {
  if (!PersistOn()) {
    DrawGuests(pixels, pitch, width, height, ov);  // pezzi oltre il bordo (HdWorld_BeforeRender)
    return;
  }
  DrawLive(pixels, pitch, width, height, ov);
  DrawGuests(pixels, pitch, width, height, ov);
  DrawFrozenEnemies(pixels, pitch, width, height, cam_x, cam_y);
  DrawArrows(pixels, pitch, width, height, cam_x, cam_y);
}

// Gioco normale: le frecce piantate dell'area si disegnano sopra lo schermo
// del gioco e si dimenticano al cambio d'area (anche entrando in una casa)
static int g_normal_arrows_area = -1;

static void NormalGameArrows(uint8 *pixels, int pitch, int width, int height) {
  Ppu *ppu = g_zenv.ppu;
  if (player_is_indoors) {
    // al chiuso: le frecce della stanza (anche col mondo continuo)
    if (!g_room_n_arrows || g_room_arrows_room != dungeon_room_index || !ShowsRoom())
      return;
    g_bright = ppu->forcedBlank ? 0 : ppu->brightness;
    int ext = (width - 256) / 2;
    DrawArrowList(pixels, pitch, width, height, (int16)BG2HOFS_copy + ppu->bgBiasX - ext,
                  (int16)BG2VOFS_copy + ppu->bgBiasY + 1, g_room_arrows, g_room_n_arrows);
    return;
  }
  if (FeatureOn())
    return;  // col mondo continuo le frecce restano (DrawPersistent)
  int a = KeepInNormalGame() ? BYTE(overworld_screen_index) & 0x7f : -1;
  if (a != g_normal_arrows_area) {
    if (g_normal_arrows_area >= 0)
      g_states[g_normal_arrows_area].n_arrows = 0;
    g_normal_arrows_area = a;
  }
  if (a < 0 || !g_states[a].n_arrows || (main_module_index != 9 && !InMenuOverWorld()) || ppu->mode == 7)
    return;
  g_bright = ppu->forcedBlank ? 0 : ppu->brightness;
  int ext = (width - 256) / 2;
  DrawArrows(pixels, pitch, width, height, (int16)BG2HOFS_copy + ppu->bgBiasX - ext,
             (int16)BG2VOFS_copy + ppu->bgBiasY + 1);
}

// ---------------------------------------------------------------------------
// Composizione del frame

void HdWorld_Composite(uint8 *pixels, int pitch, int width, int height) {
  g_renders_this_frame = 0;
  GuestRestoreOam();
  // frecce della stanza dimenticate cambiando stanza o uscendo
  int room = player_is_indoors ? dungeon_room_index : -1;
  if (room != g_room_arrows_room)
    g_room_n_arrows = 0, g_room_arrows_room = room;
  if (!HdWorld_Active()) {
    NormalGameArrows(pixels, pitch, width, height);
    return;
  }
  Ppu *ppu = g_zenv.ppu;
  g_bright = ppu->forcedBlank ? 0 : ppu->brightness;
  PreparePaletteMap();
  bool mosaic = InMosaicTransition();
  bool all = InTransition() || mosaic;
  // aspetto dei nemici, anche quando la vista coincide con quella del gioco
  if (main_module_index == 9 && submodule_index == 0 && PersistOn())
    CaptureEnemies(pixels, pitch, width, height);
  int ext = (width - 256) / 2;
  int cam_x = (int16)BG2HOFS_copy + ppu->bgBiasX - ext;  // la PPU tiene solo 10 bit
  // +1: la PPU non mostra la sua riga 0, quindi la riga 0 dello schermo e' la
  // riga 1 del fondale
  int cam_y = (int16)BG2VOFS_copy + ppu->bgBiasY + 1;
  OverlayCtx ov;
  OverlayBegin(&ov, ext);
  if (!all && g_world_dx == 0 && g_world_dy == 0 && ppu->extraLeftRight == 0) {
    DrawPersistent(pixels, pitch, width, height, cam_x, cam_y, &ov);
    PrefetchNeighbors();
    return;
  }
  int ax, ay, as;
  AreaRect(overworld_screen_index, &ax, &ay, &as);
  // La PPU ha le caselle dell'area solo attorno alla telecamera del gioco
  // (con lo schermo largo, ext pixel in piu' per lato). Ai bordi sinistro e
  // destro del mondo la vista si ferma piu' all'interno (HdWorld_ClampView):
  // le colonne oltre quelle caricate vengono dall'immagine dell'area.
  int in_x0 = (int16)BG2HOFS_copy - ext, in_x1 = (int16)BG2HOFS_copy + 256 + ext;
  // mulinello: all'arrivo il gioco carica solo le 256 colonne normali, le
  // altre ripetono un pezzo di mappa finche' il passaggio non e' finito
  if (main_module_index == 9 && submodule_index == 0x2e)
    in_x0 += ext, in_x1 -= ext;
  if (in_x0 < ax) in_x0 = ax;
  if (in_x1 > ax + as) in_x1 = ax + as;
  // area vicina non ancora disegnata (al massimo un frame): colore di fondo
  uint32 outside = Bgr15ToPixel(main_palette_buffer[0], 5);
  int last_head = -2;
  AreaImage *img = NULL;
  // Mosaico del gioco anche sulle immagini (blocchi allineati come nella PPU)
  int msz = mosaic && (ppu->mosaicEnabled & 3) && ppu->mosaicSize > 1 ? ppu->mosaicSize : 1;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    int wy = cam_y + y;
    if (msz > 1)
      wy = cam_y + (y + 1) - (y + 1) % msz - 1;
    for (int x = 0; x < width; x++) {
      int wx = cam_x + x - (msz > 1 ? x % msz : 0);
      bool inside = wx >= in_x0 && wx < in_x1 && wy >= ay && wy < ay + as - kHiddenRows;
      if (inside && !all)
        continue;
      uint32 l = row[x] >> 24;
      if (l == 2 || l == 4 || l == 6)
        continue;  // HUD e sprite restano
      int head = HeadAt(wx, wy);
      if (head < 0)
        continue;  // oltre la mappa (la vista si ferma prima: HdWorld_ClampView)
      if (head != last_head) {
        img = GetAreaImage(head);
        last_head = head;
      }
      if (!img) {
        if (!inside)
          row[x] = OverlayBlend(&ov, Dim(outside), x, y);
        continue;
      }
      int ix = wx - img->x, iy = wy - img->y;
      if (iy >= img->size - kHiddenRows)
        iy = img->size - kHiddenRows - 1;
      if ((unsigned)ix < (unsigned)img->size && (unsigned)iy < (unsigned)img->size) {
        row[x] = OverlayBlend(&ov, Dim(img->pix[iy * img->size + ix]), x, y);
      }
    }
  }
  if (!mosaic) {
    DrawAnimatedCells(pixels, pitch, width, height, cam_x, cam_y, all, &ov);
    DrawPersistent(pixels, pitch, width, height, cam_x, cam_y, &ov);
  }
  if (mosaic)
    FadeLikeGame(pixels, pitch, width, height);
  PrefetchNeighbors();
}

// ---------------------------------------------------------------------------
// Vista 3D (hd_3d.c): il terreno oltre i bordi dello schermo

// Punto del mondo che corrisponde al pixel (0, 0) del fotogramma (come in
// HdWorld_Composite)
void HdWorld_ViewOrigin(int width, int *x, int *y) {
  Ppu *ppu = g_zenv.ppu;
  int ext = (width - 256) / 2;
  *x = (int16)BG2HOFS_copy + ppu->bgBiasX - ext;
  *y = (int16)BG2VOFS_copy + ppu->bgBiasY + 1;
}

// Colore del terreno nel punto del mondo, preso dalle immagini delle aree.
// false: fuori dalla mappa o immagine non ancora pronta
bool HdWorld_SampleGround(int wx, int wy, uint32 *out) {
  static int last_head = -2;
  static AreaImage *last_img;
  int head = HeadAt(wx, wy);
  if (head < 0)
    return false;
  if (head != last_head || !last_img || last_img->key != AreaKey(head)) {
    last_img = GetAreaImage(head);
    last_head = last_img ? head : -2;
  }
  AreaImage *img = last_img;
  if (!img)
    return false;
  int ix = wx - img->x, iy = wy - img->y;
  if (iy >= img->size - kHiddenRows)
    iy = img->size - kHiddenRows - 1;
  if ((unsigned)ix >= (unsigned)img->size || (unsigned)iy >= (unsigned)img->size)
    return false;
  *out = Dim(img->pix[iy * img->size + ix]);
  return true;
}

// C'e' lo strato del tempo atmosferico (BG1 solo sullo schermo secondario,
// sommato al terreno)? Solo all'esterno.
bool HdWorld_OverlayPresent(void) {
  Ppu *ppu = g_zenv.ppu;
  if (overworld_screen_index >= 0x80 || player_is_indoors)
    return false;
  bool sub = (ppu->screenEnabled[1] & 1) && !(ppu->screenEnabled[0] & 1) && ppu->addSubscreen &&
             !ppu->subtractColor && ppu->mode != 7;
  return sub && (((ppu->mathEnabled & 2) && ppu->halfColor) || (ppu->mathEnabled & 0x20));
}

void HdWorld_SuppressOverlay(bool on) {
  g_overlay_suppressed = on;
}

// Somma lo strato piatto, nella posizione dello schermo, su tutta la vista 3D
// (terreno e cielo; HUD e sprite restano davanti, come gli sprite del gioco
// con le palette 0-3). uniform: invece del disegno (gocce del gioco) si somma
// solo il colore medio dello strato, cioe' il buio del temporale, quando le
// gocce le disegna gia' la pioggia HD
void HdWorld_OverlayFlat(uint8 *pixels, int pitch, int width, int height, bool uniform) {
  bool org = g_overlay_suppressed;
  g_overlay_suppressed = false;
  OverlayCtx ov;
  OverlayBegin(&ov, (width - 256) / 2);
  g_overlay_suppressed = org;
  if (!ov.on && !ov.sky)
    return;
  Ppu *ppu = g_zenv.ppu;
  g_bright = ppu->forcedBlank ? 0 : ppu->brightness;
  if (uniform && ov.on) {
    // colore medio dei pixel opachi dello strato e quanta parte copre
    int n = 0, opaque = 0, sr = 0, sg = 0, sb = 0;
    for (int y = 0; y < height; y += 4) {
      for (int x = 0; x < width; x += 4, n++) {
        int col = OverlayColor(&ov, x, y);
        if (col >= 0)
          opaque++, sr += col & 31, sg += col >> 5 & 31, sb += col >> 10 & 31;
      }
    }
    if (!opaque)
      return;
    int r = sr / opaque, g = sg / opaque, b = sb / opaque;
    uint32 oc = Dim((uint32)(r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2));
    int ar = oc >> 16 & 255, ag = oc >> 8 & 255, ab = oc & 255;
    int k = opaque * 256 / n;   // c + k * ((c + oc) / 2 - c)
    for (int y = 0; y < height; y++) {
      uint32 *row = (uint32 *)(pixels + y * pitch);
      for (int x = 0; x < width; x++) {
        uint32 c = row[x], l = c >> 24 & 0xf;
        int bit = l == 7 ? 1 : l == 3 ? 5 : l;
        if (l == 2 || l == 4 || l == 6 || !(ppu->mathEnabled & (1 << bit)))
          continue;
        int cr = c >> 16 & 255, cg = c >> 8 & 255, cb = c & 255;
        cr += (((cr + ar) >> 1) - cr) * k >> 8;
        cg += (((cg + ag) >> 1) - cg) * k >> 8;
        cb += (((cb + ab) >> 1) - cb) * k >> 8;
        row[x] = (c & 0xff000000) | cr << 16 | cg << 8 | cb;
      }
    }
    return;
  }
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++) {
      uint32 l = row[x] >> 24 & 0xf;
      if (l == 2 || l == 4 || l == 6)
        continue;
      // stessi strati a cui il gioco somma lo strato (7 = acqua, e' BG2;
      // 3 = fondale e cielo della vista 3D, come il colore di fondo)
      int bit = l == 7 ? 1 : l == 3 ? 5 : l;
      if (ov.on && !(ppu->mathEnabled & (1 << bit)))
        continue;
      row[x] = OverlayBlend(&ov, row[x], x, y);
    }
  }
}
