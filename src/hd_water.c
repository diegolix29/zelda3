// Acqua come in Four Swords Adventures (opzione FxWater). Lavora sull'immagine
// gia' disegnata, sui pixel d'acqua marcati con il layer 7 (HdFx_MarkWater):
//  1. profondita' e schiuma: il colore si schiarisce verso le rive e attorno a
//     sassi e pali, con una fascia di schiuma e increspature che si allontanano;
//  2. superficie viva: macchie chiare che scorrono piano;
//  3. cascate (caselle 0x1ca-0x1cd con la palette 7, piu' i bordi diagonali e
//     l'entrata nascosta): colonne che scorrono, bordo chiaro in cima (solo
//     dove c'e' la cima vera), schiuma e nebbia alla base;
//  4. vasche dei villaggi (caselle 0x126/0x127/0x136/0x137): per il gioco
//     non sono acqua, ma la loro acqua si disegna come le fontane.
// Le luci (illuminazione dinamica, ombre morbide, buio) restano: il colore
// nuovo si moltiplica per il rapporto fra il pixel illuminato e lo stesso
// pixel prima delle luci (HdWater_BeforeEffects ne fa una copia).
// I passi sui pixel girano su piu' core (hd_par.c), a strisce dell'immagine.
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "hd_water.h"
#include "hd_fx.h"
#include "hd_par.h"
#include "hd_light.h"
#include "hd_world.h"
#include "tile_detect.h"
#include "overworld.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "dungeon.h"
#include "snes/ppu.h"

bool g_fx_water;

enum {
  kLayerHud = 2,
  kLayerWater = 7,
  kDistUnit = 3,          // distanze in terzi di pixel (passi 3 e 4)
  kDistMax = 64 * 3,      // distanze oltre i 64 pixel non servono
  kBlurRadius = 7,        // media dei colori dell'acqua (toglie il disegno a righe dello SNES)
  kMaxMist = 320,
  kSumCh = 3,             // canali della media (g_sum), vedi BlurRows
};

static uint32 *g_pre;     // l'immagine prima di ombre e luci
static int g_pre_cap;
static bool g_pre_valid;
static uint16 *g_dist;
static uint8 *g_kind;     // 0 = altro, 1 = acqua, 2 = cascata
static uint8 *g_depth;    // 0 = non si sa, 1 = acqua alta (0x08), 2 = acqua bassa (0x09)
static uint8 *g_fixed;    // 1 = onda animata dello SNES: acqua in ogni fase (AnimWaterPixel)
static uint32 *g_sum;     // somme per la media, impacchettate (vedi BlurRows)
static uint32 *g_sum2;
static int g_buf_cap;

static uint8 g_flood[4096];
static int g_flood_room = -1;
static uint8 g_flood_bridge[4096];  // passerelle sopra l'acqua allagata
static bool g_flood_bridge_valid;

void HdWater_ClearFlood(void) {
  memset(g_flood, 0, sizeof(g_flood));
  g_flood_bridge_valid = false;
  g_flood_room = dungeon_room_index;
}

void HdWater_MarkFlood(int pos) {
  g_flood[pos & 0xfff] = 1;
}

// stanza della diga (anche prima di tirare la leva); mentre la chiusa si
// riempie l'acqua si vede solo dentro la finestra che scende (HdFx_MarkWater)
enum { kDamRoom = 0x10b };
static bool FloodRoom(void) {
  return player_is_indoors && dungeon_room_index == kDamRoom;
}

// Dungeon_LoadAttribute_Selectable (scale, passaggi fra stanze) ricarica la
// tabella a pezzi: nella fase 1 sono gia' nuovi solo i primi
// dung_draw_height_indicator, gli altri sono della stanza di prima; nelle fasi
// 1 e 2 mancano ancora scale e interruttori sopra le caselle (le scale della
// stanza della leva hanno sotto caselle d'acqua: un alone bianco). La tabella
// finita si calcola qui su una copia della RAM, che poi torna com'era
// Nello scorrimento fra due stanze si vedono la vecchia e la nuova, ma la
// tabella e' una: per la vecchia vale la copia presa quando era ferma
static uint8 g_attr_cache[0x2000], g_attr_prev[0x2000];
static bool g_attr_cached;
static int g_attr_prev_room = -1;

void HdWater_PrepareAttrs(void) {
  g_attr_cached = false;
  if (main_module_index != 7)
    return;
  int st = overworld_map_state;
  if (submodule_index == 0 && (st == 0 || st == 5)) {
    memcpy(g_attr_prev, dung_bg2_attr_table, sizeof(g_attr_prev));
    g_attr_prev_room = dungeon_room_index;
    return;
  }
  // (nella fase 0 la tabella e' gia' giusta: dopo lo specchio la stanza e'
  // caricata tutta, con l'acqua della chiusa che le caselle non hanno)
  if (st != 1 && st != 2)
    return;
  static uint8 save[sizeof(g_ram)];
  memcpy(save, g_ram, sizeof(g_ram));
  if (st == 1) {
    for (int i = dung_draw_height_indicator; i < 0x2000; i++) {
      uint16 e = dung_bg2[i];
      uint8 v = attributes_for_tile[e & 0x3ff];
      dung_bg2_attr_table[i] = v >= 0x10 && v < 0x1c ? v | e >> 14 : v;  // come il gioco
    }
  }
  Dungeon_LoadObjectAttribute();
  memcpy(g_attr_cache, dung_bg2_attr_table, sizeof(g_attr_cache));
  memcpy(g_ram, save, sizeof(g_ram));
  g_attr_cached = true;
}

uint8 HdWater_DungAttr(int i, int wx, int wy) {
  i &= 0x1fff;
  int room = (wy >> 9 & 15) << 4 | (wx >> 9 & 15);
  if (main_module_index == 7 && submodule_index != 0 && room == g_attr_prev_room && room != dungeon_room_index)
    return g_attr_prev[i];
  return g_attr_cached ? g_attr_cache[i] : dung_bg2_attr_table[i];
}

// Le case non sono labirinti (cur_palace_index_x2 0xff): nei labirinti le
// stesse caselle sono altre cose
bool HdWater_IsTubCell(int pos) {
  if ((cur_palace_index_x2 & 0xff) != 0xff)
    return false;
  uint16 e = dung_bg2[pos & 0xfff];
  int t = e & 0x3ff;
  return ((e >> 10) & 7) == 4 && ((t >= 0x1a0 && t <= 0x1a3) || t == 0x194 || t == 0x195);
}

// Interno della vasca: viola-blu (695cdb, bordo scuro 382ab1); il bordo
// marrone della vasca resta com'e' e fa da riva
bool HdWater_IsTubColor(uint32 c) {
  int r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
  return b >= r + 50 && b >= g + 50;
}

// Dallo stato del gioco (bit 0x800 della stanza: leva tirata), cosi' vale
// anche caricando uno stato salvato: le caselle si ricalcolano se mancano
bool HdWater_IsFlood(int pos) {
  if (!FloodRoom() || !(dung_savegame_state_bits & 0x800))
    return false;
  if (g_flood_room != kDamRoom)
    Dungeon_MarkWaterFlooring();
  return g_flood[pos & 0xfff];
}

// Passerelle sopra l'acqua della diga: caselle non allagate fra due caselle
// allagate della stessa riga (o colonna), al massimo 12, che cominciano e
// finiscono con la ringhiera (attributo 0x02). L'acqua passa sotto: niente riva
static void FindFloodBridges(void) {
  memset(g_flood_bridge, 0, sizeof(g_flood_bridge));
  for (int pass = 0; pass < 2; pass++) {
    for (int a = 0; a < 64; a++) {
      for (int b = 1; b < 63; b++) {
        int p0 = pass ? b * 64 + a : a * 64 + b, step = pass ? 64 : 1;
        if (g_flood[p0] || !g_flood[p0 - step] || dung_bg2_attr_table[p0] != 0x02)
          continue;
        int e = b;
        while (e < 64 && !g_flood[pass ? e * 64 + a : a * 64 + e])
          e++;
        int pl = pass ? (e - 1) * 64 + a : a * 64 + e - 1;
        if (e < 64 && e - b <= 12 && dung_bg2_attr_table[pl] == 0x02)
          for (int i = b; i < e; i++)
            g_flood_bridge[pass ? i * 64 + a : a * 64 + i] = 1;
        b = e;
      }
    }
  }
  g_flood_bridge_valid = true;
}

static bool FloodBridgeAt(int wx, int wy) {
  int pos = ((wy & 0x1f8) << 3) + ((wx & 0x1f8) >> 3);
  if (!FloodRoom() || !(dung_savegame_state_bits & 0x800))
    return false;
  HdWater_IsFlood(pos);  // ricalcola le caselle allagate se mancano
  if (!g_flood_bridge_valid)
    FindFloodBridges();
  return g_flood_bridge[pos];
}

static inline float Clamp01(float v) {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

static inline float Smooth(float a, float b, float v) {
  float t = Clamp01((v - a) / (b - a));
  return t * t * (3 - 2 * t);
}

static inline uint32 Hash2(int x, int y) {
  uint32 h = (uint32)x * 374761393u + (uint32)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

static inline float Rnd(int x, int y) {
  return (Hash2(x, y) & 0xffff) * (1.0f / 65535.0f);
}

// Rumore a valori, morbido, fra 0 e 1
static float Noise(float x, float y) {
  float fx = floorf(x), fy = floorf(y);
  int ix = (int)fx, iy = (int)fy;
  float tx = x - fx, ty = y - fy;
  tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
  float a = Rnd(ix, iy), b = Rnd(ix + 1, iy), c = Rnd(ix, iy + 1), d = Rnd(ix + 1, iy + 1);
  return a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty;
}

static float Rand01(void) {
  static uint32 s = 0x9e3779b9u;
  s ^= s << 13, s ^= s >> 17, s ^= s << 5;
  return (s & 0xffffff) * (1.0f / 16777216.0f);
}

static bool InScene(int *mod_out) {
  if (!g_fx_water)
    return false;
  int mod = HdFx_SceneModule();
  *mod_out = mod;
  return mod == 7 || mod == 9 || mod == 11;
}

static bool EnsureBuffers(int n) {
  if (n <= g_buf_cap)
    return true;
  uint32 *pre = realloc(g_pre, n * sizeof(uint32));
  if (pre) g_pre = pre;
  uint16 *dist = realloc(g_dist, n * sizeof(uint16));
  if (dist) g_dist = dist;
  uint8 *kind = realloc(g_kind, n);
  if (kind) g_kind = kind;
  uint8 *depth = realloc(g_depth, n);
  if (depth) g_depth = depth;
  uint8 *fixed = realloc(g_fixed, n);
  if (fixed) g_fixed = fixed;
  uint32 *sum = realloc(g_sum, n * kSumCh * sizeof(uint32));
  if (sum) g_sum = sum;
  uint32 *sum2 = realloc(g_sum2, n * kSumCh * sizeof(uint32));
  if (sum2) g_sum2 = sum2;
  if (!pre || !dist || !kind || !depth || !fixed || !sum || !sum2)
    return false;
  g_buf_cap = n;
  return true;
}

void HdWater_BeforeEffects(uint8 *pixels, int pitch, int width, int height) {
  int mod;
  g_pre_valid = false;
  if (!InScene(&mod) || !EnsureBuffers(width * height))
    return;
  for (int y = 0; y < height; y++)
    memcpy(g_pre + y * width, pixels + y * pitch, width * sizeof(uint32));
  g_pre_valid = true;
}

// ---------------------------------------------------------------------------
// Caselle delle cascate

// Casella 8x8 (numero, palette, ribaltamenti) del terreno nel punto del mondo,
// anche fuori dall'area corrente col mondo continuo; -1 se non si sa
static int CellEntry(int wx, int wy) {
  if (wx < 0 || wy < 0)
    return -1;
  if (HdWorld_Active() && (HdWorld_BackgroundFromImages() || !HdWorld_InsideCurrentArea(wx, wy))) {
    int m16 = HdWorld_WorldTile(wx, wy);
    if (m16 >= 0)
      return GetMap16toMap8Table()[m16 * 4 + ((wy >> 3) & 1) * 2 + ((wx >> 3) & 1)];
    if (!HdWorld_InsideCurrentArea(wx, wy))
      return -1;
  }
  return Overworld_GetMap8AtLocation((uint16)wx >> 3, (uint16)wy);
}

// Le cascate dell'esterno (Cascata dei Desideri, Dominio di Zora e le altre):
// bordo in cima 0x1ca-0x1cb, corpo 0x1cc, fondo 0x1cd, sempre con la palette 7
static bool IsWaterfallEntry(int e) {
  int t = e & 0x3ff;
  return e >= 0 && (e >> 10 & 7) == 7 && t >= 0x1ca && t <= 0x1cd;
}

// Bordi diagonali delle cascate (palette 7): mezza casella e' cascata, il
// resto roccia. Nel Dominio di Zora e alla Cascata dei Desideri
static bool IsWaterfallEdgeTile(int t) {
  return t == 0x1c8 || t == 0x1e5 || t == 0x1e6 || (t >= 0x1f3 && t <= 0x1f6);
}

// caselle esaminate anche oltre i bordi dello schermo (kCellMargin per lato):
// un ponte si riconosce dall'acqua ai due lati, che puo' essere fuori schermo
enum { kCellMargin = 20, kMaxCellsX = 72 + 2 * kCellMargin, kMaxCellsY = 34 + 2 * kCellMargin };
enum { kCellOther, kCellFall, kCellWater, kCellUnknown, kCellShallow, kCellBridge, kCellSky,
       kCellFallEdge, kCellTrough, kCellWarp };
static uint8 g_cells[kMaxCellsY][kMaxCellsX];
// casella animata (onde sulla riva, 32 caselle dalla kAnimFirst): numero da 0,
// bit 8 ribaltata in orizzontale, bit 9 in verticale; -1 se non lo e'
static int16 g_cell_anim[kMaxCellsY][kMaxCellsX];
// Mondo Oscuro: ogni casella animata (anche bordi e diagonali della riva,
// che non hanno l'attributo dell'acqua), -1 se no (MarkDarkShoreCells)
static int16 g_cell_shore[kMaxCellsY][kMaxCellsX];
static uint8 g_cell_attr[kMaxCellsY][kMaxCellsX];
static int16 g_cell_entry[kMaxCellsY][kMaxCellsX];  // casella 8x8 senza ribaltamenti, -1 se non si sa
static bool g_cell_lip[kMaxCellsY][kMaxCellsX];     // cima vera di una cascata

static inline bool IsWaterCell(int v) {
  return v == kCellWater || v == kCellShallow;
}

// Ponti sull'acqua (quello del castello, quello del lago Hylia...): l'acqua
// passa sotto, quindi i loro lati non sono una riva e non fanno schiuma. Un
// ponte e' una fila di celle non d'acqua, larga da 4 a 16 celle, con acqua ai
// due capi, le ringhiere (attributo 0x02) come prima e ultima cella e in
// mezzo il piano su cui si cammina (0x00); e i suoi lati sono dritti e
// paralleli: la stessa fila si ripete per almeno 12 celle di lunghezza,
// contando anche dove le ringhiere (le stesse caselle) continuano sulla terra
// (il ponte del fiume a est del castello passa su sole 7 celle d'acqua).
// Isole (anche quelle con i bordi 0x02, lago Hylia) e sassi non sono cosi'
enum { kBridgeMinLength = 12 };
static uint16 g_bridge_tag[kMaxCellsY][kMaxCellsX];

static void FindBridges(void) {
  for (int pass = 0; pass < 2; pass++) {
    int n = pass ? kMaxCellsX : kMaxCellsY, len = pass ? kMaxCellsY : kMaxCellsX;
#define CELL(a, i) (pass ? g_cells[i][a] : g_cells[a][i])
#define ATTR(a, i) (pass ? g_cell_attr[i][a] : g_cell_attr[a][i])
#define TAG(a, i) (*(pass ? &g_bridge_tag[i][a] : &g_bridge_tag[a][i]))
#define ENT(a, i) (pass ? g_cell_entry[i][a] : g_cell_entry[a][i])
    memset(g_bridge_tag, 0, sizeof(g_bridge_tag));
    // file candidate: TAG sulla prima cella = inizio e fine della fila
    for (int a = 0; a < n; a++) {
      for (int b = 1; b < len - 1; b++) {
        if (!IsWaterCell(CELL(a, b - 1)) || IsWaterCell(CELL(a, b)) || ATTR(a, b) != 0x02)
          continue;
        int rail = 0;
        while (b + rail < len && ATTR(a, b + rail) == 0x02)
          rail++;
        int e = b, deck = 0;
        while (e < len && !IsWaterCell(CELL(a, e)) && CELL(a, e) != kCellUnknown)
          deck += ATTR(a, e) == 0x00, e++;
        if (e < len && IsWaterCell(CELL(a, e)) && e - b >= 4 && deck >= 2 && ATTR(a, e - 1) == 0x02) {
          // il ponte finisce con la ringhiera dopo il piano, larga come quella
          // d'inizio: un sasso attaccato alla ringhiera (castello) resta un
          // sasso, con la sua schiuma
          int last = e - 1;
          while (ATTR(a, last) != 0x00)
            last--;
          int end = last + 1;
          while (end < e && end <= last + rail && ATTR(a, end) == 0x02)
            end++;
          if (end - b <= 16)
            TAG(a, b) = (uint16)(end << 8 | b) + 1;
        }
        b = e - 1;
      }
    }
    // ponte solo dove la stessa fila (stesso inizio e stessa fine) si ripete
    for (int a = 0; a < n; a++) {
      for (int b = 0; b < len; b++) {
        uint16 tag = TAG(a, b);
        if (!tag)
          continue;
        int lo = a, hi = a;
        while (lo > 0 && TAG(lo - 1, b) == tag) lo--;
        while (hi < n - 1 && TAG(hi + 1, b) == tag) hi++;
        // le ringhiere che continuano sulla terra (stesse caselle ai due lati)
        int end = (tag - 1) >> 8;
        int16 e0 = ENT(a, b), e1 = ENT(a, end - 1);
        while (lo > 0 && e0 >= 0 && ENT(lo - 1, b) == e0 && ENT(lo - 1, end - 1) == e1)
          lo--;
        while (hi < n - 1 && e0 >= 0 && ENT(hi + 1, b) == e0 && ENT(hi + 1, end - 1) == e1)
          hi++;
        if (hi - lo + 1 < kBridgeMinLength)
          continue;
        for (int r = lo; r <= hi; r++)
          for (int i = b; i < end; i++)
            if (!IsWaterCell(CELL(r, i)))
              if (pass) g_cells[i][r] = kCellBridge; else g_cells[r][i] = kCellBridge;
      }
    }
#undef CELL
#undef ATTR
#undef TAG
#undef ENT
  }
}

// Sul Monte della Morte (chiaro e oscuro) il vuoto
// oltre i dirupi e' "acqua profonda" per il gioco, ma sullo schermo e' il
// cielo con le nuvole (marcate anche loro come layer 7): li' l'acqua non si
// tocca. Vale anche per le aree vicine viste col mondo continuo
static bool IsSkyPoint(int wx, int wy) {
  int col = wx >> 9, row = wy >> 9;
  if ((row == 0 && col >= 3) || (row == 1 && col >= 3 && col <= 6))
    return true;
  // la Piramide (Mondo Oscuro, 0x5b) non e' cielo: il vuoto attorno alla cima
  // e' il colore di fondo, che HdFx_MarkWater non segna come acqua, e il
  // fossato sotto e' acqua vera (prima restava com'era, senza acqua HD)
  return false;
}

// Vero se c'e' una cella di cascata (cima, corpo o fondo) nella stessa fila,
// a sinistra (dir -1) o a destra (dir 1), a non piu' di due celle
static bool FallBeside(int cy, int cx, int dir) {
  for (int i = 1; i <= 2; i++) {
    int x = cx + dir * i;
    if (x >= 0 && x < kMaxCellsX && g_cells[cy][x] == kCellFall)
      return true;
  }
  return false;
}

static bool LipBeside(int cy, int cx) {
  for (int i = -2; i <= 2; i++) {
    int x = cx + i;
    if (i && x >= 0 && x < kMaxCellsX && g_cells[cy][x] == kCellFall && g_cell_lip[cy][x])
      return true;
  }
  return false;
}

// Le parti delle cascate che non sono le caselle 0x1ca-0x1cd:
// - l'acqua con la palette 7 in mezzo a una cascata (l'entrata nascosta della
//   Cascata dei Desideri, caselle 0x1c6/0x1c7) e' cascata: deve sembrare una
//   cascata come le altre;
// - i bordi diagonali accanto alla cascata: i loro pixel blu sono cascata (il
//   resto e' roccia), con la cima se sono accanto alla cima
static void FindFallParts(void) {
  for (int cy = 0; cy < kMaxCellsY; cy++) {
    for (int cx = 0; cx < kMaxCellsX; cx++) {
      int e = g_cell_entry[cy][cx], v = g_cells[cy][cx];
      if (e < 0 || (e >> 10 & 7) != 7)
        continue;
      // (0x1c6/0x1c7 sono anche onde animate della riva: qui non lo sono,
      // se no l'entrata si vedeva come acqua in mezzo alla cascata)
      if (IsWaterCell(v) && FallBeside(cy, cx, -1) && FallBeside(cy, cx, 1))
        g_cells[cy][cx] = kCellFall, g_cell_anim[cy][cx] = -1;
    }
    for (int cx = 0; cx < kMaxCellsX; cx++) {
      int e = g_cell_entry[cy][cx];
      // 0x1c8, accanto alla cima, per il gioco e' acqua (0x08)
      int v = g_cells[cy][cx];
      if (e < 0 || (e >> 10 & 7) != 7 || (v != kCellOther && !IsWaterCell(v)) || !IsWaterfallEdgeTile(e & 0x3ff))
        continue;
      if (FallBeside(cy, cx, -1) || FallBeside(cy, cx, 1)) {
        g_cells[cy][cx] = kCellFallEdge;
        g_cell_lip[cy][cx] = LipBeside(cy, cx);
        g_cell_anim[cy][cx] = -1;  // 0x1c8 non e' un'onda: angoli bianchi in cima
      }
    }
  }
}

// Vasche dei villaggi: 2 file di caselle con la palette 7, 0x126/0x127 sopra
// e 0x136/0x137 sotto (la stessa casella + 0x10). Dentro i pixel azzurri
// sono acqua
static void FindTroughs(void) {
  for (int cy = 0; cy < kMaxCellsY - 1; cy++) {
    for (int cx = 0; cx < kMaxCellsX; cx++) {
      int e = g_cell_entry[cy][cx], b = g_cell_entry[cy + 1][cx];
      if (e < 0 || b < 0 || (e >> 10 & 7) != 7 || (b >> 10 & 7) != 7)
        continue;
      int t = e & 0x3ff;
      if ((t == 0x126 || t == 0x127) && (b & 0x3ff) == t + 0x10 && g_cells[cy][cx] == kCellOther &&
          g_cells[cy + 1][cx] == kCellOther)
        g_cells[cy][cx] = g_cells[cy + 1][cx] = kCellTrough;
    }
  }
}

// Che cosa e' ogni cella 8x8 della vista all'aperto: cascata, acqua (attributo
// 0x08/0x09) o altro. Vero se c'e' una cascata
// Onde animate dello SNES sulla riva (caselle 0x1c0-0x1df all'aperto): i
// pixel scuri degli archi cambiano a ogni fase (ogni 9 fotogrammi) e la riva
// HD, fatta coi colori, saltava avanti e indietro (archi presi per sassi con
// la schiuma, soprattutto quando la terra e' fuori dallo schermo). Un pixel
// che cambia fra le tre fasi e' sempre acqua; quelli fissi seguono i colori.
// g_anim_var[casella][riga]: bit 7 - x se il pixel cambia
static uint8 g_anim_var[32][8];
static int g_anim_first = -1;   // prima casella animata, -1 se non si sa

static void ComputeAnimMask(void) {
  g_anim_first = -1;
  if (animated_tile_vram_addr != 0x3c00)
    return;
  g_anim_first = (animated_tile_vram_addr - 0x2000) >> 4;
  for (int tile = 0; tile < 32; tile++) {
    for (int row = 0; row < 8; row++) {
      uint8 diff = 0;
      int idx0[8];
      for (int ph = 0; ph < 3; ph++) {
        const uint8 *p = &g_ram[0xa680 + ph * 0x400 + tile * 32];
        uint8 b0 = p[row * 2], b1 = p[row * 2 + 1], b2 = p[16 + row * 2], b3 = p[17 + row * 2];
        for (int x = 0; x < 8; x++) {
          int s = 7 - x;
          int v = (b0 >> s & 1) | (b1 >> s & 1) << 1 | (b2 >> s & 1) << 2 | (b3 >> s & 1) << 3;
          if (ph == 0)
            idx0[x] = v;
          else if (v != idx0[x])
            diff |= 0x80 >> x;
        }
      }
      g_anim_var[tile][row] = diff;
    }
  }
}

// Il pixel (x, y) dello schermo e' un pixel che cambia di una casella animata?
static bool AnimWaterPixel(int x, int y, int cam_x, int cam_y, int ext) {
  int wx = x - ext + cam_x, wy = y + cam_y;
  int cx = (wx >> 3) - (((cam_x - ext) & ~7) >> 3) + kCellMargin, cy = (wy >> 3) - ((cam_y & ~7) >> 3) + kCellMargin;
  if (cx < 0 || cy < 0 || cx >= kMaxCellsX || cy >= kMaxCellsY)
    return false;
  int a = g_cell_anim[cy][cx];
  if (a < 0)
    return false;
  int px = wx & 7, py = wy & 7;
  if (a & 0x100) px = 7 - px;
  if (a & 0x200) py = 7 - py;
  return (g_anim_var[a & 0x1f][py] >> (7 - px) & 1) != 0;
}

// Nel Mondo Chiaro le onde animate della riva non hanno sempre l'attributo
// dell'acqua, ma anche i fiori sono animati: contano solo le caselle animate
// accanto all'acqua, o accanto (al massimo 2 passi) a quelle che lo sono
static void KeepShoreAnim(void) {
  static uint8 keep[kMaxCellsY][kMaxCellsX];
  for (int cy = 0; cy < kMaxCellsY; cy++)
    for (int cx = 0; cx < kMaxCellsX; cx++)
      keep[cy][cx] = g_cells[cy][cx] == kCellWater || g_cells[cy][cx] == kCellShallow;
  for (int step = 0; step < 3; step++) {
    for (int cy = 0; cy < kMaxCellsY; cy++) {
      for (int cx = 0; cx < kMaxCellsX; cx++) {
        if (keep[cy][cx] || g_cell_anim[cy][cx] < 0)
          continue;
        for (int dy = -1; dy <= 1 && !keep[cy][cx]; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int y = cy + dy, x = cx + dx;
            if (y >= 0 && x >= 0 && y < kMaxCellsY && x < kMaxCellsX && keep[y][x] && keep[y][x] < 2 + step) {
              keep[cy][cx] = 2 + step;
              break;
            }
          }
      }
    }
  }
  for (int cy = 0; cy < kMaxCellsY; cy++)
    for (int cx = 0; cx < kMaxCellsX; cx++)
      if (!keep[cy][cx])
        g_cell_anim[cy][cx] = -1;
}

static bool ClassifyCells(int width, int height, int cam_x, int cam_y, int ext) {
  bool world = overworld_screen_index < 0x80;  // non nelle aree speciali (Dominio di Zora...)
  const uint8 *attr8 = GetMap8toTileAttr();
  int x0 = ((cam_x - ext) & ~7) - kCellMargin * 8, y0 = (cam_y & ~7) - kCellMargin * 8;
  bool any = false;
  for (int cy = 0; cy < kMaxCellsY; cy++) {
    for (int cx = 0; cx < kMaxCellsX; cx++) {
      int wx = x0 + cx * 8, wy = y0 + cy * 8;
      uint8 v = kCellOther;
      g_cell_attr[cy][cx] = 0;
      g_cell_entry[cy][cx] = -1;
      g_cell_lip[cy][cx] = false;
      g_cell_anim[cy][cx] = -1;
      g_cell_shore[cy][cx] = -1;
      if (world && IsSkyPoint(wx, wy)) {
        v = kCellSky;
      } else if (wx < cam_x - ext + width + kCellMargin * 8 && wy < cam_y + height + kCellMargin * 8) {
        int e = CellEntry(wx, wy);
        if (e >= 0) {
          g_cell_attr[cy][cx] = attr8[e & 0x1ff];
          g_cell_entry[cy][cx] = (int16)(e & 0x1fff);
          g_cell_lip[cy][cx] = IsWaterfallEntry(e) && (e & 0x3ff) <= 0x1cb;
          // il portale fra i due mondi (0x1df, attributo 0x4b) e' una casella
          // animata come le onde della riva, ma non e' acqua; nel Mondo Oscuro
          // anche i fiori (crocette chiare): li' contano solo le caselle d'acqua
          // (nel Mondo Chiaro alcune onde della riva hanno altri attributi)
          int at = (e & 0x3ff) - g_anim_first, ea = attr8[e & 0x1ff];
          bool dark = overworld_screen_index & 0x40;
          if (dark && g_anim_first >= 0 && at >= 0 && at < 32 && !IsWaterfallEntry(e) && ea != 0x4b)
            g_cell_shore[cy][cx] = (int16)(at | (e & 0x4000 ? 0x100 : 0) | (e & 0x8000 ? 0x200 : 0) | (e & 0x1c00));
          if (g_anim_first >= 0 && at >= 0 && at < 32 && !IsWaterfallEntry(e) && ea != 0x4b &&
              (!dark || ea == 0x08 || ea == 0x09))
            g_cell_anim[cy][cx] = (int16)(at | (e & 0x4000 ? 0x100 : 0) | (e & 0x8000 ? 0x200 : 0) | (e & 0x1c00));
        }
        if (e < 0)
          v = kCellUnknown;
        else if (IsWaterfallEntry(e))
          v = kCellFall, any = true;
        else if (attr8[e & 0x1ff] == 0x08)
          v = kCellWater;
        else if (attr8[e & 0x1ff] == 0x09)
          v = kCellShallow;
        else if (attr8[e & 0x1ff] == 0x4b)
          v = kCellWarp;
      }
      g_cells[cy][cx] = v;
    }
  }
  if (!(overworld_screen_index & 0x40))
    KeepShoreAnim();
  if (any)
    FindFallParts();
  FindTroughs();
  FindBridges();
  return any;
}

// Bocche che versano acqua dal muro (Palazzo della Palude): nel pavimento (BG2)
// la casella 0x1b2 con la palette 7 e' l'acqua dentro la bocca, sotto la
// 0x1b3 e' il getto che cade nella vasca. Si disegnano come una piccola
// cascata. 1 = dentro la bocca (solo i pixel azzurri), 2 = getto, 0 = altro
static bool IsSpoutTile(uint16 e, int t) {
  return (e >> 10 & 7) == 7 && (e & 0x3ff) == t;
}

// Il punto del mondo e' nella stanza caricata: nello scorrimento fra due
// stanze la mappa delle caselle e' gia' della nuova anche per l'altra
static bool InLoadedRoom(int wx, int wy) {
  return ((wy >> 9 & 15) << 4 | (wx >> 9 & 15)) == (dungeon_room_index & 0xff);
}

static int SpoutIn(const uint16 *m, int pos) {
  uint16 e = m[pos];
  if (IsSpoutTile(e, 0x1b2))
    return pos < 64 * 63 && IsSpoutTile(m[pos + 64], 0x1b3) ? 1 : 0;
  // in fondo al getto la fila di schiuma 0x1b5 e' la fine del getto (3),
  // anche con la vasca piena (per il gioco e' acqua: i suoi puntini azzurri
  // diventavano isolette con la schiuma attorno)
  if (IsSpoutTile(e, 0x1b5) && pos >= 64) {
    for (int d = -1; d <= 1; d++) {
      int p = pos + d;
      if ((p >> 6) == (pos >> 6) && IsSpoutTile(m[p - 64], 0x1b3))
        return 3;
    }
    return 0;
  }
  // il getto e' largo 4 caselle, la bocca 2: valgono anche le caselle del
  // getto accanto (fino a 2) a una che sta sotto la bocca
  if (IsSpoutTile(e, 0x1b3) && pos >= 64) {
    for (int d = -2; d <= 2; d++) {
      int p = pos + d;
      if ((p >> 6) != (pos >> 6) || !IsSpoutTile(m[p], 0x1b3))
        continue;
      if (IsSpoutTile(m[p - 64], 0x1b2) || (d == 0 && IsSpoutTile(m[p - 64], 0x1b3)))
        return 2;
    }
  }
  return 0;
}

// bocche e getti sul pavimento (BG2) o sul piano di sopra (BG1, stanza 0x66)
static int SpoutAt(int wx, int wy) {
  if (!InLoadedRoom(wx, wy))
    return 0;
  int pos = ((wy & 0x1f8) << 3) + ((wx & 0x1f8) >> 3);
  int v = SpoutIn(dung_bg2, pos);
  return v ? v : SpoutIn(dung_bg1, pos);
}

static bool RoomHasSpouts(void) {
  for (int pos = 64; pos < 0x2000; pos++)
    if ((pos & 0xfff) >= 64 && IsSpoutTile(dung_bg2[pos], 0x1b3) && IsSpoutTile(dung_bg2[pos - 64], 0x1b2))
      return true;
  return false;
}

// Nei labirinti: acqua bassa (0x09) o alta (0x08) nel punto del mondo, dal
// piano di sotto o da quello di sopra (come HdFx_MarkWater)
static int DungeonDepthAt(int wx, int wy) {
  int i = ((wy & 0x1f8) << 3) + ((wx & 0x1f8) >> 3);
  if (HdWater_IsTubCell(i))
    return 1;  // vasca: acqua alta
  uint8 a = HdWater_DungAttr(i, wx, wy), b = HdWater_IsFlood(i) ? 0x08 : HdWater_DungAttr(i + 0x1000, wx, wy);
  return a == 0x09 || b == 0x09 ? 2 : a == 0x08 || b == 0x08 ? 1 : 0;
}

static inline int CellAt(int x, int y, int cam_x, int cam_y, int ext) {
  int wx = x - ext + cam_x, wy = y + cam_y;
  int cx = (wx >> 3) - (((cam_x - ext) & ~7) >> 3) + kCellMargin, cy = (wy >> 3) - ((cam_y & ~7) >> 3) + kCellMargin;
  return cx >= 0 && cy >= 0 && cx < kMaxCellsX && cy < kMaxCellsY ? g_cells[cy][cx] : kCellUnknown;
}

// Riga del mondo dove comincia la cima della cascata sopra il pixel, -1 se il
// pixel non e' nella cima: la fascia chiara si misura da li', uguale per
// tutte le colonne (anche quelle dei bordi diagonali, che cominciano piu' giu')
static int LipTop(int x, int y, int cam_x, int cam_y, int ext) {
  int wx = x - ext + cam_x, wy = y + cam_y;
  int y0 = ((cam_y & ~7) >> 3) - kCellMargin;
  int cx = (wx >> 3) - (((cam_x - ext) & ~7) >> 3) + kCellMargin, cy = (wy >> 3) - y0;
  if (cx < 0 || cy < 0 || cx >= kMaxCellsX || cy >= kMaxCellsY || !g_cell_lip[cy][cx])
    return -1;
  while (cy > 0 && g_cell_lip[cy - 1][cx])
    cy--;
  return (cy + y0) * 8;
}

// ---------------------------------------------------------------------------
// Distanza in pixel (trasformata a due passate, 3 in orizzontale e verticale,
// 4 in diagonale) dai pixel con g_kind >= seed (dalla riva: terreno e cascate,
// cosi' c'e' schiuma anche alla base delle cascate). HUD e personaggi sono
// "trasparenti"; fuori dall'immagine non c'e' riva.

static inline int MinI(int a, int b) {
  return a < b ? a : b;
}

// Trasformata a due passate su un'immagine w x h (w >= 2)
static void Chamfer(uint16 *d, int w, int h) {
  // andata: vicini a sinistra e sopra; la prima riga e le colonne ai bordi a
  // parte, cosi' il ciclo interno non ha controlli
  for (int x = 1; x < w; x++)
    d[x] = MinI(d[x], d[x - 1] + 3);
  for (int y = 1; y < h; y++) {
    uint16 *row = d + y * w;
    const uint16 *up = row - w;
    int v = MinI(row[0], MinI(up[0] + 3, up[1] + 4));
    row[0] = v;
    for (int x = 1; x < w - 1; x++) {
      v = MinI(MinI(row[x], v + 3), MinI(up[x] + 3, MinI(up[x - 1] + 4, up[x + 1] + 4)));
      row[x] = v;
    }
    row[w - 1] = MinI(MinI(row[w - 1], v + 3), MinI(up[w - 1] + 3, up[w - 2] + 4));
  }
  // ritorno: vicini a destra e sotto
  for (int x = w - 2; x >= 0; x--)
    d[(h - 1) * w + x] = MinI(d[(h - 1) * w + x], d[(h - 1) * w + x + 1] + 3);
  for (int y = h - 2; y >= 0; y--) {
    uint16 *row = d + y * w;
    const uint16 *dn = row + w;
    int v = MinI(row[w - 1], MinI(dn[w - 1] + 3, dn[w - 2] + 4));
    row[w - 1] = v;
    for (int x = w - 2; x > 0; x--) {
      v = MinI(MinI(row[x], v + 3), MinI(dn[x] + 3, MinI(dn[x + 1] + 4, dn[x - 1] + 4)));
      row[x] = v;
    }
    row[0] = MinI(MinI(row[0], v + 3), MinI(dn[0] + 3, dn[1] + 4));
  }
}

// Ogni parte calcola una striscia di colonne piu' un margine per lato largo
// quanto il limite: un percorso piu' corto del limite non esce dal margine,
// quindi nella striscia il risultato e' lo stesso dell'immagine intera
typedef struct DistJob {
  int w, h, seed_lo, seed_hi, cap, margin;
} DistJob;

static uint16 *g_band[kHdParMaxParts];
static int g_band_cap[kHdParMaxParts];

static void DistBand(void *ctx, int part, int x0, int x1) {
  const DistJob *j = (const DistJob *)ctx;
  int w = j->w, h = j->h;
  int a = x0 - j->margin, b = x1 + j->margin;
  if (a < 0) a = 0;
  if (b > w) b = w;
  int bw = b - a;
  if (bw * h > g_band_cap[part]) {
    uint16 *p = realloc(g_band[part], bw * h * sizeof(uint16));
    if (!p) {
      for (int y = 0; y < h; y++)
        for (int x = x0; x < x1; x++)
          g_dist[y * w + x] = (uint16)j->cap;
      return;
    }
    g_band[part] = p, g_band_cap[part] = bw * h;
  }
  uint16 *d = g_band[part];
  for (int y = 0; y < h; y++) {
    const uint8 *k = g_kind + y * w + a;
    uint16 *row = d + y * bw;
    for (int x = 0; x < bw; x++)
      row[x] = k[x] >= j->seed_lo && k[x] <= j->seed_hi ? 0 : (uint16)j->cap;
  }
  if (bw >= 2)
    Chamfer(d, bw, h);
  for (int y = 0; y < h; y++)
    memcpy(g_dist + y * w + x0, d + y * bw + (x0 - a), (x1 - x0) * sizeof(uint16));
}

// Distanza dai pixel con seed_lo <= g_kind <= seed_hi, fino a cap_px pixel
// (oltre vale cap_px): ogni uso chiede solo quanto gli serve
static void ComputeDistance(int w, int h, int seed_lo, int seed_hi, int cap_px) {
  DistJob j = { w, h, seed_lo, seed_hi, MinI(cap_px * kDistUnit, kDistMax), cap_px + 1 };
  HdPar_ForParts(DistBand, &j, w);
}

// Colori dell'acqua di questa scena: i piu' frequenti fra i pixel marcati
// (prima delle luci, quindi i colori della palette). Servono a trovare la riva
// vera: le caselle di riva per il gioco non sono acqua, ma contengono pezzi
// d'acqua (senza, la schiuma farebbe gli scalini delle caselle 8x8), e quelle
// d'acqua contengono le linee scure della riva.
enum { kMaxColors = 64, kMaxWaterColors = 8 };
static uint32 g_water_colors[kMaxWaterColors];
static int g_num_water_colors, g_water_min_luma;

static inline int Luma(uint32 c) {
  return (int)(c >> 16 & 0xff) * 3 + (int)(c >> 8 & 0xff) * 6 + (int)(c & 0xff);
}

// Stessa tinta: il verde e il blu rispetto al rosso vanno nello stesso verso
// (o sono quasi uguali al rosso)
static bool SameTint(uint32 a, uint32 b) {
  int ar = a >> 16 & 0xff, ag = a >> 8 & 0xff, ab = a & 0xff;
  int br = b >> 16 & 0xff, bg = b >> 8 & 0xff, bb = b & 0xff;
  bool g_ok = abs(ag - ar) <= 12 || abs(bg - br) <= 12 || (ag - ar) * (bg - br) > 0;
  bool b_ok = abs(ab - ar) <= 12 || abs(bb - br) <= 12 || (ab - ar) * (bb - br) > 0;
  return g_ok && b_ok;
}

static void FindWaterColors(int n, int n_water) {
  uint32 col[kMaxColors];
  int cnt[kMaxColors], inner[kMaxColors], nc = 0, last = -1;
  for (int i = 0; i < n; i++) {
    if (g_kind[i] != 1 || g_fixed[i])
      continue;  // le onde animate non contano: hanno i colori della fase
    uint32 c = g_pre[i] & 0xffffff;
    if (last < 0 || col[last] != c) {
      last = -1;
      for (int j = 0; j < nc; j++)
        if (col[j] == c) { last = j; break; }
      if (last < 0) {
        if (nc == kMaxColors)
          continue;
        col[nc] = c, cnt[nc] = 0, inner[nc] = 0, last = nc++;
      }
    }
    cnt[last]++;
    inner[last] += g_dist[i] >= 4 * kDistUnit;
  }
  // il bordo scuro delle vasche e le linee della riva stanno solo vicino alla
  // riva: non sono colori dell'acqua anche se sono frequenti
  for (int j = 0; j < nc; j++)
    if (inner[j] * 10 < cnt[j] * 3)
      cnt[j] = 0;
  g_num_water_colors = 0;
  g_water_min_luma = 1 << 30;
  int found[kMaxWaterColors];
  for (int k = 0; k < kMaxWaterColors; k++) {
    int best = -1;
    for (int j = 0; j < nc; j++)
      if (cnt[j] > 0 && (best < 0 || cnt[j] > cnt[best]))
        best = j;
    // solo i colori che fanno almeno il 2% dell'acqua
    if (best < 0 || cnt[best] * 50 < n_water)
      break;
    // tinta opposta a quella del colore principale (piu' rosso che verde e blu
    // mentre l'acqua e' verde o blu): e' la base scura delle scogliere del
    // Mondo Oscuro, larga e frequente, non acqua
    if (g_num_water_colors && !SameTint(col[best], g_water_colors[0])) {
      cnt[best] = 0;
      k--;
      continue;
    }
    found[g_num_water_colors] = cnt[best];
    g_water_colors[g_num_water_colors++] = col[best];
    if (Luma(col[best]) < g_water_min_luma)
      g_water_min_luma = Luma(col[best]);
    cnt[best] = 0;
  }
  // Il primo colore e' quello delle onde animate della riva (BlurRows). Con due
  // colori quasi alla pari (acqua alta e bassa, Laghetto della Montagna) il piu'
  // frequente cambiava a ogni fase delle onde (9 fotogrammi) e la fascia lungo
  // la riva si schiariva e si scuriva: resta quello di prima finche' e' ancora
  // un colore dell'acqua con almeno il 60% dei pixel del primo
  static uint32 prev_first;
  for (int j = 1; j < g_num_water_colors; j++) {
    if (g_water_colors[j] == prev_first && found[j] * 10 >= found[0] * 6) {
      uint32 c = g_water_colors[0];
      g_water_colors[0] = g_water_colors[j], g_water_colors[j] = c;
      break;
    }
  }
  if (g_num_water_colors)
    prev_first = g_water_colors[0];
}

// Luminosita' dello schermo (INIDISP, 1-15): nelle dissolvenze (mappa del
// labirinto, cambi di stanza) il gioco scurisce tutto. Le soglie sui colori
// e i colori chiari (schiuma, cascate, nebbia) seguono la luminosita': prima
// i colori scuri si somigliavano tutti, i muri diventavano acqua con la
// schiuma attorno e la schiuma restava bianca sullo schermo quasi nero
static int g_bri = 15;
static float g_bright = 1.0f;

// Azzurro o blu: la fascia scura lungo la riva, le sue onde animate (gli
// ovali dello SNES) e la linea chiara della riva. Erba, terra e rocce no
// La notte di pioggia dell'inizio (prima di salvare Zelda) ha i colori piu'
// scuri: la fascia lungo la riva (2d3952) ha il blu solo 37 sopra il rosso e
// diventava terra, con la schiuma bianca a scalini attorno (g_night)
static bool g_night;

static inline bool IsBluish(uint32 c) {
  int cr = c >> 16 & 0xff, cg = c >> 8 & 0xff, cb = c & 0xff;
  return cb >= cr + (g_night ? 30 : 40) * g_bri / 15 && cb >= cg + 16 * g_bri / 15 && cb > cr;
}

// Pixel di cascata nei bordi diagonali accanto alla cascata: azzurro, e nel
// Mondo Oscuro (cascate verdi) anche verde chiaro; il resto e' roccia
static inline bool IsFallEdgeColor(uint32 c) {
  if (IsBluish(c))
    return true;
  int cr = c >> 16 & 0xff, cg = c >> 8 & 0xff, cb = c & 0xff;
  return (overworld_screen_index & 0x40) && cg >= cr + 40 * g_bri / 15 && cg >= cb;
}

static inline bool IsWaterColor(uint32 c) {
  c &= 0xffffff;
  for (int j = 0; j < g_num_water_colors; j++)
    if (g_water_colors[j] == c)
      return true;
  return false;
}

// Colore simile a uno dei colori dell'acqua (somma delle differenze <= tol) e
// della stessa tinta (il blu rispetto al rosso va nello stesso verso): il
// pavimento scuro accanto a un'acqua scura non e' acqua
static inline bool IsNearWaterColor(uint32 c, int tol) {
  int cr = c >> 16 & 0xff, cg = c >> 8 & 0xff, cb = c & 0xff;
  tol = tol * g_bri / 15;
  for (int j = 0; j < g_num_water_colors; j++) {
    uint32 w = g_water_colors[j];
    int wr = w >> 16 & 0xff, wg = w >> 8 & 0xff, wb = w & 0xff;
    int d = abs(cr - wr) + abs(cg - wg) + abs(cb - wb);
    if (d <= tol && (d == 0 || (cb - cr) * (wb - wr) > 0))
      return true;
  }
  return false;
}

// I pezzetti di riva piccoli (meno di 24 pixel) che toccano l'acqua sono
// acqua: i pixel scuri delle onde animate dello SNES sul bordo della riva
// facevano dei cerchietti di schiuma. Sassi e pali sono molto piu' grandi.
// g_sum2 fa da pila: si riempie dopo, in BlurWater
static void FillSmallIslands(int w, int h) {
  enum { kSmallIsland = 24 };
  uint32 *st = g_sum2;
  int n = w * h;
  for (int y0 = 0; y0 < h; y0++)
  for (int x0 = 0; x0 < w; x0++) {
    // si parte solo dalla riva attaccata all'acqua e ci si ferma appena il
    // pezzo e' grande o tocca un pezzo grande gia' visto: sulla terra
    // lontana non si fa niente
    int i = y0 * w + x0;
    if (g_kind[i] != 3)
      continue;
    if (!(x0 > 0 && g_kind[i - 1] == 1) && !(x0 < w - 1 && g_kind[i + 1] == 1) &&
        !(y0 > 0 && g_kind[i - w] == 1) && !(y0 < h - 1 && g_kind[i + w] == 1))
      continue;
    int top = 0, head = 0;
    bool water = false, big = false;
    st[top++] = i, g_kind[i] = 4;
    while (head < top && !big) {
      int p = st[head++], x = p % w, y = p / w;
      int q[4] = { x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, y > 0 ? p - w : -1, y < h - 1 ? p + w : -1 };
      for (int j = 0; j < 4; j++) {
        if (q[j] < 0)
          continue;
        uint8 k = g_kind[q[j]];
        if (k == 3) {
          g_kind[q[j]] = 4, st[top++] = q[j];
          if (top >= kSmallIsland)
            big = true;
        } else if (k == 1) {
          water = true;
        } else if (k == 5) {
          big = true;
        }
      }
    }
    bool fill = water && !big;
    for (int j = 0; j < top; j++) {
      g_kind[st[j]] = fill ? 1 : 5;
      if (fill)
        g_depth[st[j]] = 0;
    }
  }
  for (int i = 0; i < n; i++)
    if (g_kind[i] == 5)
      g_kind[i] = 3;
}

// Media dei colori dell'acqua (prima delle luci) in un quadrato di lato
// 2r+1: toglie le righe dell'acqua dello SNES ma tiene le zone chiare
// dell'acqua bassa e quelle scure dell'acqua profonda
// Per pixel tre numeri a 32 bit, che si sommano come numeri interi perche'
// ogni campo resta nel suo spazio (il quadrato ha 225 pixel):
//  [0] rosso | verde << 16          (fino a 225 * 255 = 57375 ciascuno)
//  [1] blu | quanti << 16 | quanti bassi << 24   (quanti fino a 225)
//  [2] quanti con profondita' nota
// La meta' della memoria di prima: la media e' limitata dalla memoria
typedef struct BlurJob {
  int w, h, r;
  uint32 *acc;
} BlurJob;

static void BlurRows(void *ctx, int part, int y0, int y1) {
  const BlurJob *j = (const BlurJob *)ctx;
  int w = j->w, r = j->r;
  uint32 *s = g_sum;
  for (int y = y0; y < y1; y++) {
    uint32 acc[kSumCh] = { 0 };
    const uint32 *pre = g_pre + y * w;
    const uint8 *k = g_kind + y * w, *dp = g_depth + y * w, *fx = g_fixed + y * w;
    // le onde animate contano col colore dell'acqua, non con quello della
    // fase (gli archi scuri facevano una macchia che cambiava ogni 9 fotogrammi)
    uint32 wc = g_num_water_colors ? g_water_colors[0] : 0;
    for (int x = -r; x < w + r; x++) {
      int xin = x + r, xout = x - r - 1;
      if (xin < w && k[xin] == 1) {
        uint32 c = fx[xin] && wc ? wc : pre[xin];
        acc[0] += (c >> 16 & 0xff) | (c >> 8 & 0xff) << 16;
        acc[1] += (c & 0xff) | 1 << 16 | (uint32)(dp[xin] == 2) << 24;
        acc[2] += dp[xin] != 0;
      }
      if (xout >= 0 && k[xout] == 1) {
        uint32 c = fx[xout] && wc ? wc : pre[xout];
        acc[0] -= (c >> 16 & 0xff) | (c >> 8 & 0xff) << 16;
        acc[1] -= (c & 0xff) | 1 << 16 | (uint32)(dp[xout] == 2) << 24;
        acc[2] -= dp[xout] != 0;
      }
      if (x >= 0 && x < w)
        memcpy(&s[(y * w + x) * kSumCh], acc, sizeof(acc));
    }
  }
}

// in verticale riga per riga, con una somma per colonna (scorrere la memoria
// per colonne era lento); ogni parte ha le sue colonne
static void BlurCols(void *ctx, int part, int x0, int x1) {
  const BlurJob *j = (const BlurJob *)ctx;
  int h = j->h, r = j->r, m = j->w * kSumCh;
  int j0 = x0 * kSumCh, j1 = x1 * kSumCh;
  uint32 *acc = j->acc, *s = g_sum, *t = g_sum2;
  memset(acc + j0, 0, (j1 - j0) * sizeof(uint32));
  for (int y = -r; y < h + r; y++) {
    int yin = y + r, yout = y - r - 1;
    if (yin < h) {
      const uint32 *p = s + yin * m;
      for (int i = j0; i < j1; i++) acc[i] += p[i];
    }
    if (yout >= 0) {
      const uint32 *p = s + yout * m;
      for (int i = j0; i < j1; i++) acc[i] -= p[i];
    }
    if (y >= 0 && y < h)
      memcpy(t + y * m + j0, acc + j0, (j1 - j0) * sizeof(uint32));
  }
}

static void BlurWater(int w, int h, int r) {
  static uint32 *acc;
  static int acc_cap;
  int m = w * kSumCh;
  if (m > acc_cap) {
    uint32 *a = realloc(acc, m * sizeof(uint32));
    if (!a)
      return;
    acc = a, acc_cap = m;
  }
  BlurJob j = { w, h, r, acc };
  HdPar_For(BlurRows, &j, h);
  HdPar_ForParts(BlurCols, &j, w);
}

// Colore sottratto dalle luci in questo frame (g_hd_light_sub): la luce e'
// pixel * fattore - g_light_sub. Il rapporto si calcola senza la sottrazione,
// che si rifa' sul colore dell'acqua (LitColor). Col solo rapporto il fattore
// cambiava col disegno dell'acqua originale (squame visibili nell'acqua HD
// delle stanze buie con 1-2 torce accese)
static float g_light_sub;

// Rapporto fra il pixel illuminato e lo stesso prima delle luci, per canale
static inline void LightRatio(uint32 lit, uint32 pre, float *k) {
  for (int c = 0; c < 3; c++) {
    float a = (float)(lit >> (16 - 8 * c) & 0xff) + g_light_sub, b = (float)(pre >> (16 - 8 * c) & 0xff);
    float v = (a + 6) / (b + 6);
    // vicino al nero il +6 lasciava 1/8 della luce: nelle stanze da Lanterna
    // schiuma e rive si vedevano grigie nel buio (alone bianco). Solo dove la
    // luce ha scurito il pixel: un canale gia' a zero nell'originale (il
    // contorno 000052 dell'erba nel Dominio di Zora) resta com'era
    if (a < 6 && a < b)
      v *= a * (1.0f / 6);
    k[c] = v > 4 ? 4 : v;
  }
}

static inline uint32 PackColor(float r, float g, float b, uint32 layer) {
  int ir = (int)(r + 0.5f), ig = (int)(g + 0.5f), ib = (int)(b + 0.5f);
  ir = ir < 0 ? 0 : ir > 255 ? 255 : ir;
  ig = ig < 0 ? 0 : ig > 255 ? 255 : ig;
  ib = ib < 0 ? 0 : ib > 255 ? 255 : ib;
  return layer << 24 | ir << 16 | ig << 8 | ib;
}

// Colore dell'acqua illuminato: k da LightRatio, poi la sottrazione delle luci
static inline uint32 LitColor(float r, float g, float b, const float *k) {
  return PackColor(r * k[0] * g_bright - g_light_sub, g * k[1] * g_bright - g_light_sub, b * k[2] * g_bright - g_light_sub, kLayerWater);
}

// ---------------------------------------------------------------------------
// Nebbia alla base delle cascate (in coordinate del mondo)

typedef struct Mist {
  float x, y, vx, vy, r0, r1;
  uint16 age, life;
} Mist;

static Mist g_mist[kMaxMist];
static int g_num_mist;

enum { kMaxBases = 128 };
static int g_base_x[kMaxBases], g_base_y[kMaxBases], g_num_bases;

static bool g_small_mist;  // getti delle bocche nei labirinti: sbuffi piccoli e radi

static void UpdateMist(bool step) {
  if (!step)
    return;
  float scale = g_small_mist ? 0.45f : 1.0f;
  int n = 0;
  for (int i = 0; i < g_num_mist; i++) {
    Mist *m = &g_mist[i];
    if (++m->age >= m->life)
      continue;
    m->x += m->vx, m->y += m->vy;
    m->vx += (Rand01() - 0.5f) * 0.02f;
    g_mist[n++] = *m;
  }
  g_num_mist = n;
  // in media uno sbuffo ogni 2 frame ogni 16 pixel di base
  int spawn = (int)(g_num_bases * 0.125f * scale + Rand01());
  for (int s = 0; s < spawn && g_num_mist < kMaxMist; s++) {
    int b = (int)(Rand01() * g_num_bases);
    if (b >= g_num_bases)
      continue;
    Mist *m = &g_mist[g_num_mist++];
    m->x = g_base_x[b] + (Rand01() - 0.5f) * 8;
    m->y = g_base_y[b] - 2 + (Rand01() - 0.5f) * 6;
    m->vx = (Rand01() - 0.5f) * 0.35f;
    m->vy = -0.05f - Rand01() * 0.25f + (Rand01() < 0.35f ? 0.3f : 0);  // un po' sale, un po' scivola sull'acqua
    m->r0 = (4 + Rand01() * 4) * scale;
    m->r1 = (12 + Rand01() * 12) * scale;
    m->age = 0;
    m->life = 60 + (uint16)(Rand01() * 70);
  }
}

// Parte dell'immagine disegnata dal gioco: senza il mondo continuo, nelle aree
// speciali (Dominio di Zora, sotto il ponte...) e al bordo dell'area le fasce
// laterali e la riga in fondo restano nere. Li' l'acqua non c'e'
static int g_vis_x0, g_vis_x1, g_vis_y1;

// dark: Mondo Oscuro all'aperto, nebbia nera come la schiuma della riva
static void DrawMist(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y, int ext, bool dark) {
  for (int i = 0; i < g_num_mist; i++) {
    Mist *m = &g_mist[i];
    float k = (float)m->age / m->life;
    float rad = m->r0 + (m->r1 - m->r0) * sqrtf(k);
    // compare in fretta, svanisce piano
    float alpha = (k < 0.15f ? k / 0.15f : 1 - (k - 0.15f) / 0.85f) * 0.16f;
    float cx = m->x - cam_x + ext, cy = m->y - cam_y;
    int x0 = (int)(cx - rad), x1 = (int)(cx + rad) + 1, y0 = (int)(cy - rad), y1 = (int)(cy + rad) + 1;
    if (x1 < 0 || y1 < 0 || x0 >= width || y0 >= height)
      continue;
    if (x0 < g_vis_x0) x0 = g_vis_x0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_vis_x1) x1 = g_vis_x1;
    if (y1 > g_vis_y1) y1 = g_vis_y1;
    float inv = 1.0f / (rad * rad);
    for (int y = y0; y < y1; y++) {
      uint32 *row = (uint32 *)(pixels + y * pitch);
      for (int x = x0; x < x1; x++) {
        float dx = x - cx, dy = y - cy, q = 1 - (dx * dx + dy * dy) * inv;
        if (q <= 0 || (row[x] >> 24) == kLayerHud)
          continue;
        float a = alpha * q * q;
        uint32 c = row[x];
        float r = (float)(c >> 16 & 0xff), g = (float)(c >> 8 & 0xff), b = (float)(c & 0xff);
        float tr = 240 * g_bright, tg = 246 * g_bright, tb = 255 * g_bright;
        if (dark)
          tr = r * 0.12f + 6, tg = g * 0.12f + 2, tb = b * 0.12f;
        row[x] = PackColor(r + (tr - r) * a, g + (tg - g) * a, b + (tb - b) * a, c >> 24);
      }
    }
  }
}

// ---------------------------------------------------------------------------

// Che cosa e' ogni pixel: 1 acqua, 2 cascata, 3 riva (terreno), 0 il resto
typedef struct ClassJob {
  const uint8 *pixels;
  int pitch, w, cam_x, cam_y, ext;
  bool outdoors, falls, outside_area;
  bool stream;  // getto che scende nella vasca vuota (leva del Palazzo della Palude)
  int n_water[kHdParMaxParts], n_fall[kHdParMaxParts];
  float fall_sum[kHdParMaxParts][3];
} ClassJob;

// Dentro la bocca: getto dal primo pixel azzurro della colonna in giu' (il
// labbro scuro della bocca sopra l'acqua faceva una riga a trattini)
static bool SpoutWaterBelow(int x, int y, int w, int cam_y) {
  for (int yy = y, n = (y + cam_y) & 7; yy >= 0 && n >= 0; yy--, n--)
    if (IsBluish(g_pre[yy * w + x]))
      return true;
  return false;
}

static void ClassifyRows(void *ctx, int part, int y0, int y1) {
  ClassJob *j = (ClassJob *)ctx;
  int w = j->w, cam_x = j->cam_x, cam_y = j->cam_y, ext = j->ext;
  bool outdoors = j->outdoors;
  int n_water = 0, n_fall = 0;
  float fall_sum[3] = { 0 };
  for (int y = y0; y < y1; y++) {
    const uint32 *row = (const uint32 *)(j->pixels + y * j->pitch);
    uint8 *k = g_kind + y * w, *dp = g_depth + y * w;
    for (int x = 0; x < w; x++) {
      uint32 l = row[x] >> 24;
      int cell = outdoors && (l <= 1 || l == 5 || l == kLayerWater) ? CellAt(x, y, cam_x, cam_y, ext) : kCellUnknown;
      dp[x] = cell == kCellShallow ? 2 : cell == kCellWater ? 1 : 0;
      if (!outdoors && l == kLayerWater)
        dp[x] = DungeonDepthAt(x - ext + cam_x, y + cam_y);
      g_fixed[y * w + x] = 0;
      int spout = !outdoors && j->falls ? SpoutAt(x - ext + cam_x, y + cam_y) : 0;
      if (x < g_vis_x0 || x >= g_vis_x1 || y >= g_vis_y1) {
        k[x] = 0;  // fasce nere: non e' una riva
      } else if (cell == kCellWarp) {
        k[x] = 0;  // portale fra i mondi: ne' acqua ne' riva (e' azzurro)
      } else if (spout && (l == kLayerHud || l == 4 || l == 6)) {
        k[x] = 6;  // sprite sopra il getto: non lo interrompono
      } else if ((spout && (l <= 1 || l == 5 || (spout == 3 && l == kLayerWater)) && (spout >= 2 || SpoutWaterBelow(x, y, w, cam_y))) || (j->stream && l == kLayerWater)) {
        uint32 c = g_pre[y * w + x];  // getto della bocca: piccola cascata
        fall_sum[0] += c >> 16 & 0xff, fall_sum[1] += c >> 8 & 0xff, fall_sum[2] += c & 0xff;
        n_fall++;
        k[x] = 2;
      } else if (outdoors && (l <= 1 || l == 5 || l == kLayerWater) && cell != kCellSky &&
                 AnimWaterPixel(x, y, cam_x, cam_y, ext)) {
        k[x] = 1, n_water++;  // onda animata della riva: acqua in ogni fase
        g_fixed[y * w + x] = 1;
      } else if (j->falls && (cell == kCellFall || cell == kCellFallEdge) && (l == kLayerHud || l == 4 || l == 6)) {
        k[x] = 6;  // cuori, Link, nemici sopra la cascata: non la interrompono
      } else if (j->falls && (cell == kCellFall || (cell == kCellFallEdge && IsFallEdgeColor(g_pre[y * w + x]))) && l != 0) {
        uint32 c = g_pre[y * w + x];
        fall_sum[0] += c >> 16 & 0xff, fall_sum[1] += c >> 8 & 0xff, fall_sum[2] += c & 0xff;
        n_fall++;
        k[x] = 2;
      } else if (l == kLayerWater && (cell == kCellWater || cell == kCellShallow || cell == kCellUnknown)) {
        k[x] = 1, n_water++;
      } else if (cell == kCellTrough && IsBluish(g_pre[y * w + x])) {
        k[x] = 1, n_water++;  // acqua delle vasche
      } else if (cell == kCellBridge || (!outdoors && l <= 1 && FloodBridgeAt(x - ext + cam_x, y + cam_y))) {
        k[x] = 0;  // ponte: l'acqua passa sotto, non e' una riva
      } else if (j->outside_area && !HdWorld_InsideCurrentArea(x - ext + cam_x, y + cam_y)) {
        k[x] = 0;  // fuori dall'area senza mondo continuo: nero, non e' una riva
      } else {
        // anche i pixel d'acqua delle caselle di riva (le onde animate dello
        // SNES sul bordo, casella 0x1c0): sono riva, e quelli azzurri vicino
        // all'acqua diventano acqua qui sotto. Lasciati come erano, quando la
        // telecamera li scopriva lampeggiavano i cerchietti originali
        k[x] = (l <= 1 || l == 5 || (outdoors && l == kLayerWater && cell != kCellSky)) ? 3 : 0;
      }
    }
    // Mondo Oscuro: nei bordi diagonali della cascata verde fra i trattini
    // verdi ci sono pixel neri; fra due pixel di cascata della stessa riga
    // (nelle caselle della cascata e del suo bordo) e' tutto cascata, se no
    // i lati restavano a strisce. La roccia fuori dal primo pixel verde resta
    if (j->falls && outdoors && (overworld_screen_index & 0x40)) {
      int last = -1;
      for (int x = 0; x < w; x++) {
        if (k[x] != 2)
          continue;
        if (last >= 0 && x - last > 1 && x - last <= 16) {
          bool ok = true;
          for (int i = last + 1; i < x && ok; i++) {
            int c = CellAt(i, y, cam_x, cam_y, ext);
            uint32 l = row[i] >> 24;
            ok = (c == kCellFall || c == kCellFallEdge) && (l <= 1 || l == 5 || l == kLayerWater);
          }
          for (int i = last + 1; i < x && ok; i++) {
            if (k[i] == 1)
              n_water--;
            uint32 c = g_pre[y * w + i];
            fall_sum[0] += c >> 16 & 0xff, fall_sum[1] += c >> 8 & 0xff, fall_sum[2] += c & 0xff;
            n_fall++;
            k[i] = 2, g_fixed[y * w + i] = 0;
          }
        }
        last = x;
      }
    }
  }
  j->n_water[part] += n_water, j->n_fall[part] += n_fall;
  for (int c = 0; c < 3; c++)
    j->fall_sum[part][c] += fall_sum[c];
}

// Pixel che cambia di una casella animata della riva: nel Mondo Chiaro sono
// le onde (azzurre o bianche), nel Mondo Oscuro la linea verde della riva che
// si sposta sulla base nera della scogliera. Se in una fase dell'animazione
// il pixel e' nero (molto piu' scuro dell'acqua, non dei suoi colori e non
// azzurro) e' riva: prima diventava acqua e la riva si staccava dai sassi
static bool AnimPixelWaterish(int x, int y, int cam_x, int cam_y, int ext) {
  int wx = x - ext + cam_x, wy = y + cam_y;
  int cx = (wx >> 3) - (((cam_x - ext) & ~7) >> 3) + kCellMargin, cy = (wy >> 3) - ((cam_y & ~7) >> 3) + kCellMargin;
  if (cx < 0 || cy < 0 || cx >= kMaxCellsX || cy >= kMaxCellsY)
    return true;
  int a = g_cell_anim[cy][cx];
  if (a < 0)
    return true;
  int px = wx & 7, py = wy & 7;
  if (a & 0x100) px = 7 - px;
  if (a & 0x200) py = 7 - py;
  int tile = a & 0x1f, pal = (a >> 10) & 7, s = 7 - px;
  for (int ph = 0; ph < 3; ph++) {
    const uint8 *p = &g_ram[0xa680 + ph * 0x400 + tile * 32];
    int v = (p[py * 2] >> s & 1) | (p[py * 2 + 1] >> s & 1) << 1 | (p[16 + py * 2] >> s & 1) << 2 |
            (p[17 + py * 2] >> s & 1) << 3;
    // trasparente: il colore di fondo
    uint16 c15 = main_palette_buffer[v ? pal * 16 + v : 0];
    int r = (c15 & 31) << 3, g = (c15 >> 5 & 31) << 3, b = (c15 >> 10 & 31) << 3;
    uint32 c = (uint32)(r * g_bri / 15) << 16 | (uint32)(g * g_bri / 15) << 8 | (uint32)(b * g_bri / 15);
    if (Luma(c) * 3 < g_water_min_luma * 2 && !IsNearWaterColor(c, 48) && !IsBluish(c))
      return false;
  }
  return true;
}

static void AnimShorePixels(int w, int h, int cam_x, int cam_y, int ext) {
  if (!g_num_water_colors)
    return;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int i = y * w + x;
      if (g_fixed[i] && g_kind[i] == 1 && !AnimPixelWaterish(x, y, cam_x, cam_y, ext))
        g_kind[i] = 3, g_fixed[i] = 0;
    }
}

// Mondo Oscuro: la riva e' la base nera della scogliera con le linee verdi
// animate dello SNES. Nel Mondo Chiaro le onde diventano acqua con la schiuma
// bianca, qui restavano le linee originali: la fascia diventa acqua
// (DarkShoreToWater) e la schiuma e' la stessa, ma nera (PaintWaterRows)
// Tutto per caselle calcolato una volta per frame (maschere separabili) e i
// pixel in parallelo: prima ogni pixel guardava piu' caselle e il Mondo
// Oscuro costava 2 ms in piu' di una scena uguale del Mondo Chiaro
static uint8 g_cell_border[kMaxCellsY][kMaxCellsX];     // riva nella casella o in una delle 4 accanto
static uint32 g_cell_dark[kMaxCellsY][kMaxCellsX];      // colore piu' scuro della casella di riva

static inline int CellXY(int x, int y, int cam_x, int cam_y, int ext, int *cy) {
  int wx = x - ext + cam_x, wy = y + cam_y;
  int cx = (wx >> 3) - (((cam_x - ext) & ~7) >> 3) + kCellMargin;
  *cy = (wy >> 3) - ((cam_y & ~7) >> 3) + kCellMargin;
  return cx;
}

static int ShoreCellOf(int x, int y, int cam_x, int cam_y, int ext) {
  int cy, cx = CellXY(x, y, cam_x, cam_y, ext, &cy);
  if (cx < 0 || cy < 0 || cx >= kMaxCellsX || cy >= kMaxCellsY)
    return -1;
  return g_cell_shore[cy][cx];
}

// v[y][x] = 1 se src ha un 1 nel quadrato di lato 2r+1 (in due passi)
static void DilateCells(uint8 dst[kMaxCellsY][kMaxCellsX], uint8 src[kMaxCellsY][kMaxCellsX], int r) {
  static uint8 tmp[kMaxCellsY][kMaxCellsX];
  for (int cy = 0; cy < kMaxCellsY; cy++)
    for (int cx = 0; cx < kMaxCellsX; cx++) {
      uint8 v = 0;
      for (int dx = -r; dx <= r && !v; dx++) {
        int xx = cx + dx;
        v = xx >= 0 && xx < kMaxCellsX && src[cy][xx];
      }
      tmp[cy][cx] = v;
    }
  for (int cy = 0; cy < kMaxCellsY; cy++)
    for (int cx = 0; cx < kMaxCellsX; cx++) {
      uint8 v = 0;
      for (int dy = -r; dy <= r && !v; dy++) {
        int yy = cy + dy;
        v = yy >= 0 && yy < kMaxCellsY && tmp[yy][cx];
      }
      dst[cy][cx] = v;
    }
}

// isolette piccole in mezzo all'acqua (pali, sassi, piante): li' la schiuma
// nera ha lo spessore normale, sulla costa 1,2 volte (richiesta
// dell'utente). 1 = casella di un'isoletta o a 2 caselle
static uint8 g_cell_small[kMaxCellsY][kMaxCellsX];
static uint8 g_cell_own[kMaxCellsY][kMaxCellsX], g_cell_near1[kMaxCellsY][kMaxCellsX];

typedef struct DarkJob {
  int w, h, cam_x, cam_y, ext, mode;
  uint32 band;
  bool have_band;
} DarkJob;

// 0x40: pixel di riva dell'isoletta, DarkShoreToWater non lo tocca e
// l'anello verde dei pali resta quello del gioco originale; si toglie dopo.
// Solo i pixel delle caselle accanto ai pali (sono poche: prima si
// scorreva tutto lo schermo cinque volte). mode 0: i pixel nelle caselle
// dell'isoletta; 1: quelli attaccati a un pixel gia' segnato, a una casella
// al massimo (0x20); 2: 0x20 -> 0x40
static void IslandCells(int w, int h, int cam_x, int cam_y, int ext, int mode) {
  int bx = ((cam_x - ext) & ~7) >> 3, by = (cam_y & ~7) >> 3;
  for (int cy = 0; cy < kMaxCellsY; cy++)
    for (int cx = 0; cx < kMaxCellsX; cx++) {
      if (!(mode == 0 ? g_cell_own[cy][cx] : g_cell_near1[cy][cx]))
        continue;
      int sx = (bx + cx - kCellMargin) * 8 - cam_x + ext, sy = (by + cy - kCellMargin) * 8 - cam_y;
      for (int y = sy < 0 ? 0 : sy; y < sy + 8 && y < h; y++)
        for (int x = sx < 0 ? 0 : sx; x < sx + 8 && x < w; x++) {
          int i = y * w + x;
          if (mode == 2) {
            if (g_kind[i] & 0x20)
              g_kind[i] = (g_kind[i] & ~0x20) | 0x40;
          } else if (g_kind[i] != 3) {
          } else if (mode == 0) {
            g_kind[i] |= 0x40;
          } else if ((x > 0 && (g_kind[i - 1] & 0x40)) || (x < w - 1 && (g_kind[i + 1] & 0x40)) ||
                     (y > 0 && (g_kind[i - w] & 0x40)) || (y < h - 1 && (g_kind[i + w] & 0x40))) {
            g_kind[i] |= 0x20;
          }
        }
    }
}

static void MarkSmallIslands(int w, int h, int cam_x, int cam_y, int ext) {
  // per caselle e non per pixel: le caselle arrivano kCellMargin oltre lo
  // schermo, un palo tagliato dal bordo dello schermo resta un palo (prima,
  // toccando il bordo, sembrava terra grande e perdeva l'anello)
  enum { kSmallCells = 16 };
  static uint8 seen[kMaxCellsY][kMaxCellsX];
  static int16 st[kMaxCellsY * kMaxCellsX];
  memset(seen, 0, sizeof(seen));
  memset(g_cell_own, 0, sizeof(g_cell_own));
#define LAND(cy, cx) (g_cells[cy][cx] != kCellWater && g_cells[cy][cx] != kCellShallow)
  for (int sy = 0; sy < kMaxCellsY; sy++)
    for (int sx = 0; sx < kMaxCellsX; sx++) {
      if (seen[sy][sx] || !LAND(sy, sx))
        continue;
      int top = 0, head = 0;
      bool big = false;
      st[top++] = (int16)(sy * kMaxCellsX + sx), seen[sy][sx] = 1;
      while (head < top) {
        int p = st[head++], cy = p / kMaxCellsX, cx = p % kMaxCellsX;
        // fuori dalla mappa nota o sul bordo della griglia: non si sa, e' terra
        if (g_cells[cy][cx] == kCellUnknown || cy == 0 || cx == 0 || cy == kMaxCellsY - 1 || cx == kMaxCellsX - 1)
          big = true;
        static const int8 d[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
        for (int k = 0; k < 4; k++) {
          int yy = cy + d[k][0], xx = cx + d[k][1];
          if (yy < 0 || xx < 0 || yy >= kMaxCellsY || xx >= kMaxCellsX || seen[yy][xx] || !LAND(yy, xx))
            continue;
          seen[yy][xx] = 1, st[top++] = (int16)(yy * kMaxCellsX + xx);
        }
      }
      if (big || top > kSmallCells)
        continue;
      for (int j = 0; j < top; j++)
        g_cell_own[st[j] / kMaxCellsX][st[j] % kMaxCellsX] = 1;
    }
#undef LAND
  // i sassi nell'acqua (caselle 0x1e1 sopra e 0x1cf sotto, palette 7; nella
  // palude sembrano ceppi) tengono tutti l'anello col filo verde dello SNES e
  // la schiuma attorno, anche quelli che si toccano (cerchio di sassi): prima
  // solo le isolette piccole, e nel cerchio l'anello diventava acqua
  for (int cy = 0; cy < kMaxCellsY; cy++)
    for (int cx = 0; cx < kMaxCellsX; cx++) {
      int e = g_cell_entry[cy][cx], t = e & 0x3ff;
      if (e >= 0 && (e >> 10 & 7) == 7 && (t == 0x1e1 || t == 0x1cf))
        g_cell_own[cy][cx] = 1;
    }
  DilateCells(g_cell_small, g_cell_own, 2);
  DilateCells(g_cell_near1, g_cell_own, 1);
  // l'anello: i pixel nelle caselle dell'isoletta e quelli attaccati fino a 2
  // pixel (non tutto a una casella: la fascia nera sotto la scala accanto a un
  // palo restava una macchia scura; non di piu': in alcune fasi
  // dell'animazione l'anello tocca quella fascia)
  IslandCells(w, h, cam_x, cam_y, ext, 0);
  for (int step = 0; step < 2; step++) {
    IslandCells(w, h, cam_x, cam_y, ext, 1);
    IslandCells(w, h, cam_x, cam_y, ext, 2);
  }
}

// quanto il pixel e' vicino a un'isoletta (0-1), interpolato fra le
// caselle: lo spessore della schiuma cambia senza scalini
static float SmallAmount(int x, int y, int cam_x, int cam_y, int ext) {
  int wx = x - ext + cam_x - 4, wy = y + cam_y - 4;
  int bx = ((cam_x - ext) & ~7) >> 3, by = (cam_y & ~7) >> 3;
  int c0x = (wx >> 3) - bx + kCellMargin, c0y = (wy >> 3) - by + kCellMargin;
  float fx = (wx & 7) * (1 / 8.0f), fy = (wy & 7) * (1 / 8.0f), v[2][2];
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      int cx = c0x + i, cy = c0y + j;
      v[j][i] = cx >= 0 && cy >= 0 && cx < kMaxCellsX && cy < kMaxCellsY && g_cell_small[cy][cx] ? 1.0f : 0.0f;
    }
  return (v[0][0] * (1 - fx) + v[0][1] * fx) * (1 - fy) + (v[1][0] * (1 - fx) + v[1][1] * fx) * fy;
}

// il colore piu' scuro della casella animata (la base nera della scogliera)
static uint32 AnimCellDarkest(int a) {
  int tile = a & 0x1f, pal = (a >> 10) & 7, best = 1 << 30;
  uint32 res = 0;
  uint16 seen = 0;
  for (int ph = 0; ph < 3; ph++)
    for (int py = 0; py < 8; py++) {
      const uint8 *p = &g_ram[0xa680 + ph * 0x400 + tile * 32];
      for (int s = 0; s < 8; s++) {
        int v = (p[py * 2] >> s & 1) | (p[py * 2 + 1] >> s & 1) << 1 | (p[16 + py * 2] >> s & 1) << 2 |
                (p[17 + py * 2] >> s & 1) << 3;
        if (!v || (seen >> v & 1))
          continue;
        seen |= 1 << v;
        uint16 c15 = main_palette_buffer[pal * 16 + v];
        int r = (c15 & 31) << 3, g = (c15 >> 5 & 31) << 3, b = (c15 >> 10 & 31) << 3;
        if (r + g + b < best)
          best = r + g + b, res = (uint32)(r * g_bri / 15) << 16 | (uint32)(g * g_bri / 15) << 8 | (uint32)(b * g_bri / 15);
      }
    }
  return res;
}

// conta come riva solo una casella animata a 2 caselle al massimo da una
// casella animata d'acqua (i fiori lontani no); poi le maschere per casella
// usate da DarkShoreToWater
static void MarkDarkShoreCells(void) {
  static uint8 anim[kMaxCellsY][kMaxCellsX], near_anim[kMaxCellsY][kMaxCellsX];
  for (int cy = 0; cy < kMaxCellsY; cy++)
    for (int cx = 0; cx < kMaxCellsX; cx++)
      anim[cy][cx] = g_cell_anim[cy][cx] >= 0;
  DilateCells(near_anim, anim, 2);
  // il colore piu' scuro, una volta per casella animata diversa
  static uint32 cache[256];
  static uint8 known[256];
  memset(known, 0, sizeof(known));
  for (int cy = 0; cy < kMaxCellsY; cy++)
    for (int cx = 0; cx < kMaxCellsX; cx++) {
      if (!near_anim[cy][cx])
        g_cell_shore[cy][cx] = -1;
      int a = g_cell_shore[cy][cx];
      if (a < 0)
        continue;
      int key = (a & 0x1f) | ((a >> 10) & 7) << 5;
      if (!known[key])
        cache[key] = AnimCellDarkest(a), known[key] = 1;
      g_cell_dark[cy][cx] = cache[key];
    }
  for (int cy = 0; cy < kMaxCellsY; cy++)
    for (int cx = 0; cx < kMaxCellsX; cx++)
      g_cell_border[cy][cx] = g_cell_shore[cy][cx] >= 0 || (cy > 0 && g_cell_shore[cy - 1][cx] >= 0) ||
                              (cy < kMaxCellsY - 1 && g_cell_shore[cy + 1][cx] >= 0) ||
                              (cx > 0 && g_cell_shore[cy][cx - 1] >= 0) || (cx < kMaxCellsX - 1 && g_cell_shore[cy][cx + 1] >= 0);
}

// Nel Mondo Chiaro la fascia delle caselle animate della riva diventa acqua
// fino alla roccia (i suoi colori sono vicini a quelli dell'acqua) e la
// schiuma nasce li'. Nel Mondo Oscuro la fascia e' nera con le linee verdi e
// restava riva: l'acqua finiva 14 pixel prima e la schiuma era su un altro
// bordo. Qui la base nera (il colore piu' scuro delle caselle animate
// d'acqua) e i verdi (linee, archi, fascia oliva) della riva diventano acqua
// col colore dell'acqua (g_fixed); la roccia (marrone, grigio scuro) resta.
// Verde chiaro e nero fino a una casella piu' in la' della riva, l'oliva solo
// a 2 pixel. L'anello verde dei pali resta quello del gioco originale
// (richiesta dell'utente): i suoi pixel non arrivano qui (g_kind 0x43)
static void DarkShoreRows(void *ctx, int part, int y0, int y1) {
  const DarkJob *j = (const DarkJob *)ctx;
  int w = j->w, cam_x = j->cam_x, cam_y = j->cam_y, ext = j->ext;
  uint32 band = j->band;
  for (int y = y0; y < y1; y++)
    for (int x = 0; x < w; x++) {
      int i = y * w + x;
      if (g_kind[i] != 3)
        continue;
      uint32 c = g_pre[i];
      int r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
      bool green = g >= r + 8 && g >= b;
      bool black = j->have_band && abs(r - (int)(band >> 16 & 0xff)) <= 6 && abs(g - (int)(band >> 8 & 0xff)) <= 6 &&
                   abs(b - (int)(band & 0xff)) <= 6;
      if (!green && !black)
        continue;  // la terra si scarta qui, senza guardare le caselle
      int cy, cx = CellXY(x, y, cam_x, cam_y, ext, &cy);
      if (cx < 0 || cy < 0 || cx >= kMaxCellsX || cy >= kMaxCellsY)
        continue;
      bool border = g_cell_border[cy][cx];
      if (black || g >= r + 40) {
        if (!border)
          continue;
      } else if (ShoreCellOf(x, y, cam_x, cam_y, ext) < 0 && ShoreCellOf(x, y + 2, cam_x, cam_y, ext) < 0 &&
                 ShoreCellOf(x, y - 2, cam_x, cam_y, ext) < 0 && ShoreCellOf(x + 2, y, cam_x, cam_y, ext) < 0 &&
                 ShoreCellOf(x - 2, y, cam_x, cam_y, ext) < 0) {
        continue;
      }
      g_kind[i] = 1, g_fixed[i] = 1, g_depth[i] = 0;
    }
}

// le onde chiare dell'acqua bassa contro la sabbia: nel Mondo Chiaro sono
// azzurre e diventano acqua (GrowBluishWater), qui sono verdi e restavano
// riva, una colonna chiara fra la sabbia e la schiuma. I pixel verdi del
// layer dell'acqua attaccati all'acqua, fino a 3 pixel, sono acqua
// mode 0: segna (4), mode 1: 4 -> acqua
static void GrowGreenRows(void *ctx, int part, int y0, int y1) {
  const DarkJob *j = (const DarkJob *)ctx;
  int w = j->w, h = j->h;
  for (int y = y0; y < y1; y++)
    for (int x = 0; x < w; x++) {
      int i = y * w + x;
      if (j->mode == 1) {
        if (g_kind[i] == 4)
          g_kind[i] = 1, g_depth[i] = 0;
        continue;
      }
      if (g_kind[i] != 3 || (g_pre[i] >> 24) != kLayerWater)
        continue;
      uint32 c = g_pre[i];
      int r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
      if (g < r + 8 || g < b)
        continue;
      if ((x > 0 && g_kind[i - 1] == 1) || (x < w - 1 && g_kind[i + 1] == 1) ||
          (y > 0 && g_kind[i - w] == 1) || (y < h - 1 && g_kind[i + w] == 1))
        g_kind[i] = 4;
    }
}

static void DarkShoreToWater(int w, int h, int cam_x, int cam_y, int ext) {
  // il nero della fascia: il piu' scuro fra le caselle animate d'acqua
  // (alcune sono tutte acqua, senza la base nera); senza caselle animate
  // nella vista quello dell'ultima volta (in mezzo al lago coi pali)
  static uint32 last_band;
  static bool have_last;
  uint32 band = last_band;
  bool have_band = have_last;
  int band_sum = 1 << 30;
  for (int cy = 0; cy < kMaxCellsY; cy++)
    for (int cx = 0; cx < kMaxCellsX; cx++) {
      if (g_cell_anim[cy][cx] < 0 || g_cell_shore[cy][cx] < 0)
        continue;
      uint32 d = g_cell_dark[cy][cx];
      int s = (d >> 16 & 0xff) + (d >> 8 & 0xff) + (d & 0xff);
      if (s < band_sum)
        band_sum = s, band = d, have_band = true;
    }
  last_band = band, have_last = have_band;
  DarkJob j = { w, h, cam_x, cam_y, ext, 0, band, have_band };
  HdPar_For(DarkShoreRows, &j, h);
  for (int step = 0; step < 3; step++) {
    j.mode = 0;
    HdPar_For(GrowGreenRows, &j, h);
    j.mode = 1;
    HdPar_For(GrowGreenRows, &j, h);
  }
}

// La riva vera, pixel per pixel (i due passi di HdWater_Apply)
typedef struct ShoreJob {
  int pass, reach, tol;
  bool outdoors;
  int w, cam_x, cam_y, ext;
} ShoreJob;

// Nei labirinti: il pixel e' in una casella di pavimento normale (attributo
// 0x00 sul pavimento). Li' la riva si cerca solo vicino e con i colori
// dell'acqua: nel Palazzo del Ghiaccio (stanza 0x120) i puntini azzurri del
// corridoio largo 32 pixel fra due vasche diventavano acqua con i bagliori
static bool IsPlainFloorPixel(const ShoreJob *j, int i) {
  int wx = i % j->w - j->ext + j->cam_x, wy = i / j->w + j->cam_y;
  int pos = ((wy & 0x1f8) << 3) + ((wx & 0x1f8) >> 3);
  return HdWater_DungAttr(pos, wx, wy) == 0x00;
}

static void ShorePixels(void *ctx, int part, int i0, int i1) {
  const ShoreJob *j = (const ShoreJob *)ctx;
  bool outdoors = j->outdoors;
  if (j->pass == 0) {
    for (int i = i0; i < i1; i++)
      if (g_kind[i] == 1 && !g_fixed[i] && g_dist[i] <= 4 * kDistUnit && !IsNearWaterColor(g_pre[i], 24) &&
          !(outdoors && IsBluish(g_pre[i])))
        g_kind[i] = 3;
  } else {
    for (int i = i0; i < i1; i++)
      if (g_kind[i] == 3 && g_dist[i] <= j->reach &&
          (IsNearWaterColor(g_pre[i], j->tol) || outdoors && IsBluish(g_pre[i]))) {
        if (!outdoors && j->tol && IsPlainFloorPixel(j, i) &&
            (g_dist[i] > 12 * kDistUnit || !IsNearWaterColor(g_pre[i], 0)))
          continue;
        g_kind[i] = 1, g_depth[i] = 0;
      }
  }
}

// La base scura delle scogliere (Mondo Oscuro) sta dentro le caselle d'acqua
// ed e' larga fino a 12-16 pixel: la riva pixel per pixel (ShorePixels) ne
// prendeva solo i primi 4 e il resto diventava acqua, fino ai sassi, o restava
// a pezzetti. Dalla riva gia' trovata si va avanti nei pixel d'acqua con un
// colore ben diverso da quelli dell'acqua (e non azzurri, come la fascia scura
// lungo la riva del Mondo Chiaro). g_sum2 fa da coda e g_dist dal passo
// fatto: si riempiono di nuovo dopo
static void GrowShore(int w, int h) {
  enum { kMaxSteps = 16, kTol = 48 };
  uint32 *q = g_sum2;
  int top = 0, n = w * h;
  for (int i = 0; i < n; i++) {
    if (g_kind[i] != 3)
      continue;
    int x = i % w;
    int nb[4] = { x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, i >= w ? i - w : -1, i + w < n ? i + w : -1 };
    for (int k = 0; k < 4; k++) {
      int r = nb[k];
      if (r < 0 || g_kind[r] != 1 || g_fixed[r] || IsNearWaterColor(g_pre[r], kTol) || IsBluish(g_pre[r]))
        continue;
      g_kind[r] = 3, g_dist[r] = 1;
      q[top++] = r;
    }
  }
  for (int head = 0; head < top; head++) {
    int p = q[head], x = p % w;
    if (g_dist[p] >= kMaxSteps)
      continue;
    int nb[4] = { x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, p >= w ? p - w : -1, p + w < n ? p + w : -1 };
    for (int k = 0; k < 4; k++) {
      int r = nb[k];
      if (r < 0 || g_kind[r] != 1 || g_fixed[r] || IsNearWaterColor(g_pre[r], kTol) || IsBluish(g_pre[r]))
        continue;
      g_kind[r] = 3, g_dist[r] = g_dist[p] + 1;
      q[top++] = r;
    }
  }
}

// Mondo Oscuro: la fila in fondo alla cascata verde ha una linea scura con dei
// puntini, piu' lunga di un'isoletta: restava riva e faceva dei buchi nella
// schiuma bianca alla base. Un pixel di riva con la cascata subito sopra
// (fino a 3 pixel) e l'acqua subito sotto (fino a 3 pixel) e' acqua
// in parallelo per righe: prima si segna (9) e poi si fa acqua, cosi' ogni
// pixel guarda i tipi di prima come quando si andava in ordine
typedef struct FallBaseJob {
  int w, h, mode;
} FallBaseJob;

static void FallBaseRows(void *ctx, int part, int y0, int y1) {
  const FallBaseJob *j = (const FallBaseJob *)ctx;
  int w = j->w, h = j->h;
  for (int y = y0 < 1 ? 1 : y0; y < y1 && y < h - 1; y++)
    for (int x = 0; x < w; x++) {
      int i = y * w + x;
      if (j->mode == 1) {
        if (g_kind[i] == 9)
          g_kind[i] = 1, g_depth[i] = 0;
        continue;
      }
      if (g_kind[i] != 3)
        continue;
      bool up = false, down = false;
      for (int d = 1; d <= 3; d++) {
        if (y - d >= 0 && g_kind[i - d * w] == 2) up = true;
        if (y + d < h && g_kind[i + d * w] == 1) down = true;
      }
      if (up && down)
        g_kind[i] = 9;
    }
}

static void FillFallBase(int w, int h) {
  FallBaseJob j = { w, h, 0 };
  HdPar_For(FallBaseRows, &j, h);
  j.mode = 1;
  HdPar_For(FallBaseRows, &j, h);
}

// L'acqua azzurra attaccata all'acqua (fino a 40 pixel di cammino) e' acqua:
// in mezzo ai gruppi di sassi (fiume a est del castello) ci sono pezzi d'acqua
// dentro caselle di sasso a piu' di 12 pixel dalle caselle d'acqua.
// g_sum2 fa da coda e g_dist dal passo fatto: si riempiono di nuovo dopo
static void GrowBluishWater(int w, int h) {
  enum { kMaxSteps = 40 };
  uint32 *q = g_sum2;
  int top = 0, n = w * h;
  for (int i = 0; i < n; i++) {
    if (g_kind[i] != 3 || !IsBluish(g_pre[i]))
      continue;
    int x = i % w;
    if ((x > 0 && g_kind[i - 1] == 1) || (x < w - 1 && g_kind[i + 1] == 1) ||
        (i >= w && g_kind[i - w] == 1) || (i + w < n && g_kind[i + w] == 1))
      q[top++] = i, g_dist[i] = 1;
  }
  for (int j = 0; j < top; j++)
    g_kind[q[j]] = 1, g_depth[q[j]] = 0;
  for (int head = 0; head < top; head++) {
    int p = q[head], x = p % w;
    if (g_dist[p] >= kMaxSteps)
      continue;
    int nb[4] = { x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, p >= w ? p - w : -1, p + w < n ? p + w : -1 };
    for (int k = 0; k < 4; k++) {
      int r = nb[k];
      if (r < 0 || g_kind[r] != 3 || !IsBluish(g_pre[r]))
        continue;
      g_kind[r] = 1, g_depth[r] = 0, g_dist[r] = g_dist[p] + 1;
      q[top++] = r;
    }
  }
}

// I pixel d'acqua di una striscia di righe
// Corrente (Palazzo della Palude, stanza 0x16): sul piano di sopra le caselle
// d'acqua 0x1b0/0x1b1 con la palette 7 sono animate dal gioco come acqua che
// scorre verso sinistra (a destra se ribaltate). -1 sinistra, 1 destra, 0 ferma
static int FlowAt(int wx, int wy) {
  if (!InLoadedRoom(wx, wy))
    return 0;
  uint16 e = dung_bg2[0x1000 + ((wy & 0x1f8) << 3) + ((wx & 0x1f8) >> 3)];
  if ((e >> 10 & 7) != 7 || ((e & 0x3ff) != 0x1b0 && (e & 0x3ff) != 0x1b1))
    return 0;
  return e & 0x4000 ? 1 : -1;
}

typedef struct PaintJob {
  uint8 *pixels;
  int pitch, w, h, cam_x, cam_y, ext;
  float t;
  bool outdoors, see_floor, dark_shore;
} PaintJob;

// Il pavimento (BG2) nel punto del mondo, come lo disegnerebbe la PPU senza
// l'acqua del piano di sopra: casella di dung_bg2, grafica dalla VRAM
// Le caselle animate dell'acqua (0x1b0-0x1bf con la palette 7: getto e schiuma
// sotto le bocche) non sono pavimento: i loro archi SNES traspariva sotto
// l'acqua HD. Li' niente fondo (kNoFloor)
enum { kNoFloor = 0xffffffff };
static uint32 FloorColor(int wx, int wy) {
  Ppu *ppu = g_zenv.ppu;
  uint16 e = dung_bg2[((wy & 0x1f8) << 3) + ((wx & 0x1f8) >> 3)];
  if ((e >> 10 & 7) == 7 && (e & 0x3f0) == 0x1b0)
    return kNoFloor;
  int r = e & 0x8000 ? 7 - (wy & 7) : wy & 7, bit = e & 0x4000 ? wx & 7 : 7 - (wx & 7);
  int adr = ppu->bgLayer[1].tileAdr + (e & 0x3ff) * 16 + r;
  uint16 p01 = ppu->vram[adr & 0x7fff], p23 = ppu->vram[(adr + 8) & 0x7fff];
  int v = (p01 >> bit & 1) | (p01 >> (8 + bit) & 1) << 1 | (p23 >> bit & 1) << 2 | (p23 >> (8 + bit) & 1) << 3;
  uint16 col = ppu->cgram[v ? (e >> 10 & 7) * 16 + v : 0];
  uint32 cr = col & 31, cg = col >> 5 & 31, cb = col >> 10 & 31, br = INIDISP_copy & 15;
  cr = (cr << 3 | cr >> 2) * br / 15, cg = (cg << 3 | cg >> 2) * br / 15, cb = (cb << 3 | cb >> 2) * br / 15;
  return cr << 16 | cg << 8 | cb;
}

static void PaintWaterRows(void *ctx, int part, int y0, int y1) {
  const PaintJob *j = (const PaintJob *)ctx;
  uint8 *pixels = j->pixels;
  int pitch = j->pitch, w = j->w, h = j->h, cam_x = j->cam_x, cam_y = j->cam_y, ext = j->ext;
  float t = j->t;
  bool see_floor = j->see_floor, dark_shore = j->dark_shore;
  for (int y = y0; y < y1; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    const uint32 *pre = g_pre + y * w;
    float wy = (float)(y + cam_y);
    for (int x = 0; x < w; x++) {
      int i = y * w + x;
      if (g_kind[i] != 1)
        continue;
      const uint32 *ps = &g_sum2[i * kSumCh];
      // r, g, b, quanti, quanti bassi, quanti con profondita' nota
      int s[6] = { ps[0] & 0xffff, ps[0] >> 16, ps[1] & 0xffff, ps[1] >> 16 & 0xff, ps[1] >> 24, ps[2] };
      float inv = 1.0f / (s[3] ? s[3] : 1);
      // i colori a piena luce: alla fine si riscuriscono (g_bright)
      float ib = inv / g_bright;
      float br = s[0] * ib, bg = s[1] * ib, bb = s[2] * ib;
      float wx = (float)(x - ext + cam_x);
      // nella corrente il disegno della superficie scorre (0.5 pixel per frame)
      int flow = j->outdoors ? 0 : FlowAt((int)wx, (int)wy);
      float px = wx;
      if (flow)
        wx -= flow * t * 0.5f;
      float d = (float)g_dist[i] / kDistUnit;
      // quanta acqua c'e' nel quadrato della media (225 pixel), contando come
      // acqua la parte fuori dallo schermo: ai bordi dell'immagine il quadrato
      // e' tagliato e l'acqua sembrava poca (righe delle fontane sui bordi)
      int qx = (x + kBlurRadius < w ? x + kBlurRadius : w - 1) - (x - kBlurRadius > 0 ? x - kBlurRadius : 0) + 1;
      int qy = (y + kBlurRadius < h ? y + kBlurRadius : h - 1) - (y - kBlurRadius > 0 ? y - kBlurRadius : 0) + 1;
      float amount = (float)s[3] + (float)((2 * kBlurRadius + 1) * (2 * kBlurRadius + 1) - qx * qy);
      float deep = Smooth(0, 26, d);
      // Acqua bassa (0x09) e alta (0x08) ben distinguibili, con un passaggio
      // morbido (media sul quadrato di 15 pixel). I colori restano quelli
      // dell'acqua originale (azzurro e blu in Hyrule, verde chiaro e scuro
      // nel Mondo Oscuro): l'alta un po' piu' scura al largo, la bassa quasi
      // uguale, e riflessi, macchie e schiuma prendono la tinta dell'acqua
      // invece di un azzurro fisso (prima l'acqua era piu' chiara e la palude
      // del Mondo Oscuro diventava verde menta).
      // Dove non si sa (acqua delle caselle di riva) vale la media dei vicini
      float sf = s[5] ? (float)s[4] / s[5] : 0;
      float shallow = Smooth(0.2f, 0.8f, sf);
      float dk = 1.02f - 0.14f * deep;
      float r = br * (dk + (1.04f - dk) * shallow), g = bg * (dk + (1.04f - dk) * shallow), b = bb * (dk + (1.04f - dk) * shallow);
      // tinta chiara dell'acqua: verso il bianco ma con il suo colore
      float lr = br + (255 - br) * 0.55f, lg = bg + (255 - bg) * 0.55f, lb = bb + (255 - bb) * 0.55f;
      if (shallow > 0) {
        float c1 = 1 - fabsf(Noise(wx * (1 / 7.0f) + t * 0.012f, wy * (1 / 7.0f) - t * 0.008f) * 2 - 1);
        float c2 = 1 - fabsf(Noise(wx * (1 / 5.0f) - t * 0.010f + 31, wy * (1 / 5.0f) + t * 0.009f) * 2 - 1);
        float caust = (Smooth(0.80f, 0.97f, c1) * 0.6f + Smooth(0.82f, 0.98f, c2) * 0.4f) * shallow * 0.22f;
        r += (lr - r) * caust, g += (lg - g) * caust, b += (lb - b) * caust;
      }
      // dove il fondale scende: una linea scura morbida
      float edge = 1 - fabsf(sf - 0.5f) * 2;
      if (s[5] && edge > 0.5f) {
        float e = 1 - 0.16f * Smooth(0.5f, 1, edge);
        r *= e, g *= e, b *= e;
      }
      // macchie chiare che scorrono piano (come i riflessi del cielo di Four Swords)
      float n = Noise(wx * (1 / 46.0f) + t * 0.0040f, wy * (1 / 46.0f) + t * 0.0025f) * 0.65f +
                Noise(wx * (1 / 17.0f) - t * 0.0060f, wy * (1 / 17.0f) + t * 0.0015f) * 0.35f;
      float patch = Smooth(0.52f, 0.64f, n) * (0.35f + 0.65f * deep) * (1 - shallow);
      r += br * 0.10f * patch, g += bg * 0.10f * patch, b += bb * 0.10f * patch;
      // increspature leggere
      float sh = sinf(wx * 0.31f + wy * 0.13f + t * 0.07f + 6.0f * Noise(wx * 0.11f, wy * 0.11f + t * 0.01f)) * 2;
      r += sh, g += sh, b += sh;
      // corrente: strisce chiare allungate che vanno con l'acqua
      if (flow) {
        float sn = Noise(wx * (1 / 22.0f), wy * (1 / 3.5f)) * 0.7f + Noise(wx * (1 / 9.0f), wy * (1 / 2.5f) + 17) * 0.3f;
        float st = Smooth(0.55f, 0.70f, sn) * 0.38f;
        r += (lr - r) * st, g += (lg - g) * st, b += (lb - b) * st;
      }
      // nei labirinti l'acqua profonda e' trasparente come nel gioco originale:
      // si vede il fondo, il pavimento senza l'acqua del gioco (FloorColor).
      // Quanta e' profonda nel quadrato della media, cosi' non c'e' uno scalino
      if (see_floor && s[5]) {
        // un po' scura: il fondo si intravede (meno limpida)
        float deepf = Clamp01((float)(s[5] - s[4]) * inv), a = 0.35f * deepf, dark = 1 - 0.18f * deepf;
        uint32 c = FloorColor((int)px, (int)wy);
        float fb = 1.0f / g_bright;
        if (c != kNoFloor)
          r += ((c >> 16 & 0xff) * fb - r) * a, g += ((c >> 8 & 0xff) * fb - g) * a, b += ((c & 0xff) * fb - b) * a;
        r *= dark, g *= dark, b *= dark;
      }
      // negli specchi d'acqua piccoli (fontane delle statue) le macchie non si
      // vedono: righe chiare che scorrono, come l'acqua che si muove
      float small = 1 - Smooth(25, 60, amount);
      if (small > 0) {
        float fl = sinf(wy * 0.9f - t * 0.20f + wx * 0.35f + Noise(wx * 0.25f, wy * 0.25f - t * 0.02f) * 3);
        float hl = Smooth(0.35f, 0.9f, fl) * small;
        r += (lr - r) * hl * 0.7f, g += (lg - g) * hl * 0.7f, b += (lb - b) * hl * 0.7f;
      }
      // schiuma: fascia vicino alla riva e increspature che se ne allontanano
      // (oltre 15 pixel dalla riva fascia e increspature sono zero: al largo
      // non si calcolano, e' la maggior parte dell'acqua)
      // meno schiuma sull'acqua marrone e su quella verde delle paludi (nel
      // Mondo Oscuro le piante nell'acqua sono tante: la schiuma attorno a
      // ognuna schiariva tutta la palude), solo un filo chiaro sulla verde
      float blue = Clamp01(fminf((bb - br) / 70 + 0.35f, (bb - bg) / 60 + 0.6f));
      float green = Clamp01((bg - bb) / 40);
      if (blue < 0.3f * green)
        blue = 0.3f * green;
      // Mondo Oscuro vicino alla riva nera della scogliera: la stessa schiuma
      // del Mondo Chiaro, ma nera e piena (le piante nell'acqua restano col
      // filo chiaro)
      // nel Mondo Oscuro all'aperto la schiuma e' sempre nera, come quella
      // bianca del Mondo Chiaro (prima vicino ai bordi a caselle: a meta' riva
      // si passava a quella chiara)
      bool dark_foam = dark_shore && d < 15;
      float foam = 0;
      if (d < 15) {
        float nw = Noise(wx * 0.1f, wy * 0.1f);
        // la schiuma nera della costa (riva, sabbia) e' larga 1,2 volte,
        // quella attorno ai pali normale (richiesta dell'utente)
        float fs = dark_foam ? 1.2f - 0.2f * SmallAmount(x, y, cam_x, cam_y, ext) : 1.0f;
        float fw = (2.0f + 0.9f * sinf(t * 0.045f + nw * 6.283f)) * fs;
        foam = (1 - Smooth(fw - 1.0f * fs, fw + 1.3f * fs, d)) * 0.85f;
        float ph = t * 0.012f + Noise(wx * (1 / 22.0f), wy * (1 / 22.0f)) * 2;
        ph -= floorf(ph);
        float rr = fw + 1.5f + ph * 8, dd = d - rr;
        // al chiuso niente increspature: nelle vasche strette si spezzavano in
        // archi chiari che sembravano le onde dello SNES (sotto le bocche)
        if (j->outdoors && dd > -4 && dd < 4) {
          float ring = (1 - ph) * expf(-dd * dd * 0.9f) * 0.45f *
                       Smooth(0.30f, 0.60f, Noise(wx * 0.2f + t * 0.01f, wy * 0.2f));
          if (ring > foam) foam = ring;
        }
      }
      // poca schiuma negli specchi d'acqua piccoli (l'acqua delle statue):
      // amount e' quanta acqua c'e' nel quadrato della media (225 pixel)
      foam *= (dark_foam ? 1.0f : blue) * (0.2f + 0.8f * Smooth(50, 150, amount));
      if (g_night)
        foam *= 0.45f;  // di notte la schiuma non spicca bianca sull'acqua scura
      // schiuma chiara con la tinta dell'acqua (bianco azzurro in Hyrule,
      // bianco verde nel Mondo Oscuro), quella nera appena con la tinta
      float fr = br + (255 - br) * 0.80f, fg = bg + (255 - bg) * 0.80f, fb = bb + (255 - bb) * 0.80f;
      if (dark_foam)
        fr = br * 0.12f + 6, fg = bg * 0.12f + 2, fb = bb * 0.12f;
      r += (fr - r) * foam, g += (fg - g) * foam, b += (fb - b) * foam;
      float k[3];
      LightRatio(row[x], pre[x], k);
      row[x] = LitColor(r, g, b, k);
    }
  }
}

// Luce sulle cascate (torce, stanze buie): il rapporto fra l'immagine
// illuminata e quella di prima, medio in un quadrato di 7 pixel della
// cascata. Pixel per pixel le luci che si sommano facevano risaltare il
// disegno del getto originale (strisce scure e puntini azzurri)
static float *g_fall_k;
static int g_fall_k_cap;

static bool FallLight(const uint8 *pixels, int pitch, int w, int h) {
  if (w * h > g_fall_k_cap) {
    float *k = realloc(g_fall_k, (size_t)w * h * 3 * sizeof(float));
    if (!k)
      return false;
    g_fall_k = k, g_fall_k_cap = w * h;
  }
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int i = y * w + x;
      if (g_kind[i] != 2)
        continue;
      float a[3] = { 0 }, b[3] = { 0 };
      int n = 0;
      for (int yy = y - 3; yy <= y + 3; yy++) {
        if (yy < 0 || yy >= h)
          continue;
        const uint32 *row = (const uint32 *)(pixels + yy * pitch);
        for (int xx = x - 3; xx <= x + 3; xx++) {
          int ii = yy * w + xx;
          if (xx < 0 || xx >= w || g_kind[ii] != 2)
            continue;
          uint32 lit = row[xx], pre = g_pre[ii];
          for (int c = 0; c < 3; c++)
            a[c] += (lit >> (16 - 8 * c) & 0xff) + g_light_sub, b[c] += pre >> (16 - 8 * c) & 0xff;
          n++;
        }
      }
      for (int c = 0; c < 3; c++) {
        float v = (a[c] + 6 * n) / (b[c] + 6 * n);
        if (a[c] < 6 * n && a[c] < b[c])
          v *= a[c] / (6 * n);  // nel buio la cascata resta nera (LightRatio)
        g_fall_k[i * 3 + c] = v > 4 ? 4 : v;
      }
    }
  }
  return true;
}

static uint8 g_last_frame;
static int g_mist_place = -1;  // stanza o area della nebbia
static uint32 g_time;  // frame di gioco con l'effetto attivo (anche nelle aree speciali)

void HdWater_Apply(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  int mod;
  g_light_sub = (float)g_hd_light_sub;
  if (!g_pre_valid || !InScene(&mod)) {
    g_num_mist = 0;
    return;
  }
  g_pre_valid = false;
  // cambio di stanza o di area (anche per le scale, che non spostano la
  // camera): la nebbia della stanza di prima non resta a mezz'aria
  int place = mod == 7 ? dungeon_room_index : 0x1000 | overworld_screen_index;
  // col mondo continuo le aree vicine sono gia' sullo schermo: passando da
  // un'area all'altra la nebbia (in coordinate del mondo) resta
  bool same_world = mod != 7 && g_mist_place >= 0x1000 && HdWorld_Active() &&
      ((g_mist_place ^ place) & 0x40) == 0 && overworld_screen_index < 0x80 && (g_mist_place & 0xff) < 0x80;
  if (place != g_mist_place && !same_world)
    g_num_mist = 0;
  g_mist_place = place;
  g_bri = (INIDISP_copy & 0x80) ? 0 : INIDISP_copy & 15;
  if (g_bri == 0)
    return;  // schermo nero
  g_bright = g_bri / 15.0f;
  g_night = !player_is_indoors && sram_progress_indicator < 2 && !(overworld_screen_index & 0x40);
  bool step = g_last_frame != frame_counter;
  g_last_frame = frame_counter;
  if (step)
    g_time++;
  float t = (float)g_time;
  int ext = (width - 256) / 2;
  int cam_x = BG2HOFS_copy2 + bias_x, cam_y = BG2VOFS_copy2 + bias_y;
  int w = width, h = height;
  g_vis_x0 = 0, g_vis_x1 = w, g_vis_y1 = h;
  if (!HdWorld_Active()) {
    Ppu *ppu = g_zenv.ppu;
    g_vis_x0 = ext - ppu->extraLeftCur, g_vis_x1 = ext + 256 + ppu->extraRightCur;
    g_vis_y1 = 224 + ppu->extraBottomCur;
    if (g_vis_x0 < 0) g_vis_x0 = 0;
    if (g_vis_x1 > w) g_vis_x1 = w;
    if (g_vis_y1 > h) g_vis_y1 = h;
  }

  bool outdoors = mod != 7;
  if (outdoors)
    ComputeAnimMask();
  else
    g_anim_first = -1;
  bool falls = outdoors ? ClassifyCells(width, height, cam_x, cam_y, ext) : RoomHasSpouts();
  g_small_mist = !outdoors;
  // col mondo non continuo oltre il bordo dell'area lo schermo e' nero: non
  // deve fare schiuma come una riva
  bool outside_area = mod == 9 && !HdWorld_Active() && overworld_screen_index < 0x80;
  // leva che accende l'acqua (Module07_0C, fase 10): la finestra dell'acqua
  // e' una colonna stretta che scende dalla bocca fino al fondo della vasca
  bool stream = !outdoors && main_module_index == 7 && submodule_index == 12 && subsubmodule_index == 10;
  ClassJob cj = { pixels, pitch, w, cam_x, cam_y, ext, outdoors, falls, outside_area, stream };
  HdPar_For(ClassifyRows, &cj, h);
  int n_water = 0, n_fall = 0;
  float fall_sum[3] = { 0 };
  for (int p = 0; p < kHdParMaxParts; p++) {
    n_water += cj.n_water[p], n_fall += cj.n_fall[p];
    for (int c = 0; c < 3; c++)
      fall_sum[c] += cj.fall_sum[p][c];
  }
  if (!n_water && !n_fall) {
    g_num_bases = 0;
    UpdateMist(step);
    DrawMist(pixels, pitch, width, height, cam_x, cam_y, ext, outdoors && (overworld_screen_index & 0x40));
    return;
  }
  // la riva vera, pixel per pixel: vicino all'acqua marcata (12 pixel) i
  // colori dell'acqua sono acqua anche nelle caselle di riva; nell'acqua
  // marcata i colori piu' scuri di quelli dell'acqua sono le linee della riva
  // Nei labirinti la ricerca va piu' lontano (48 pixel): l'acqua ferma delle
  // statue accanto alle vasche (grotta della fata) diventa acqua anche lei,
  // e conta anche un azzurro appena diverso (quello delle statue)
  // acqua del piano di sopra sommata in trasparenza al pavimento (diga): e'
  // acqua tutta la casella, la riva non si cerca dai colori. Il pixel
  // originale cambia con le onde animate del gioco e i cunei del cancello
  // sotto l'acqua facevano una schiuma che lampeggiava
  bool see_floor = !outdoors && (TS_copy & 1) && !(TM_copy & 1);
  bool dark_shore = outdoors && (overworld_screen_index & 0x40) && g_anim_first >= 0;
  if (dark_shore)
    MarkDarkShoreCells();
  if (n_water) {
    ComputeDistance(w, h, 2, 3, 5);
    FindWaterColors(w * h, n_water);
  }
  if (n_water && !see_floor) {
    // nelle caselle d'acqua della riva i pixel che non sono acqua (linee della
    // riva, pavimento attorno alle vasche) sono attaccati al terreno; i segni
    // d'onda in mezzo all'acqua restano acqua (prima facevano un bordino di
    // schiuma dove non c'e' niente)
    // solo nel Mondo Oscuro: nel Mondo Chiaro la base nera ondulata delle
    // scogliere resta coperta dall'acqua, come prima
    if (outdoors && (overworld_screen_index & 0x40))
      AnimShorePixels(w, h, cam_x, cam_y, ext);
    ShoreJob sj = { 0, 0, 0, outdoors, w, cam_x, cam_y, ext };
    HdPar_For(ShorePixels, &sj, w * h);
    if (outdoors)
      GrowShore(w, h);
    // nella stanza della diga pareti, pilastri e muri hanno colori vicini a
    // quelli dell'acqua: la riva si cerca solo vicino e con i colori giusti
    bool near = outdoors || FloodRoom();
    int reach_px = near ? 12 : 48;
    ComputeDistance(w, h, 1, 1, reach_px + 1);
    sj.pass = 1, sj.reach = reach_px * kDistUnit, sj.tol = near ? 0 : 48;
    HdPar_For(ShorePixels, &sj, w * h);
    if (outdoors)
      GrowBluishWater(w, h);
    // nella stanza della diga i puntini del disegno sotto l'acqua facevano
    // tanti scogli di pochi pixel con la schiuma attorno
    if (near)
      FillSmallIslands(w, h);
    // senza cascate (g_kind 2) non c'e' niente da riempire
    if (outdoors && (overworld_screen_index & 0x40) && n_fall)
      FillFallBase(w, h);
    if (dark_shore) {
      MarkSmallIslands(w, h, cam_x, cam_y, ext);
      DarkShoreToWater(w, h, cam_x, cam_y, ext);
      for (int i = 0; i < w * h; i++)
        g_kind[i] &= ~0x40;
    }
  }
  // fino a 27 pixel: piu' in la' l'acqua e' gia' "al largo" (deep = 1)
  ComputeDistance(w, h, 2, 3, 27);
  BlurWater(w, h, kBlurRadius);

  // 1 e 2: acqua
  PaintJob pj = { pixels, pitch, w, h, cam_x, cam_y, ext, t, outdoors, see_floor, dark_shore };
  HdPar_For(PaintWaterRows, &pj, h);

  // 3: cascate
  g_num_bases = 0;
  bool fall_light = n_fall && FallLight(pixels, pitch, w, h);
  if (n_fall) {
    float fk = 1.0f / (n_fall * g_bright);
    float fr = fall_sum[0] * fk * 0.72f, fg = fall_sum[1] * fk * 0.78f, fb = fall_sum[2] * fk * 0.92f;
    for (int x = 0; x < w; x++) {
      int top = -1;
      for (int y = 0; y <= h; y++) {
        bool in = y < h && (g_kind[y * w + x] == 2 || g_kind[y * w + x] == 6);
        if (in && top < 0)
          top = y;
        if (in || top < 0)
          continue;
        // colonna di cascata da top a y-1
        int bottom = y - 1;
        float wx = (float)(x - ext + cam_x);
        float speed = 2.2f + Rnd((int)wx >> 1, 77) * 1.2f;
        // il piede vero: sotto la colonna c'e' acqua. Ai bordi diagonali la
        // colonna finisce contro la roccia: niente schiuma ne' nebbia (prima
        // la nebbia copriva la roccia accanto alla cima, dietro i cuori)
        bool foot = bottom < h - 1 && g_kind[(bottom + 1) * w + x] == 1;
        for (int yy = top; yy <= bottom; yy++) {
          int i = yy * w + x;
          if (g_kind[i] != 2)
            continue;  // coperto da HUD o sprite
          uint32 *p = (uint32 *)(pixels + yy * pitch) + x;
          float wy = (float)(yy + cam_y);
          float v = wy - t * speed;
          float s1 = Noise(wx * (1 / 2.3f), v * (1 / 15.0f)), s2 = Noise(wx * (1 / 5.0f) + 13, v * (1 / 34.0f));
          float st = Smooth(0.45f, 0.85f, s1 * 0.6f + s2 * 0.4f);
          // ai lati della colonna la cascata e' piu' scura (tonda)
          int l = 0, rgt = 0;
          while (l < 6 && x - l - 1 >= 0 && (g_kind[i - l - 1] == 2 || g_kind[i - l - 1] == 6)) l++;
          while (rgt < 6 && x + rgt + 1 < w && (g_kind[i + rgt + 1] == 2 || g_kind[i + rgt + 1] == 6)) rgt++;
          float side = 0.72f + 0.28f * Smooth(0, 6, (float)(l < rgt ? l : rgt));
          float r = fr * side, g = fg * side, b = fb * side;
          float a = st * 0.75f;
          // nel Mondo Oscuro le cascate sono nere e verdi: striature, bordo
          // della cima e schiuma vanno verso il nero (come la schiuma della riva)
          float wr = 228, wg = 240, wb = 255;
          if (dark_shore)
            wr = r * 0.12f + 6, wg = g * 0.12f + 2, wb = b * 0.12f;
          r += (wr - r) * a, g += (wg - g) * a, b += (wb - b) * a;
          // bordo chiaro nella cima vera (caselle 0x1ca-0x1cb), non sul bordo
          // dello schermo o dell'area (Cascata dei Desideri); schiuma in fondo
          float fby = (float)(bottom - yy);
          int lip_top = outdoors ? LipTop(x, yy, cam_x, cam_y, ext) : -1;
          if (lip_top >= 0) {
            float lip = 1 - Smooth(0, 6, wy - lip_top);
            if (dark_shore)
              r += (r * 0.12f + 6 - r) * lip * 0.7f, g += (g * 0.12f + 2 - g) * lip * 0.7f, b += (b * 0.12f - b) * lip * 0.7f;
            else
              r += (245 - r) * lip * 0.7f, g += (250 - g) * lip * 0.7f, b += (255 - b) * lip * 0.7f;
          }
          if (foot) {
            float f = (1 - Smooth(0, 7, fby)) * (0.55f + 0.45f * Noise(wx * 0.3f, wy * 0.3f - t * 0.1f));
            if (dark_shore)
              r += (r * 0.12f + 6 - r) * f, g += (g * 0.12f + 2 - g) * f, b += (b * 0.12f - b) * f;
            else
              r += (240 - r) * f, g += (246 - g) * f, b += (255 - b) * f;
          }
          float kk[3];
          const float *k = fall_light ? &g_fall_k[i * 3] : kk;
          if (!fall_light)
            LightRatio(*p, g_pre[i], kk);
          *p = LitColor(r, g, b, k);
        }
        // base della cascata: punto della nebbia, ogni 4 colonne
        if (foot && ((int)wx & 3) == 0 && g_num_bases < kMaxBases) {
          g_base_x[g_num_bases] = (int)wx, g_base_y[g_num_bases] = bottom + cam_y;
          g_num_bases++;
        }
        top = -1;
      }
    }
  }

  // 4: nebbia alla base delle cascate
  UpdateMist(step);
  DrawMist(pixels, pitch, width, height, cam_x, cam_y, ext, dark_shore);
}
