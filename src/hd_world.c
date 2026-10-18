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
//   video contiene un misto delle due, tutto il fondale viene dalle immagini.
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "hd_world.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "features.h"
#include "assets.h"
#include "overworld.h"
#include "load_gfx.h"
#include "snes/ppu.h"

uint8 Overworld_AreaHead(int square);

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

// Ritorno dalla mappa del mondo: il gioco ricarica le sovrapposizioni e
// schiarisce lo schermo (sottomoduli 32-34) prima di ridare il controllo
static bool InMapReturn(void) {
  return main_module_index == 9 && submodule_index >= 32 && submodule_index <= 34;
}

bool HdWorld_Active(void) {
  if (!FeatureOn() || overworld_screen_index >= 0x80)
    return false;
  if (main_module_index != 9 && !InMenuOverWorld() && !InFluteArrival())
    return false;
  return g_zenv.ppu->mode != 7;
}

// Luminosita' dello schermo (registro INIDISP, 0-15): il gioco la usa per le
// dissolvenze, per esempio all'arrivo con l'Ocarina. La PPU la applica ai
// suoi pixel; a quelli presi dalle immagini delle aree si applica qui.
static int g_bright = 15;

static inline uint32 Dim(uint32 c) {
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
  return main_module_index == 9 && submodule_index >= 13 && submodule_index <= 22;
}

bool HdWorld_BackgroundFromImages(void) {
  return HdWorld_Active() && (InTransition() || InMosaicTransition());
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
//   si vedono, fermi, anche oltre il bordo dell'area.
// Entrando in una casa o in un labirinto, e caricando un salvataggio, tutto
// il mondo esterno torna com'era.

enum { kMaxExtra = 1024 };  // erba tagliata e buche della pala
enum { kMaxTiles = 256, kMaxKilled = 128, kMaxAlive = 16, kEnemyResetGap = 2 };  // 2 quadrati in mezzo = 3 aree
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
};
enum { kRecSize = 64 };

typedef struct EnemyRec {
  uint16 blk;
  int x, y, z;
  uint8 data[kRecSize];
} EnemyRec;

typedef struct AreaState {
  bool used;
  uint32 version;  // cambia solo con le caselle (l'immagine va ridisegnata)
  int n_tiles, n_killed, n_alive;
  uint16 tile_pos[kMaxTiles], tile_val[kMaxTiles];
  // erba alta tagliata e buche della pala: il gioco non le mette nella sua
  // lista (ha posto per 256 caselle), quindi si ricordano a parte
  int n_extra;
  uint16 extra_pos[kMaxExtra], extra_val[kMaxExtra];
  uint16 killed[kMaxKilled];
  EnemyRec alive[kMaxAlive];
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

static bool PersistOn(void) {
  return FeatureOn() && BYTE(overworld_screen_index) < 0x80;
}

static void ForgetEnemies(int i) {
  g_states[i].n_killed = g_states[i].n_alive = 0;
  for (int j = 0; j < kMaxAlive; j++)
    g_patches[i][j].valid = false;
}

void HdWorld_ResetStates(void) {
  if (!g_any_state)
    return;
  memset(g_states, 0, sizeof(g_states));
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
  // nemici vivi
  st->n_alive = 0;
  for (int k = 0; k < 16 && st->n_alive < kMaxAlive; k++) {
    if (!IsAreaEnemy(k))
      continue;
    EnemyRec *r = &st->alive[st->n_alive++];
    r->blk = sprite_N_word[k];
    r->x = SpriteWorldX(k), r->y = SpriteWorldY(k), r->z = sprite_z[k];
    uint8 *d = r->data;
    for (int f = 0; f < countof(kSpriteFields); f++) {
      const SpriteField *sf = &kSpriteFields[f];
      memcpy(d, g_ram + sf->addr + k * sf->size, sf->size);
      d += sf->size;
    }
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
    bool active = alive;
    for (int k = 0; k < 16 && !active; k++)
      active = sprite_state[k] && sprite_N_word[k] == blk;
    if (!active)
      st->killed[st->n_killed++] = blk;
  }
  st->used = true;
  g_any_state = true;
  if (changed)
    st->version++;
}

void HdWorld_RememberExtraTile(uint16 pos, uint16 value) {
  if (!PersistOn())
    return;
  AreaState *st = &g_states[BYTE(overworld_screen_index) & 0x7f];
  int i = 0;
  while (i < st->n_extra && st->extra_pos[i] != pos)
    i++;
  if (i == kMaxExtra)
    return;
  if (i == st->n_extra)
    st->n_extra++;
  st->extra_pos[i] = pos, st->extra_val[i] = value;
  st->used = true;
  st->version++;
  g_any_state = true;
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
    if (g_states[i].used && i != (a & 0x7f) && AreaGap(i, a) >= kEnemyResetGap)
      ForgetEnemies(i);
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
    const uint8 *d = r->data;
    for (int f = 0; f < countof(kSpriteFields); f++) {
      const SpriteField *sf = &kSpriteFields[f];
      memcpy(g_ram + sf->addr + k * sf->size, d, sf->size);
      d += sf->size;
    }
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
  int lx = link_x_coord, ly = link_y_coord;
  for (int k = 0; k < 16; k++) {
    if (!IsAreaEnemy(k))
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
        // niente Link ne' altri nemici che passano davanti
        if (mine && px >= lx - 4 && px < lx + 20 && py >= ly - 8 && py < ly + 24)
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

// Nemici delle aree vicine, fermi come l'ultima volta che si sono visti
static void DrawFrozenEnemies(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y) {
  int cur = BYTE(overworld_screen_index) & 0x7f;
  for (int a = 0; a < 128; a++) {
    AreaState *st = &g_states[a];
    // l'area appena raggiunta si salta, tranne nel primo frame del passaggio,
    // quando il gioco non ha ancora caricato i suoi nemici (sparivano per un
    // frame)
    if (!st->used || (a == cur && !InTransition()) || ((a ^ cur) & 0x40) || !st->n_alive)
      continue;
    for (int i = 0; i < st->n_alive; i++) {
      EnemyRec *r = &st->alive[i];
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
static bool g_vis_glide;           // uscita da una casa: la vista scivola al suo posto

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

void HdWorld_UpdateCamera(int *cam_x, int *cam_y) {
  int gx = BG2HOFS_copy2, gy = BG2VOFS_copy2;
  // titolo, selezione file, caricamento, e dentro case e labirinti (6, 7):
  // il mondo esterno torna com'era
  if (main_module_index <= 7)
    HdWorld_ResetStates();
  else if (main_module_index == 9 && submodule_index == 0 && PersistOn())
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
  // fase 10: Link esce dalla porta dopo il cerchio che si apre
  int mode = !HdWorld_Active() ? 0 : InFluteArrival() ? 1 : main_module_index != 9 ? 0 :
             submodule_index == 0 || submodule_index == 10 || InMapReturn() ? 1 : InTransition() ? 2 :
             InMosaicTransition() ? 3 : 0;
  int prev_mode = g_prev_mode;
  g_prev_mode = mode;
  if (mode == 0) {
    g_vis_valid = false;
    g_world_dx = g_world_dy = 0;
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
    if (prev_mode == 2 && g_vis_valid) {
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
    } else if (!g_vis_valid) {
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

enum { kMaxImages = 16 };

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
void HdWorld_ClampView(int *cam_x, int *cam_y) {
  if (!HdWorld_Active())
    return;
  Ppu *ppu = g_zenv.ppu;
  int gx = BG2HOFS_copy2, gy = BG2VOFS_copy2;
  int ext = ppu->extraLeftRight;
  // righe dello schermo (240 con extend_y); la riga 0 dello schermo e' la
  // riga 1 del fondale (HdWorld_Composite)
  int rows = (ppu->renderFlags & kPpuRenderFlags_Height240) ? 240 : 224;
  int min_x = ext, max_x = 4096 - 256 - ext;
  int min_y = 0, max_y = kWorldBottom - 1 - rows;
  if (gy < min_y) min_y = gy;
  if (gy - (rows - 224) > max_y) max_y = gy - (rows - 224);
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
  // sulla montagna le nuvole (palette 3) non hanno pennellate, come l'acqua (hd_fx.c)
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
        bool smooth = water || (mountain && pal == 3);
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
              ac->color[v] = Bgr15ToPixel(main_palette_buffer[pal * 16 + v], smooth ? 7 : 1);
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
            dst[col] = v ? Bgr15ToPixel(main_palette_buffer[pal * 16 + v], smooth ? 7 : 1) :
                       water && !backdrop_scene ? (backdrop & 0xffffff) | 7u << 24 : backdrop;
          }
        }
      }
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
  return Overworld_AreaHead((wy >> 9) * 8 + (wx >> 9)) | (overworld_screen_index & 0x40);
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

static void OverlayBegin(OverlayCtx *o, int ext) {
  Ppu *ppu = g_zenv.ppu;
  bool sub = (ppu->screenEnabled[1] & 1) && !(ppu->screenEnabled[0] & 1) && ppu->addSubscreen &&
             !ppu->subtractColor && ppu->mode != 7;
  o->on = sub && (ppu->mathEnabled & 2) && ppu->halfColor;
  o->sky = sub && !o->on && (ppu->mathEnabled & 0x20);
  o->half = ppu->halfColor;
  o->x0 = ppu->bgLayer[0].hScroll + ppu->bgBiasX - ext;
  o->y0 = ppu->bgLayer[0].vScroll + ppu->bgBiasY + 1;
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
  return (c & 0xff000000) | (((c >> 16 & 255) + r) >> 1) << 16 | (((c >> 8 & 255) + g) >> 1) << 8 | ((c & 255) + b) >> 1;
}

// Ridisegna le caselle animate delle immagini visibili con il fotogramma
// d'animazione attuale (stessa memoria video per tutte le aree dello stesso
// mondo), dove il fondale viene dalle immagini
static void DrawAnimatedCells(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y, bool all,
                              const OverlayCtx *ov) {
  Ppu *ppu = g_zenv.ppu;
  uint16 chr_base = ppu->bgLayer[1].tileAdr;
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
      if (ac->y + 8 > img->size - kHiddenRows)
        continue;  // ultime righe nascoste: lasciate come sono
      // celle allineate a 8 pixel e aree a 512: questi controlli valgono per
      // tutta la cella
      int cwx = img->x + ac->x, cwy = img->y + ac->y;
      if (!all && HdWorld_InsideCurrentArea(cwx, cwy))
        continue;  // qui disegna gia' il gioco
      if (HeadAt(cwx, cwy) != (img->key & 0x7f))
        continue;  // coperta da un'altra area (o fuori dalla mappa)
      int e = ac->entry, tile = e & 0x3ff;
      bool hf = (e & 0x4000) != 0, vf = (e & 0x8000) != 0;
      const uint16 *t = &ppu->vram[(chr_base + tile * 16) & 0x7fff];
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

// Montagna della Morte (chiara e oscura): e' molto piu' in alto delle aree
// vicine, che nel gioco non si vedono mai insieme ad essa
static bool IsMountainArea(int head) {
  int a = head & 0x3f;
  return a == 0x03 || a == 0x05 || a == 0x07;
}

// Nebbia sui confini fra la montagna e la pianura: l'area dell'altro livello
// sfuma in una foschia allontanandosi dal bordo dell'area in cui sta Link,
// cosi' si capisce che in mezzo c'e' un dislivello (vale nei due versi)
static void DrawHeightFog(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y) {
  enum { kFogDist = 128, kFogInside = 5 };  // lunghezza della sfumatura, in pixel
  const uint32 kFog = Dim(0xc4ccd4);
  const float kFogStart = 0.6f;  // velatura sul confine
  bool cur_high = IsMountainArea(overworld_screen_index);
  int ax, ay, as;
  AreaRect(overworld_screen_index, &ax, &ay, &as);
  // Dentro l'area la nebbia comincia gia' 5 pixel prima dei lati che
  // confinano con l'altro livello. Il vicino si guarda proprio di fronte al
  // pixel: lungo il lato di un'area grande ci possono essere aree di tutti e
  // due i livelli. Sopra e sotto: una colonna per volta (in blocchi da 8).
  enum { kMaxFogCols = 1024 / 8 + 2 };
  bool col_up[kMaxFogCols], col_down[kMaxFogCols];
  int ncols = (width + 7) / 8 + 1;
  if (ncols > kMaxFogCols) ncols = kMaxFogCols;
  for (int i = 0; i < ncols; i++) {
    int wx = ((cam_x >> 3) + i) << 3;
    int hu = HeadAt(wx, ay - 1), hd = HeadAt(wx, ay + as);
    col_up[i] = hu >= 0 && IsMountainArea(hu) != cur_high;
    col_down[i] = hd >= 0 && IsMountainArea(hd) != cur_high;
  }
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    int wy = cam_y + y;
    int dy = wy < ay ? ay - wy : wy >= ay + as ? wy - (ay + as - 1) : 0;
    int last_head = -2;
    bool other = false;
    int hl = HeadAt(ax - 1, wy), hr = HeadAt(ax + as, wy);
    bool side_left = hl >= 0 && IsMountainArea(hl) != cur_high;
    bool side_right = hr >= 0 && IsMountainArea(hr) != cur_high;
    for (int x = 0; x < width; x++) {
      int wx = cam_x + x;
      int dx = wx < ax ? ax - wx : wx >= ax + as ? wx - (ax + as - 1) : 0;
      uint32 c = row[x], l = c >> 24;
      float f;
      if (!dx && !dy) {
        // dentro l'area corrente, vicino a un lato che da' sull'altro livello
        if (l == 2 || l == 4 || l == 6)
          continue;  // HUD e sprite restano nitidi
        int k = kFogInside, ci = (wx >> 3) - (cam_x >> 3);
        if (ci < 0) ci = 0;
        if (ci >= ncols) ci = ncols - 1;
        if (col_up[ci] && wy - ay < k) k = wy - ay;
        if (col_down[ci] && ay + as - 1 - wy < k) k = ay + as - 1 - wy;
        if (side_left && wx - ax < k) k = wx - ax;
        if (side_right && ax + as - 1 - wx < k) k = ax + as - 1 - wx;
        if (k >= kFogInside)
          continue;
        f = kFogStart * (kFogInside - k) / (kFogInside + 1);
      } else {
        int head = HeadAt(wx, wy);
        if (head != last_head)
          last_head = head, other = head >= 0 && IsMountainArea(head) != cur_high;
        if (!other || l == 2)
          continue;
        f = (float)(dx > dy ? dx : dy) / kFogDist;
        if (f > 1) f = 1;
        // gia' velata sul bordo (60%), poi sale con una curva morbida fino
        // alla nebbia piena: il taglio non sembra un pavimento che continua
        f = kFogStart + (1 - kFogStart) * f * f * (3 - 2 * f);
      }
      int r = (int)(((c >> 16) & 0xff) * (1 - f) + ((kFog >> 16) & 0xff) * f);
      int g = (int)(((c >> 8) & 0xff) * (1 - f) + ((kFog >> 8) & 0xff) * f);
      int b = (int)((c & 0xff) * (1 - f) + (kFog & 0xff) * f);
      // nella nebbia niente shader: fondale (layer 3) che non viene dipinto
      row[x] = 3u << 24 | r << 16 | g << 8 | b;
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
// Composizione del frame

void HdWorld_Composite(uint8 *pixels, int pitch, int width, int height) {
  g_renders_this_frame = 0;
  if (!HdWorld_Active())
    return;
  Ppu *ppu = g_zenv.ppu;
  g_bright = ppu->forcedBlank ? 0 : ppu->brightness;
  bool mosaic = InMosaicTransition();
  bool all = InTransition() || mosaic;
  // aspetto dei nemici, anche quando la vista coincide con quella del gioco
  if (main_module_index == 9 && submodule_index == 0 && PersistOn())
    CaptureEnemies(pixels, pitch, width, height);
  if (!all && g_world_dx == 0 && g_world_dy == 0 && ppu->extraLeftRight == 0) {
    PrefetchNeighbors();
    return;
  }
  int ext = (width - 256) / 2;
  int cam_x = (int16)BG2HOFS_copy + ppu->bgBiasX - ext;  // la PPU tiene solo 10 bit
  // +1: la PPU non mostra la sua riga 0, quindi la riga 0 dello schermo e' la
  // riga 1 del fondale
  int cam_y = (int16)BG2VOFS_copy + ppu->bgBiasY + 1;
  int ax, ay, as;
  AreaRect(overworld_screen_index, &ax, &ay, &as);
  // La PPU ha le caselle dell'area solo attorno alla telecamera del gioco
  // (con lo schermo largo, ext pixel in piu' per lato). Ai bordi sinistro e
  // destro del mondo la vista si ferma piu' all'interno (HdWorld_ClampView):
  // le colonne oltre quelle caricate vengono dall'immagine dell'area.
  int in_x0 = (int16)BG2HOFS_copy - ext, in_x1 = (int16)BG2HOFS_copy + 256 + ext;
  if (in_x0 < ax) in_x0 = ax;
  if (in_x1 > ax + as) in_x1 = ax + as;
  // area vicina non ancora disegnata (al massimo un frame): colore di fondo
  uint32 outside = Bgr15ToPixel(main_palette_buffer[0], 5);
  int last_head = -2;
  AreaImage *img = NULL;
  // Mosaico del gioco anche sulle immagini (blocchi allineati come nella PPU)
  int msz = mosaic && (ppu->mosaicEnabled & 3) && ppu->mosaicSize > 1 ? ppu->mosaicSize : 1;
  OverlayCtx ov;
  OverlayBegin(&ov, ext);
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
    if (PersistOn())
      DrawFrozenEnemies(pixels, pitch, width, height, cam_x, cam_y);
  }
  DrawHeightFog(pixels, pitch, width, height, cam_x, cam_y);
  if (mosaic)
    FadeLikeGame(pixels, pitch, width, height);
  PrefetchNeighbors();
}
