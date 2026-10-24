#include <math.h>
#include "zelda_rtl.h"
#include "variables.h"
#include "misc.h"
#include "nmi.h"
#include "poly.h"
#include "attract.h"
#include "snes/ppu.h"
#include "snes/snes_regs.h"
#include "snes/dma.h"
#include "spc_player.h"
#include "util.h"
#include "audio.h"
#include "assets.h"
#include "hd_light.h"
#include "hd_fx.h"
#include "hd_hud.h"
#include "hd_menu.h"
ZeldaEnv g_zenv;
uint8 g_ram[131072];

uint32 g_wanted_zelda_features;

static void Startup_InitializeMemory();

typedef struct SimpleHdma {
  const uint8 *table;
  const uint8 *indir_ptr;
  uint8 rep_count;
  uint8 mode;
  uint8 ppu_addr;
  uint8 indir_bank;
} SimpleHdma;
static void SimpleHdma_Init(SimpleHdma *c, DmaChannel *dc);
static void SimpleHdma_DoLine(SimpleHdma *c);

static const uint8 bAdrOffsets[8][4] = {
  {0, 0, 0, 0},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1},
  {0, 1, 2, 3},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1}
};
static const uint8 transferLength[8] = {
  1, 2, 2, 4, 4, 4, 2, 4
};
const uint16 kUpperBitmasks[] = { 0x8000, 0x4000, 0x2000, 0x1000, 0x800, 0x400, 0x200, 0x100, 0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1 };
const uint8 kLitTorchesColorPlus[] = {31, 8, 4, 0};
const uint8 kDungeonCrystalPendantBit[13] = {0, 0, 4, 2, 0, 16, 2, 1, 64, 4, 1, 32, 8};
const int8 kGetBestActionToPerformOnTile_x[4] = { 7, 7, -3, 16 };
const int8 kGetBestActionToPerformOnTile_y[4] = { 6, 24, 12, 12 };
#define AT_WORD(x) (uint8)(x), (x)>>8
// direct
static const uint8 kAttractDmaTable0[13] = {0x20, AT_WORD(0x00ff), 0x50, AT_WORD(0xe018), 0x50, AT_WORD(0xe018), 1, AT_WORD(0x00ff), 0};
static const uint8 kAttractDmaTable1[10] = {0x48, AT_WORD(0x00ff), 0x30, AT_WORD(0xd830), 1, AT_WORD(0x00ff), 0};
static const uint8 kHdmaTableForEnding[19] = {
  0x52, AT_WORD(0x600), 8, AT_WORD(0xe2), 8, AT_WORD(0x602), 5, AT_WORD(0x604), 0x10, AT_WORD(0x606), 0x81, AT_WORD(0xe2), 0,
};
static const uint8 kSpotlightIndirectHdma[7] = {0xf8, AT_WORD(0x1b00), 0xf8, AT_WORD(0x1bf0), 0};
static const uint8 kMapModeHdma0[7] = {0xf0, AT_WORD(0xdd27), 0xf0, AT_WORD(0xde07), 0};
static const uint8 kMapModeHdma1[7] = {0xf0, AT_WORD(0xdee7), 0xf0, AT_WORD(0xdfc7), 0};
static const uint8 kAttractIndirectHdmaTab[7] = {0xf0, AT_WORD(0x1b00), 0xf0, AT_WORD(0x1be0), 0};
static const uint8 kHdmaTableForPrayingScene[7] = {0xf8, AT_WORD(0x1b00), 0xf8, AT_WORD(0x1bf0), 0};

void zelda_ppu_write(uint32_t adr, uint8_t val) {
  assert(adr >= INIDISP && adr <= STAT78);
  ppu_write(g_zenv.ppu, (uint8)adr, val);
}

void zelda_ppu_write_word(uint32_t adr, uint16_t val) {
  zelda_ppu_write(adr, val);
  zelda_ppu_write(adr + 1, val >> 8);
}

static const uint8 *SimpleHdma_GetPtr(uint32 p) {
  switch (p) {

  case 0xCFA87: return kAttractDmaTable0;
  case 0xCFA94: return kAttractDmaTable1;
  case 0xebd53: return kHdmaTableForEnding;
  case 0x0F2FB: return kSpotlightIndirectHdma;
  case 0xabdcf: return kMapModeHdma0;             // mode7
  case 0xabdd6: return kMapModeHdma1;             // mode7
  case 0xABDDD: return kAttractIndirectHdmaTab;   // mode7
  case 0x2c80c: return kHdmaTableForPrayingScene;

  case 0x1b00: return (uint8 *)hdma_table_dynamic;
  case 0x1be0: return (uint8 *)hdma_table_dynamic + 0xe0;
  case 0x1bf0: return (uint8 *)hdma_table_dynamic + 0xf0;
  case 0xadd27: return (uint8*)kMapMode_Zooms1;
  case 0xade07: return (uint8*)kMapMode_Zooms1 + 0xe0;
  case 0xadee7: return (uint8*)kMapMode_Zooms2;
  case 0xadfc7: return (uint8*)kMapMode_Zooms2 + 0xe0;
  case 0x600: return &g_ram[0x600];
  case 0x602: return &g_ram[0x602];
  case 0x604: return &g_ram[0x604];
  case 0x606: return &g_ram[0x606];
  case 0xe2: return &g_ram[0xe2];
  default:
    assert(0);
    return NULL;
  }
}

static void SimpleHdma_Init(SimpleHdma *c, DmaChannel *dc) {
  if (!dc->hdmaActive) {
    c->table = 0;
    return;
  }
  c->table = SimpleHdma_GetPtr(dc->aAdr | dc->aBank << 16);
  c->rep_count = 0;
  c->mode = dc->mode | dc->indirect << 6;
  c->ppu_addr = dc->bAdr;
  c->indir_bank = dc->indBank;
}

static void SimpleHdma_DoLine(SimpleHdma *c) {
  if (c->table == NULL)
    return;
  bool do_transfer = false;
  if ((c->rep_count & 0x7f) == 0) {
    c->rep_count = *c->table++;
    if (c->rep_count == 0) {
      c->table = NULL;
      return;
    }
    if(c->mode & 0x40) {
      c->indir_ptr = SimpleHdma_GetPtr(c->indir_bank << 16 | c->table[0] | c->table[1] * 256);
      c->table += 2;
    }
    do_transfer = true;
  }
  if(do_transfer || c->rep_count & 0x80) {
    for(int j = 0, j_end = transferLength[c->mode & 7]; j < j_end; j++) {
      uint8 v = c->mode & 0x40 ? *c->indir_ptr++ : *c->table++;
      zelda_ppu_write(0x2100 + c->ppu_addr + bAdrOffsets[c->mode & 7][j], v);
    }
  }
  c->rep_count--;
}

// Spostamento visivo della telecamera fluida
int g_cam_dx, g_cam_dy;
// Mondo continuo: angolo della vista nel mondo all'ultimo frame, valido
// anche quando la telecamera del gioco salta al cambio d'area
int g_view_wx, g_view_wy;
bool g_view_valid;
// Spostamento totale applicato al fondale nel frame disegnato (telecamera
// fluida + interpolazione per schermi oltre i 60 Hz), letto anche dallo shader
int g_render_bias_x, g_render_bias_y;
// Frazione di frame trascorsa (0-1) per l'interpolazione; 1 = nessuna
float g_interp_alpha = 1.0f;

extern int g_render_bias_x, g_render_bias_y;
extern int g_lamp_cone_idx;

#include "hd_world.h"
#include "hd_trans.h"

static void ConfigurePpuSideSpace() {
  // Let PPU impl know about the maximum allowed extra space on the sides and bottom
  int extra_right = 0, extra_left = 0, extra_bottom = 0;
//  printf("main %d, sub %d  (%d, %d, %d)\n", main_module_index, submodule_index, BG2HOFS_copy2, room_bounds_x.v[2 | (quadrant_fullsize_x >> 1)], quadrant_fullsize_x >> 1);
  int mod = main_module_index;
  if (mod == 14)
    mod = saved_module_for_menu;
  // cerchio che si chiude (15) o si riapre (16): come dentro o fuori
  if (mod == 15 || mod == 16)
    mod = player_is_indoors ? 7 : 9;
  // caduta in un labirinto (Module11): dalla fase 3 la stanza nuova e' gia' caricata
  if (mod == 17 && subsubmodule_index >= 3)
    mod = 7;
  // mappa del mondo e mappa dell'Ocarina: nuvole anche oltre il bordo della mappa
  g_zenv.ppu->m7CloudFill = main_module_index == 14 && (submodule_index == 7 || submodule_index == 10);
  g_zenv.ppu->bg1ConeQuadrant = (mod == 7 && hdr_dungeon_dark_with_lantern && TS_copy != 0) ? g_lamp_cone_idx : -1;
  if (mod == 9) {
    if (main_module_index == 14 && submodule_index == 7 && overworld_map_state >= 4) {
      // World map
      extra_left = kPpuExtraLeftRight, extra_right = kPpuExtraLeftRight;
      extra_bottom = 16;
    } else {
      // outdoors
      extra_left = BG2HOFS_copy2 - ow_scroll_vars0.xstart;
      extra_right = ow_scroll_vars0.xend - BG2HOFS_copy2;
      extra_bottom = ow_scroll_vars0.yend - BG2VOFS_copy2;
    }
  } else if (mod == 7) {
    // indoors (anche col cono della Lanterna: la PPU tiene solo il suo
    // quadrante di BG1, vedi PpuClipLampCone)
    int qm = quadrant_fullsize_x >> 1;
    extra_left = IntMax(BG2HOFS_copy2 - room_bounds_x.v[qm], 0);
    extra_right = IntMax(room_bounds_x.v[qm + 2] - BG2HOFS_copy2, 0);

    int qy = quadrant_fullsize_y >> 1;
    extra_bottom = IntMax(room_bounds_y.v[qy + 2] - BG2VOFS_copy2, 0);
  } else if (mod == 20 || mod == 0 || mod == 1) {
    extra_left = kPpuExtraLeftRight, extra_right = kPpuExtraLeftRight;
    extra_bottom = 16;
  }
  if (HdWorld_Active()) {
    // Mondo continuo: oltre il bordo il fondale arriva dalle immagini delle aree
    PpuSetExtraSideSpace(g_zenv.ppu, kPpuExtraLeftRight, kPpuExtraLeftRight, 16);
    return;
  }
  if (g_render_bias_x | g_render_bias_y) {
    // Un valore negativo (durante le transizioni) vuol dire "nessun limite"
    if (extra_left >= 0) extra_left = IntMax(extra_left + g_render_bias_x, 0);
    if (extra_right >= 0) extra_right = IntMax(extra_right - g_render_bias_x, 0);
    if (extra_bottom >= 0) extra_bottom = IntMax(extra_bottom - g_render_bias_y, 0);
  }
  PpuSetExtraSideSpace(g_zenv.ppu, extra_left, extra_right, extra_bottom);
}

// Telecamera fluida: la telecamera logica del gioco resta intatta, si disegna
// solo da una posizione "visiva" che la insegue con un po' di ritardo.
// g_smooth_camera: 0 = spenta, 1 = leggera, 2 = media, 3 = forte.
int g_smooth_camera;
static float g_cam_vx, g_cam_vy;
static int g_cam_last_x, g_cam_last_y;  // Link nel frame precedente
static int g_cam_int_x, g_cam_int_y;    // posizione disegnata (pixel interi)
static bool g_cam_valid;

static bool SmoothCamera_IsGameplay() {
  int mod = main_module_index;
  // Con il mondo continuo il passaggio fra aree dura un frame (fase 1) e la
  // vista segue Link: la telecamera fluida non deve riallinearsi di colpo
  if (mod == 9 && (enhanced_features0 & kFeatures0_ContinuousWorld) &&
      submodule_index >= 1 && submodule_index <= 8 && BYTE(overworld_screen_index) < 0x80)
    return true;
  if (mod == 7 || mod == 9)
    return submodule_index == 0;
  // Dialogo aperto durante il gioco: il fondale resta dov'e'
  if (mod == 14 && submodule_index == 2)
    return saved_module_for_menu == 7 || saved_module_for_menu == 9;
  return false;
}

// Posizione visiva della telecamera negli ultimi due frame di gioco
static int g_vis_prev_x, g_vis_prev_y, g_vis_cur_x, g_vis_cur_y;

// Va chiamata una volta per frame di gioco (non per ogni disegno)
void ZeldaUpdateCamera() {
  static const float kFollow[4] = { 1.0f, 0.30f, 0.20f, 0.13f };
  enum { kMaxDelta = 24 };
  int gx = BG2HOFS_copy2, gy = BG2VOFS_copy2;
  // Con il mondo continuo si insegue la telecamera "libera" invece di quella
  // del gioco, che si ferma ai bordi dell'area
  int lx = gx, ly = gy;
  HdWorld_UpdateCamera(&lx, &ly);
  // Transizioni veloci: durante il passaggio la vista la decide hd_trans.c
  int trans_x, trans_y;
  bool trans = HdTrans_Update(g_cam_dx, g_cam_dy, &trans_x, &trans_y);
  int level = g_smooth_camera < 0 ? 0 : g_smooth_camera > 3 ? 3 : g_smooth_camera;
  if (trans)
    g_cam_valid = false;
  if (level == 0 || !SmoothCamera_IsGameplay() || !g_cam_valid ||
      abs(lx - (int)g_cam_vx) > 64 || abs(ly - (int)g_cam_vy) > 64) {
    g_cam_vx = (float)lx, g_cam_vy = (float)ly;
    g_cam_valid = (level != 0 && SmoothCamera_IsGameplay());
  } else {
    float k = kFollow[level];
    // Inseguimento con ritardo; quando Link inverte la direzione la
    // telecamera si ferma invece di continuare nel verso vecchio
    float sx = (lx - g_cam_vx) * k, sy = (ly - g_cam_vy) * k;
    int mx = link_x_coord - g_cam_last_x, my = link_y_coord - g_cam_last_y;
    if ((mx > 0 && sx < 0) || (mx < 0 && sx > 0)) sx = 0;
    if ((my > 0 && sy < 0) || (my < 0 && sy > 0)) sy = 0;
    g_cam_vx += sx;
    g_cam_vy += sy;
    // Restiamo entro il margine di tile gia' caricati in VRAM
    if (g_cam_vx < lx - kMaxDelta) g_cam_vx = (float)(lx - kMaxDelta);
    if (g_cam_vx > lx + kMaxDelta) g_cam_vx = (float)(lx + kMaxDelta);
    if (g_cam_vy < ly - kMaxDelta) g_cam_vy = (float)(ly - kMaxDelta);
    if (g_cam_vy > ly + kMaxDelta) g_cam_vy = (float)(ly + kMaxDelta);
  }
  // Pixel interi: Link cammina a passi alterni (1 e 2 pixel), mentre la
  // telecamera fluida va a velocita' costante; arrotondando, Link sullo
  // schermo tremerebbe di un pixel avanti e indietro. Mentre Link si muove la
  // telecamera avanza al massimo del suo passo e mai al contrario: a
  // velocita' costante fa i suoi stessi passi. Da fermo recupera un pixel
  // per frame, a ritmo regolare, puntando dritto a dove deve arrivare: con il
  // rallentamento graduale i pixel arrivavano a intervalli irregolari (1, 2, 3
  // frame) e con i passettini di Link la vista sembrava scattare.
  {
    int tx = (int)floorf(g_cam_vx + 0.5f), ty = (int)floorf(g_cam_vy + 0.5f);
    int mx = link_x_coord - g_cam_last_x, my = link_y_coord - g_cam_last_y;
    if (!g_cam_valid || abs(tx - g_cam_int_x) > 64 || abs(ty - g_cam_int_y) > 64) {
      g_cam_int_x = tx, g_cam_int_y = ty;
    } else {
      int dx = tx - g_cam_int_x, dy = ty - g_cam_int_y;
      if (mx) dx = (dx * mx < 0) ? 0 : (abs(dx) > abs(mx) ? mx : dx);
      else dx = lx > g_cam_int_x ? 1 : lx < g_cam_int_x ? -1 : 0;
      if (my) dy = (dy * my < 0) ? 0 : (abs(dy) > abs(my) ? my : dy);
      else dy = ly > g_cam_int_y ? 1 : ly < g_cam_int_y ? -1 : 0;
      g_cam_int_x += dx, g_cam_int_y += dy;
      // da fermo la posizione fluida riparte da quella disegnata
      if (!mx) g_cam_vx = (float)g_cam_int_x;
      if (!my) g_cam_vy = (float)g_cam_int_y;
      // sempre entro il margine di caselle caricate
      if (g_cam_int_x < lx - kMaxDelta) g_cam_int_x = lx - kMaxDelta;
      if (g_cam_int_x > lx + kMaxDelta) g_cam_int_x = lx + kMaxDelta;
      if (g_cam_int_y < ly - kMaxDelta) g_cam_int_y = ly - kMaxDelta;
      if (g_cam_int_y > ly + kMaxDelta) g_cam_int_y = ly + kMaxDelta;
    }
    // la telecamera fluida in ritardo non deve uscire dalla mappa del mondo
    HdWorld_ClampView(&g_cam_int_x, &g_cam_int_y);
  }
  g_cam_last_x = link_x_coord, g_cam_last_y = link_y_coord;
  g_cam_dx = g_cam_int_x - gx;
  g_cam_dy = g_cam_int_y - gy;
  if (trans)
    g_cam_dx += trans_x, g_cam_dy += trans_y;
  g_view_wx = g_cam_int_x, g_view_wy = g_cam_int_y;
  g_view_valid = HdWorld_Active();
  g_vis_prev_x = g_vis_cur_x, g_vis_prev_y = g_vis_cur_y;
  g_vis_cur_x = gx + g_cam_dx, g_vis_cur_y = gy + g_cam_dy;
}

// Calcola lo spostamento da applicare al disegno. Con lo schermo oltre i
// 60 Hz, fra due frame di gioco si disegna il fondale a meta' strada fra la
// posizione precedente e quella attuale. Gli sprite non si interpolano:
// Link, che la telecamera segue, resterebbe altrimenti tremolante.
static void ApplyRenderBias() {
  int ix = 0, iy = 0;
  if (g_interp_alpha < 1.0f) {
    int dx = g_vis_cur_x - g_vis_prev_x, dy = g_vis_cur_y - g_vis_prev_y;
    if (abs(dx) <= 24 && abs(dy) <= 24) {
      float back = 1.0f - g_interp_alpha;
      ix = -(int)floorf(dx * back + 0.5f);
      iy = -(int)floorf(dy * back + 0.5f);
    }
  }
  g_render_bias_x = g_cam_dx + ix;
  g_render_bias_y = g_cam_dy + iy;
  Ppu *ppu = g_zenv.ppu;
  ppu->bgBiasX = g_render_bias_x, ppu->bgBiasY = g_render_bias_y;
  ppu->objBiasX = g_cam_dx, ppu->objBiasY = g_cam_dy;
}

// Con la vista spostata (telecamera fluida o libera) la posizione verticale
// degli sprite, 8 bit relativi alla telecamera del gioco, e' ambigua: 229 puo'
// essere "poco sotto lo schermo" o "27 pixel sopra". Per ogni sprite si
// sceglie l'interpretazione piu' vicina a un oggetto vero del gioco (Link,
// nemici, oggetti in volo), visto che ogni sprite si disegna vicino al suo.
extern uint8 g_link_oam_mask[128], g_link_oam_real_f0[128], g_sprite_oam_owner[128], g_oam_real_f0[128];

// Fra v + k*period sceglie il valore la cui posizione sullo schermo (v - bias)
// cade nella finestra [lo, lo + period)
static int WrapInto(int v, int period, int bias, int lo) {
  int s = v - bias - lo;
  s = ((s % period) + period) % period;
  return s + lo + bias;
}

// Con la vista spostata (telecamera fluida o libera) la posizione degli
// sprite, relativa alla telecamera del gioco, va reinterpretata:
// - in verticale ha 8 bit: si sceglie il valore (a passi di 256) piu' vicino
//   a un oggetto vero del gioco (Link, nemici, oggetti in volo), visto che
//   ogni sprite si disegna vicino al suo;
// - in orizzontale ha 9 bit: il valore si legge nella finestra da 128 pixel
//   prima a 384 dopo la vista (gli sprite restano in vita fino a poco oltre
//   la vista: Sprite_PrepOamCoordOrDoubleRet, Ancilla_SetOam). I pezzi di
//   Link fanno eccezione: il gioco scrive
//   il 9° bit dando per scontato che Link sia sullo schermo, quindi si prende
//   il valore (a passi di 256) piu' vicino a Link.
static void ResolveSpritePositions(Ppu *ppu) {
  ppu->objYValid = ppu->objXValid = false;
  // col mondo continuo anche senza spostamento: i nemici fuori dallo schermo
  // del gioco hanno la posizione vera (SetOamHelper0 non li nasconde)
  if (ppu->objBiasY == 0 && ppu->objBiasX == 0 && !g_view_valid && !(g_trans_link_dx | g_trans_link_dy))
    return;
  int link_x = (int16)(link_x_coord - BG2HOFS_copy2) + 8;
  // Si confronta anche la x: un nemico fermo 256 pixel piu' in alto ma
  // lontano in orizzontale non deve "rubare" i pezzi di uno sprite vicino
  // (il corpo del vecchio della Montagna spariva sopra lo schermo)
  int oy[32], ox[32], n = 0, cy = BG2VOFS_copy2, cx = BG2HOFS_copy2;
  int slot[16];  // posto di ogni nemico in oy/ox, -1 = non attivo
  oy[n] = (int16)(link_y_coord - cy), ox[n++] = link_x - 8;
  for (int k = 0; k < 16; k++)
    if (slot[k] = -1, sprite_state[k]) {
      slot[k] = n;
      oy[n] = (int16)((sprite_y_hi[k] << 8 | sprite_y_lo[k]) - sprite_z[k] - cy);
      ox[n++] = (int16)((sprite_x_hi[k] << 8 | sprite_x_lo[k]) - cx);
    }
  for (int k = 0; k < kAncilla_Slots && n < 32; k++)
    if (ancilla_type[k]) {
      oy[n] = (int16)((ancilla_y_hi[k] << 8 | ancilla_y_lo[k]) - cy);
      ox[n++] = (int16)((ancilla_x_hi[k] << 8 | ancilla_x_lo[k]) - cx);
    }
  for (int i = 0; i < 128; i++) {
    int lo = ppu->oam[i * 2] & 0xff, yy = ppu->oam[i * 2] >> 8;
    int hi = ppu->oam[0x100 + (i >> 3)] >> ((i & 7) * 2) & 1;
    int x;
    if (g_link_oam_mask[i]) {
      x = lo;
      while (x + 4 < link_x - 128) x += 256;
      while (x + 4 >= link_x + 128) x -= 256;
    } else {
      x = WrapInto(lo + hi * 256, 512, 0, ppu->objBiasX - 128);  // attorno alla vista
    }
    ppu->objX[i] = (int16)x;
    // i pezzi di Link si riferiscono solo a Link (oy[0]): un nemico 256 pixel
    // piu' in la' li porterebbe fuori dallo schermo. Allo stesso modo i pezzi
    // di un nemico si riferiscono solo a lui (la spada di un soldato fuori
    // dallo schermo non deve finire accanto a Link); per gli altri (oggetti in
    // volo, effetti) l'oggetto piu' vicino
    int k0 = 0, k1 = n, o = g_sprite_oam_owner[i];
    if (g_link_oam_mask[i])
      k1 = 1;
    else if (o < 16 && slot[o] >= 0)
      k0 = slot[o], k1 = k0 + 1;
    int best = 1 << 30, y = yy;
    for (int c = -1; c <= 1; c++)
      for (int k = k0; k < k1; k++) {
        int d = abs(yy + c * 256 - oy[k]) + abs(x - ox[k]);
        if (d < best)
          best = d, y = yy + c * 256;
      }
    // transizioni veloci: Link si disegna dove cammina a velocita' costante
    if (g_link_oam_mask[i])
      x += g_trans_link_dx, y += g_trans_link_dy, ppu->objX[i] = (int16)x;
    ppu->objY[i] = (int16)y;
    // i pezzi di Link hanno la loro tabella; gli altri scritti con la y vera
    // 0xf0 (SetOamHelper0) non sono nascosti
    ppu->objShowF0[i] = g_link_oam_real_f0[i] || (!g_link_oam_mask[i] && g_oam_real_f0[i] && yy == 0xf0);
  }
  ppu->objYValid = ppu->objXValid = true;
}

void ZeldaDrawPpuFrame(uint8 *pixel_buffer, size_t pitch, uint32 render_flags) {
  SimpleHdma hdma_chans[2];

  PpuBeginDrawing(g_zenv.ppu, pixel_buffer, pitch, render_flags);

  dma_startDma(g_zenv.dma, HDMAEN_copy, true);

  SimpleHdma_Init(&hdma_chans[0], &g_zenv.dma->channel[6]);
  SimpleHdma_Init(&hdma_chans[1], &g_zenv.dma->channel[7]);

  // Cheat: Let the PPU impl know about the hdma perspective correction so it can avoid guessing.
  if ((render_flags & kPpuRenderFlags_4x4Mode7) && g_zenv.ppu->mode == 7) {
    if (hdma_chans[0].table == kMapModeHdma0)
      PpuSetMode7PerspectiveCorrection(g_zenv.ppu, kMapMode_Zooms1[0], kMapMode_Zooms1[223]);
    else if (hdma_chans[0].table == kMapModeHdma1)
      PpuSetMode7PerspectiveCorrection(g_zenv.ppu, kMapMode_Zooms2[0], kMapMode_Zooms2[223]);
    else if (hdma_chans[0].table == kAttractIndirectHdmaTab)
      PpuSetMode7PerspectiveCorrection(g_zenv.ppu, hdma_table_dynamic[0], hdma_table_dynamic[223]);
    else
      PpuSetMode7PerspectiveCorrection(g_zenv.ppu, 0, 0);
  }

  ApplyRenderBias();

  if (g_zenv.ppu->extraLeftRight != 0 || render_flags & kPpuRenderFlags_Height240)
    ConfigurePpuSideSpace();

  int height = render_flags & kPpuRenderFlags_Height240 ? 240 : 224;

  ResolveSpritePositions(g_zenv.ppu);
  // Durante il passaggio fra aree, dove la telecamera del gioco e' lontana
  // da Link, gli strati del gioco contengono caselle dell'area nuova messe a
  // caso (anche chiome d'albero davanti a Link); il fondale arriva dalle
  // immagini delle aree, quindi gli sprite vanno sempre davanti
  g_zenv.ppu->objOnTop = HdWorld_BackgroundFromImages();
  HdLight_BeforeRender(g_zenv.ppu);
  HdHud_BeforeRender(g_zenv.ppu);
  HdFx_IrisBeforeRender(g_zenv.ppu);
  HdFx_ShadowsBeforeRender(g_zenv.ppu, 256 + 2 * g_zenv.ppu->extraLeftRight, height);
  HdFx_IceBeforeRender(g_zenv.ppu);
  HdWorld_BeforeRender(256 + 2 * g_zenv.ppu->extraLeftRight, height);

  for (int i = 0; i <= height; i++) {
    if (i == 128 && irq_flag) {
      zelda_ppu_write(BG3HOFS, selectfile_var8);
      zelda_ppu_write(BG3HOFS, selectfile_var8 >> 8);
      zelda_ppu_write(BG3VOFS, 0);
      zelda_ppu_write(BG3VOFS, 0);
      if (irq_flag & 0x80) {
        irq_flag = 0;
        zelda_snes_dummy_write(NMITIMEN, 0x81);
      }
    }
    ppu_runLine(g_zenv.ppu, i);
    SimpleHdma_DoLine(&hdma_chans[0]);
    SimpleHdma_DoLine(&hdma_chans[1]);
  }
  HdLight_AfterRender(g_zenv.ppu);
  HdFx_IrisAfterRender(g_zenv.ppu);
  HdFx_ShadowsAfterRender(g_zenv.ppu);
  HdWorld_Composite(pixel_buffer, (int)pitch, 256 + 2 * g_zenv.ppu->extraLeftRight, height);

  // Illuminazione dinamica (non sulla mappa del mondo in Mode 7)
  if (g_zenv.ppu->mode != 7) {
    int frame_w = 256 + 2 * g_zenv.ppu->extraLeftRight;
    HdFx_SpriteShadows(pixel_buffer, (int)pitch, frame_w, height);
    HdFx_BeforeLight(pixel_buffer, (int)pitch, frame_w, height, g_render_bias_x, g_render_bias_y);
    HdLight_Apply(pixel_buffer, (int)pitch, frame_w, height, g_render_bias_x, g_render_bias_y);
    HdFx_AfterLight(pixel_buffer, (int)pitch, frame_w, height, g_render_bias_x, g_render_bias_y);
    HdFx_MarkWater(pixel_buffer, (int)pitch, frame_w, height, g_render_bias_x, g_render_bias_y);
    HdFx_MarkBackdrop(pixel_buffer, (int)pitch, frame_w, height);
    HdFx_SoftIris(pixel_buffer, (int)pitch, frame_w, height);
    HdFx_MenuBlur(pixel_buffer, (int)pitch, frame_w, height);
  }
}

void HdmaSetup(uint32 addr6, uint32 addr7, uint8 transfer_unit, uint8 reg6, uint8 reg7, uint8 indirect_bank) {
  Dma *dma = g_zenv.dma;
  if (addr6) {
    dma_write(dma, DMAP6, transfer_unit);
    dma_write(dma, BBAD6, reg6);
    dma_write(dma, A1T6L, addr6);
    dma_write(dma, A1T6H, addr6 >> 8);
    dma_write(dma, A1B6, addr6 >> 16);
    dma_write(dma, DAS60, indirect_bank);
  }
  dma_write(dma, DMAP7, transfer_unit);
  dma_write(dma, BBAD7, reg7);
  dma_write(dma, A1T7L, addr7);
  dma_write(dma, A1T7H, addr7 >> 8);
  dma_write(dma, A1B7, addr7 >> 16);
  dma_write(dma, DAS70, indirect_bank);
}

static void ZeldaInitializationCode() {
  zelda_snes_dummy_write(NMITIMEN, 0);
  zelda_snes_dummy_write(HDMAEN, 0);
  zelda_snes_dummy_write(MDMAEN, 0);

  Sound_LoadIntroSongBank();

  Startup_InitializeMemory();

  animated_tile_data_src = 0xa680;
  dma_source_addr_9 = 0xb280;
  dma_source_addr_14 = 0xb280 + 0x60;
  zelda_snes_dummy_write(NMITIMEN, 0x81);
}

static void ClearOamBuffer() {  // 80841e
  for (int i = 0; i < 128; i++)
    oam_buf[i].y = 0xf0;
  memset(g_oam_real_f0, 0, sizeof(g_oam_real_f0));
}

static void ZeldaRunGameLoop() {
  frame_counter++;
  ClearOamBuffer();
  Module_MainRouting();
  NMI_PrepareSprites();
  nmi_boolean = 0;
}

void ZeldaInitialize() {
  g_zenv.dma = dma_init(NULL);
  g_zenv.ppu = ppu_init();
  g_zenv.ram = g_ram;
  g_zenv.sram = (uint8*)calloc(8192, 1);
  g_zenv.vram = g_zenv.ppu->vram;
  g_zenv.player = SpcPlayer_Create();
  SpcPlayer_Initialize(g_zenv.player);
  dma_reset(g_zenv.dma);
  ppu_reset(g_zenv.ppu);
}

static void ZeldaRunPolyLoop() {
  if (intro_did_run_step && !nmi_flag_update_polyhedral) {
    Poly_RunFrame();
    intro_did_run_step = 0;
    nmi_flag_update_polyhedral = 0xff;
  }
}

void ZeldaRunFrameInternal(uint16 input, int run_what) {
  if (animated_tile_data_src == 0)
    ZeldaInitializationCode();

  if (run_what & 2)
    ZeldaRunPolyLoop();
  if (run_what & 1)
    ZeldaRunGameLoop();
  Interrupt_NMI(input);
}


static int IncrementCrystalCountdown(uint8 *a, int v) {
  int t = *a + v;
  *a = t;
  return t >> 8;
}

int frame_ctr_dbg;
static uint8 *g_emu_memory_ptr;
static ZeldaRunFrameFunc *g_emu_runframe;
static ZeldaSyncAllFunc *g_emu_syncall;

void ZeldaSetupEmuCallbacks(uint8 *emu_ram, ZeldaRunFrameFunc *func, ZeldaSyncAllFunc *sync_all) {
  g_emu_memory_ptr = emu_ram;
  g_emu_runframe = func;
  g_emu_syncall = sync_all;
}

static void EmuSynchronizeWholeState() {
  if (g_emu_syncall)
    g_emu_syncall();
}

// |ptr| must be a pointer into g_ram, will synchronize the RAM memory with the
// emulator.
static void EmuSyncMemoryRegion(void *ptr, size_t n) {
  uint8 *data = (uint8 *)ptr;
  assert(data >= g_ram && data < g_ram + 0x20000);
  if (g_emu_memory_ptr)
    memcpy(g_emu_memory_ptr + (data - g_ram), data, n);
}

static void Startup_InitializeMemory() {  // 8087c0
  memset(g_ram + 0x0, 0, 0x2000);
  main_palette_buffer[0] = 0;
  srm_var1 = 0;
  uint8 *sram = g_zenv.sram;
  if (WORD(sram[0x3e5]) != 0x55aa)
    WORD(sram[0x3e5]) = 0;
  if (WORD(sram[0x8e5]) != 0x55aa)
    WORD(sram[0x8e5]) = 0;
  if (WORD(sram[0xde5]) != 0x55aa)
    WORD(sram[0xde5]) = 0;
  INIDISP_copy = 0x80;
  flag_update_cgram_in_nmi++;
}

void ByteArray_AppendVl(ByteArray *arr, uint32 v) {
  for (; v >= 255; v -= 255)
    ByteArray_AppendByte(arr, 255);
  ByteArray_AppendByte(arr, v);
}

void saveFunc(void *ctx_in, void *data, size_t data_size) {
  ByteArray_AppendData((ByteArray *)ctx_in, data, data_size);
}

typedef struct LoadFuncState {
  uint8 *p, *pend;
} LoadFuncState;

void loadFunc(void *ctx, void *data, size_t data_size) {
  LoadFuncState *st = (LoadFuncState *)ctx;
  assert(st->pend - st->p >= data_size);
  memcpy(data, st->p, data_size);
  st->p += data_size;
}

static void InternalSaveLoad(SaveLoadFunc *func, void *ctx) {
  uint8 junk[58] = { 0 };
  func(ctx, junk, 27);
  func(ctx, g_zenv.player->ram, 0x10000);  // apu ram
  func(ctx, junk, 40); // junk
  dsp_saveload(g_zenv.player->dsp, func, ctx); // 3024 bytes of dsp
  func(ctx, junk, 15); // spc junk
  dma_saveload(g_zenv.dma, func, ctx); // 192 bytes of dma state
  ppu_saveload(g_zenv.ppu, func, ctx); // 66619 + 512 + 174
  func(ctx, g_zenv.sram, 0x2000);  // 8192 bytes of sram
  func(ctx, junk, 58); // snes junk
  func(ctx, g_zenv.ram, 0x20000);  // 0x20000 bytes of ram
  func(ctx, junk, 4); // snes junk
}

void ZeldaReset(bool preserve_sram) {
  frame_ctr_dbg = 0;
  dma_reset(g_zenv.dma);
  ppu_reset(g_zenv.ppu);
  memset(g_zenv.ram, 0, 0x20000);
  if (!preserve_sram)
    memset(g_zenv.sram, 0, 0x2000);
  ZeldaApuLock();
  ZeldaRestoreMusicAfterLoad_Locked(true);
  ZeldaApuUnlock();
  EmuSynchronizeWholeState();

}

static void LoadSnesState(SaveLoadFunc *func, void *ctx) {
  // Do the actual loading
  ZeldaApuLock();
  InternalSaveLoad(func, ctx);
  memcpy(g_zenv.ram + 0x1DBA0, g_zenv.ram + 0x1b00, 224 * 2); // hdma table was moved

  ZeldaRestoreMusicAfterLoad_Locked(false);
  ZeldaApuUnlock();
  EmuSynchronizeWholeState();
  HdWorld_ResetStates();  // il mondo persistente riparte dal salvataggio
  HdWorld_AfterLoad();
}

static void SaveSnesState(SaveLoadFunc *func, void *ctx) {
  memcpy(g_zenv.ram + 0x1b00, g_zenv.ram + 0x1DBA0, 224 * 2); // hdma table was moved
  ZeldaApuLock();
  ZeldaSaveMusicStateToRam_Locked();
  InternalSaveLoad(func, ctx);
  ZeldaApuUnlock();
}

typedef struct StateRecorder {
  uint16 last_inputs;
  uint32 frames_since_last;
  uint32 total_frames;

  // For replay
  uint32 replay_pos, replay_pos_last_complete;
  uint32 replay_frame_counter;
  uint32 replay_next_cmd_at;
  uint8 replay_cmd;
  bool replay_mode;

  ByteArray log;
  ByteArray base_snapshot;
} StateRecorder;

static StateRecorder state_recorder;

void StateRecorder_Init(StateRecorder *sr) {
  memset(sr, 0, sizeof(*sr));
}

void StateRecorder_RecordCmd(StateRecorder *sr, uint8 cmd) {
  int frames = sr->frames_since_last;
  sr->frames_since_last = 0;
  int x = (cmd < 0xc0) ? 0xf : 0x1;
  ByteArray_AppendByte(&sr->log, cmd | (frames < x ? frames : x));
  if (frames >= x)
    ByteArray_AppendVl(&sr->log, frames - x);
}

void StateRecorder_Record(StateRecorder *sr, uint16 inputs) {
  uint16 diff = inputs ^ sr->last_inputs;
  if (diff != 0) {
    sr->last_inputs = inputs;
    //    printf("0x%.4x %d: ", diff, sr->frames_since_last);
    //    size_t lb = sr->log.size;
    for (int i = 0; i < 12; i++) {
      if ((diff >> i) & 1)
        StateRecorder_RecordCmd(sr, i << 4);
    }
    //    while (lb < sr->log.size)
    //      printf("%.2x ", sr->log.data[lb++]);
    //    printf("\n");
  }
  sr->frames_since_last++;
  sr->total_frames++;
}

void StateRecorder_RecordPatchByte(StateRecorder *sr, uint32 addr, const uint8 *value, int num) {
  assert(addr < 0x20000);

  //  printf("%d: PatchByte(0x%x, 0x%x. %d): ", sr->frames_since_last, addr, *value, num);
  //  size_t lb = sr->log.size;
  int lq = (num - 1) <= 3 ? (num - 1) : 3;
  StateRecorder_RecordCmd(sr, 0xc0 | (addr & 0x10000 ? 2 : 0) | lq << 2);
  if (lq == 3)
    ByteArray_AppendVl(&sr->log, num - 1 - 3);
  ByteArray_AppendByte(&sr->log, addr >> 8);
  ByteArray_AppendByte(&sr->log, addr);
  for (int i = 0; i < num; i++)
    ByteArray_AppendByte(&sr->log, value[i]);
  //  while (lb < sr->log.size)
  //    printf("%.2x ", sr->log.data[lb++]);
  //  printf("\n");
}

void ReadFromFile(FILE *f, void *data, size_t n) {
  if (fread(data, 1, n, f) != n)
    Die("fread failed\n");
}

void StateRecorder_Load(StateRecorder *sr, FILE *f, bool replay_mode) {
  // todo: fix robustness on invalid data.
  uint32 hdr[8] = { 0 };
  ReadFromFile(f, hdr, sizeof(hdr));

  assert(hdr[0] == 1);

  sr->total_frames = hdr[1];
  ByteArray_Resize(&sr->log, hdr[2]);
  ReadFromFile(f, sr->log.data, sr->log.size);
  sr->last_inputs = hdr[3];
  sr->frames_since_last = hdr[4];

  ByteArray_Resize(&sr->base_snapshot, (hdr[5] & 1) ? hdr[6] : 0);
  ReadFromFile(f, sr->base_snapshot.data, sr->base_snapshot.size);

  sr->replay_next_cmd_at = 0;

  sr->replay_mode = replay_mode;
  if (replay_mode) {
    sr->frames_since_last = 0;
    sr->last_inputs = 0;
    sr->replay_pos = sr->replay_pos_last_complete = 0;
    sr->replay_frame_counter = 0;
    // Load snapshot from |base_snapshot_|, or reset if empty.

    if (sr->base_snapshot.size) {
      LoadFuncState state = { sr->base_snapshot.data, sr->base_snapshot.data + sr->base_snapshot.size };
      LoadSnesState(&loadFunc, &state);
      assert(state.p == state.pend);
    } else {
      ZeldaReset(false);
    }
  } else {
    // Resume replay from the saved position?
    sr->replay_pos = sr->replay_pos_last_complete = hdr[5] >> 1;
    sr->replay_frame_counter = hdr[7];
    sr->replay_mode = (sr->replay_frame_counter != 0);

    ByteArray arr = { 0 };
    ByteArray_Resize(&arr, hdr[6]);
    ReadFromFile(f, arr.data, arr.size);
    LoadFuncState state = { arr.data, arr.data + arr.size };
    LoadSnesState(&loadFunc, &state);
    ByteArray_Destroy(&arr);
    assert(state.p == state.pend);
  }
}

void StateRecorder_Save(StateRecorder *sr, FILE *f) {
  uint32 hdr[8] = { 0 };
  ByteArray arr = { 0 };
  SaveSnesState(&saveFunc, &arr);
  assert(sr->base_snapshot.size == 0 || sr->base_snapshot.size == arr.size);

  hdr[0] = 1;
  hdr[1] = sr->total_frames;
  hdr[2] = (uint32)sr->log.size;
  hdr[3] = sr->last_inputs;
  hdr[4] = sr->frames_since_last;
  hdr[5] = sr->base_snapshot.size ? 1 : 0;
  hdr[6] = (uint32)arr.size;
  // If saving while in replay mode, also need to persist
  // sr->replay_pos_last_complete and sr->replay_frame_counter
  // so the replaying can be resumed.
  if (sr->replay_mode) {
    hdr[5] |= sr->replay_pos_last_complete << 1;
    hdr[7] = sr->replay_frame_counter;
  }
  fwrite(hdr, 1, sizeof(hdr), f);
  fwrite(sr->log.data, 1, hdr[2], f);
  fwrite(sr->base_snapshot.data, 1, sr->base_snapshot.size, f);
  fwrite(arr.data, 1, arr.size, f);

  ByteArray_Destroy(&arr);
}

void StateRecorder_ClearKeyLog(StateRecorder *sr) {
  printf("Clearing key log!\n");
  sr->base_snapshot.size = 0;
  SaveSnesState(&saveFunc, &sr->base_snapshot);
  ByteArray old_log = sr->log;
  int old_frames_since_last = sr->frames_since_last;
  memset(&sr->log, 0, sizeof(sr->log));
  // If there are currently any active inputs, record them initially at timestamp 0.
  sr->frames_since_last = 0;
  if (sr->last_inputs) {
    for (int i = 0; i < 12; i++) {
      if ((sr->last_inputs >> i) & 1)
        StateRecorder_RecordCmd(sr, i << 4);
    }
  }
  if (sr->replay_mode) {
    // When clearing the key log while in replay mode, we want to keep
    // replaying but discarding all key history up until this point.
    if (sr->replay_next_cmd_at != 0xffffffff) {
      sr->replay_next_cmd_at -= old_frames_since_last;
      sr->frames_since_last = sr->replay_next_cmd_at;
      sr->replay_pos_last_complete = (uint32)sr->log.size;
      StateRecorder_RecordCmd(sr, sr->replay_cmd);
      int old_replay_pos = sr->replay_pos;
      sr->replay_pos = (uint32)sr->log.size;
      ByteArray_AppendData(&sr->log, old_log.data + old_replay_pos, old_log.size - old_replay_pos);
    }
    sr->total_frames -= sr->replay_frame_counter;
    sr->replay_frame_counter = 0;
  } else {
    sr->total_frames = 0;
  }
  ByteArray_Destroy(&old_log);
  sr->frames_since_last = 0;
}

uint16 StateRecorder_ReadNextReplayState(StateRecorder *sr) {
  assert(sr->replay_mode);
  while (sr->frames_since_last >= sr->replay_next_cmd_at) {
    int replay_pos = sr->replay_pos;
    if (replay_pos != sr->replay_pos_last_complete) {
      // Apply next command
      sr->frames_since_last = 0;
      if (sr->replay_cmd < 0xc0) {
        sr->last_inputs ^= 1 << (sr->replay_cmd >> 4);
      } else if (sr->replay_cmd < 0xd0) {
        int nb = 1 + ((sr->replay_cmd >> 2) & 3);
        uint8 t;
        if (nb == 4) do {
          nb += t = sr->log.data[replay_pos++];
        } while (t == 255);
        uint32 addr = ((sr->replay_cmd >> 1) & 1) << 16;
        addr |= sr->log.data[replay_pos++] << 8;
        addr |= sr->log.data[replay_pos++];
        do {
          g_ram[addr & 0x1ffff] = sr->log.data[replay_pos++];
          EmuSyncMemoryRegion(&g_ram[addr & 0x1ffff], 1);
        } while (addr++, --nb);
      } else {
        assert(0);
      }
    }
    sr->replay_pos_last_complete = replay_pos;
    if (replay_pos >= sr->log.size) {
      sr->replay_pos = replay_pos;
      sr->replay_next_cmd_at = 0xffffffff;
      break;
    }
    // Read the next one
    uint8 cmd = sr->log.data[replay_pos++], t;
    int mask = (cmd < 0xc0) ? 0xf : 0x1;
    int frames = cmd & mask;
    if (frames == mask) do {
      frames += t = sr->log.data[replay_pos++];
    } while (t == 255);
    sr->replay_next_cmd_at = frames;
    sr->replay_cmd = cmd;
    sr->replay_pos = replay_pos;
  }
  sr->frames_since_last++;
  // Turn off replay mode after we reached the final frame position
  if (++sr->replay_frame_counter >= sr->total_frames) {
    sr->replay_mode = false;
  }
  return sr->last_inputs;
}

void StateRecorder_StopReplay(StateRecorder *sr) {
  if (!sr->replay_mode)
    return;
  sr->replay_mode = false;
  sr->total_frames = sr->replay_frame_counter;
  sr->log.size = sr->replay_pos_last_complete;
}

#ifdef _DEBUG
// This can be used to read inputs from a text file for easier debugging
int InputStateReadFromFile() {
  static FILE *f;
  static uint32 next_ts, next_keys, cur_keys;
  char buf[64];
  char keys[64];

  while (state_recorder.total_frames == next_ts) {
    cur_keys = next_keys;
    if (!f)
      f = fopen("boss_bug.txt", "r");
    if (fgets(buf, sizeof(buf), f)) {
      if (sscanf(buf, "%d: %s", &next_ts, keys) == 1) keys[0] = 0;
      int i = 0;
      for (const char *s = keys; *s; s++) {
        static const char kKeys[] = "AXsSUDLRBY";
        const char *t = strchr(kKeys, *s);
        assert(t);
        i |= 1 << (t - kKeys);
      }
      next_keys = i;
    } else {
      next_ts = 0xffffffff;
    }
  }

  return cur_keys;
}
#endif

bool ZeldaRunFrame(int inputs) {

  // Avoid up/down and left/right from being pressed at the same time
  if ((inputs & 0x30) == 0x30) inputs ^= 0x30;
  if ((inputs & 0xc0) == 0xc0) inputs ^= 0xc0;

  inputs = HdMenu_FilterInput(inputs);

  frame_ctr_dbg++;

  bool is_replay = state_recorder.replay_mode;

  // Either copy state or apply state
  if (is_replay) {
    inputs = StateRecorder_ReadNextReplayState(&state_recorder);
  } else {
    //    input_state = InputStateReadFromFile();
    StateRecorder_Record(&state_recorder, inputs);

    // This is whether APUI00 is true or false, this is used by the ancilla code.
    uint8 apui00 = ZeldaIsMusicPlaying();
    if (apui00 != g_ram[kRam_APUI00]) {
      g_ram[kRam_APUI00] = apui00;
      EmuSyncMemoryRegion(&g_ram[kRam_APUI00], 1);
      StateRecorder_RecordPatchByte(&state_recorder, 0x648, &apui00, 1);
    }

    if (animated_tile_data_src != 0) {
      // Whenever we're no longer replaying, we'll remember what bugs were fixed,
      // but only if game is initialized.
      if (g_ram[kRam_BugsFixed] < kBugFix_Latest) {
        g_ram[kRam_BugsFixed] = kBugFix_Latest;
        EmuSyncMemoryRegion(&g_ram[kRam_BugsFixed], 1);
        StateRecorder_RecordPatchByte(&state_recorder, kRam_BugsFixed, &g_ram[kRam_BugsFixed], 1);
      }

      if (enhanced_features0 != g_wanted_zelda_features) {
        enhanced_features0 = g_wanted_zelda_features;
        EmuSyncMemoryRegion(&enhanced_features0, sizeof(enhanced_features0));
        StateRecorder_RecordPatchByte(&state_recorder, kRam_Features0, (uint8 *)&enhanced_features0, 4);
      }
    }
  }

  int run_what;
  if (g_ram[kRam_BugsFixed] < kBugFix_PolyRenderer) {
    // A previous version of this code alternated the game loop with
    // the poly renderer.
    run_what = (is_nmi_thread_active && thread_other_stack != 0x1f31) ? 2 : 1;
  } else {
    // The snes seems to let poly rendering run for a little
    // while each fram until it eventually completes a frame.
    // Simulate this by rendering the poly every n:th frame.
    run_what = (is_nmi_thread_active && IncrementCrystalCountdown(&g_ram[kRam_CrystalRotateCounter], virq_trigger)) ? 3 : 1;
    EmuSyncMemoryRegion(&g_ram[kRam_CrystalRotateCounter], 1);
  }

  if (g_emu_runframe == NULL || enhanced_features0 != 0 || g_zenv.dialogue_flags) {
    // can't compare against real impl when running with extra features.
    ZeldaRunFrameInternal(inputs, run_what);
  } else {
    g_emu_runframe(inputs, run_what);
  }

  ZeldaPushApuState();
  ZeldaUpdateCamera();
  HdWorld_TickNeighbors();  // mondo continuo: nemici delle aree vicine

  return is_replay;
}

void ZeldaSetLanguage(const char *language) {
  static const uint8 kDefaultConf[3] = { 0, 0, 0 };
  MemBlk found = { kDefaultConf, 3 };
  if (language) {
    size_t n = strlen(language);
    for (int i = 0; ; i++) {
      MemBlk mb = kDialogueMap(i);
      if (mb.ptr == 0) {
        fprintf(stderr, "Unable to find language '%s'\n", language);
        break;
      }
      MemBlk name = FindIndexInMemblk(mb, 0);
      if (name.size == n && !memcmp(name.ptr, language, n)) {
        found = FindIndexInMemblk(mb, 1);
        break;
      }
    }
  }
  g_zenv.dialogue_blk = kDialogue(found.ptr[0]);
  g_zenv.dialogue_font_blk = kDialogueFont(found.ptr[1]);
  g_zenv.dialogue_flags = found.ptr[2];
}


static const char *const kReferenceSaves[] = {
  "Chapter 1 - Zelda's Rescue.sav",
  "Chapter 2 - After Eastern Palace.sav",
  "Chapter 3 - After Desert Palace.sav",
  "Chapter 4 - After Tower of Hera.sav",
  "Chapter 5 - After Hyrule Castle Tower.sav",
  "Chapter 6 - After Dark Palace.sav",
  "Chapter 7 - After Swamp Palace.sav",
  "Chapter 8 - After Skull Woods.sav",
  "Chapter 9 - After Gargoyle's Domain.sav",
  "Chapter 10 - After Ice Palace.sav",
  "Chapter 11 - After Misery Mire.sav",
  "Chapter 12 - After Turtle Rock.sav",
  "Chapter 13 - After Ganon's Tower.sav",
};

void SaveLoadSlot(int cmd, int which) {
  char name[128];
  if (which & 256) {
    if (cmd == kSaveLoad_Save)
      return;
    sprintf(name, "saves/ref/%s", kReferenceSaves[which - 256]);
  } else {
    sprintf(name, "saves/save%d.sav", which);
  }
  FILE *f = fopen(name, cmd != kSaveLoad_Save ? "rb" : "wb");
  if (f) {
    printf("*** %s slot %d\n",
      cmd == kSaveLoad_Save ? "Saving" : cmd == kSaveLoad_Load ? "Loading" : "Replaying", which);

    if (cmd != kSaveLoad_Save)
      StateRecorder_Load(&state_recorder, f, cmd == kSaveLoad_Replay);
    else
      StateRecorder_Save(&state_recorder, f);

    fclose(f);
  }
}

// Salva o carica uno stato su un file con nome arbitrario (salvataggi automatici).
bool SaveLoadNamed(int cmd, const char *name) {
  FILE *f = fopen(name, cmd != kSaveLoad_Save ? "rb" : "wb");
  if (!f)
    return false;
  if (cmd != kSaveLoad_Save)
    StateRecorder_Load(&state_recorder, f, cmd == kSaveLoad_Replay);
  else
    StateRecorder_Save(&state_recorder, f);
  fclose(f);
  return true;
}

typedef struct StateRecoderMultiPatch {
  uint32 count;
  uint32 addr;
  uint8 vals[256];
} StateRecoderMultiPatch;


void StateRecoderMultiPatch_Init(StateRecoderMultiPatch *mp) {
  mp->count = mp->addr = 0;
}

void StateRecoderMultiPatch_Commit(StateRecoderMultiPatch *mp) {
  if (mp->count)
    StateRecorder_RecordPatchByte(&state_recorder, mp->addr, mp->vals, mp->count);
}

void StateRecoderMultiPatch_Patch(StateRecoderMultiPatch *mp, uint32 addr, uint8 value) {
  if (mp->count >= 256 || addr != mp->addr + mp->count) {
    StateRecoderMultiPatch_Commit(mp);
    mp->addr = addr;
    mp->count = 0;
  }
  mp->vals[mp->count++] = value;
  g_ram[addr] = value;
  EmuSyncMemoryRegion(&g_ram[addr], 1);
}

void PatchCommand(char c) {
  StateRecoderMultiPatch mp;

  StateRecoderMultiPatch_Init(&mp);
  if (c == 'w') {
    StateRecoderMultiPatch_Patch(&mp, 0xf372, 80);  // health filler
    StateRecoderMultiPatch_Patch(&mp, 0xf373, 80);  // magic filler
    //    b.Patch(0x1FE01, 25);
  } else if (c == 'W') {
    StateRecoderMultiPatch_Patch(&mp, 0xf375, 10);  // link_bomb_filler
    StateRecoderMultiPatch_Patch(&mp, 0xf376, 10);  // link_arrow_filler
    uint16 rupees = link_rupees_goal + 100;
    StateRecoderMultiPatch_Patch(&mp, 0xf360, rupees);  // link_rupees_goal
    StateRecoderMultiPatch_Patch(&mp, 0xf361, rupees >> 8);  // link_rupees_goal
  } else if (c == 'k') {
    StateRecorder_ClearKeyLog(&state_recorder);
  } else if (c == 'o') {
    StateRecoderMultiPatch_Patch(&mp, 0xf36f, 1);
  } else if (c == 'l') {
    StateRecorder_StopReplay(&state_recorder);
  } else if (c == 'E') {
    StateRecoderMultiPatch_Patch(&mp, 0x37f, g_ram[0x37f] ^ 1);
  }
  StateRecoderMultiPatch_Commit(&mp);
}


void ZeldaReadSram() {
  FILE *f = fopen("saves/sram.dat", "rb");
  if (f) {
    if (fread(g_zenv.sram, 1, 8192, f) != 8192)
      fprintf(stderr, "Error reading saves/sram.dat\n");
    fclose(f);
    EmuSynchronizeWholeState();
  }
}

void ZeldaWriteSram() {
  rename("saves/sram.dat", "saves/sram.bak");
  FILE *f = fopen("saves/sram.dat", "wb");
  if (f) {
    fwrite(g_zenv.sram, 1, 8192, f);
    fclose(f);
  } else {
    fprintf(stderr, "Unable to write saves/sram.dat\n");
  }
}