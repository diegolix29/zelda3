// Illuminazione dinamica: aloni caldi attorno alle fonti di luce e stanze buie
// illuminate solo dalle fiamme accese. Lavora sull'immagine gia' disegnata dalla
// PPU (prima dello shader), senza toccare la logica di gioco.
#include <math.h>
#include <string.h>
#include "hd_light.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "snes/ppu.h"
#include "hd_fx.h"
#include "hd_burn.h"
#include "hd_trans.h"

bool g_dynamic_light;

enum { kMaxLights = 480, kMaxDecor = 24 };

// Torce a muro e bracieri decorativi trovati nella mappa della stanza
typedef struct DecorLight {
  uint16 x, y;   // centro della fiamma, in pixel dentro la stanza (0-511)
  uint8 phase;   // sfasamento della pulsazione
  int8 dir;      // verso della fiamma e delle braci: -1 sinistra, 1 destra (pareti laterali), 2 in basso (parete sud)
} DecorLight;

static DecorLight g_decor[kMaxDecor];
static int g_num_decor;
static int g_decor_room = -1;
static int g_decor_scan_timer;

const uint16 *Dungeon_GetObjectTiles(int subtype, int idx);

typedef struct Light {
  float x, y;        // centro sullo schermo (pixel del frame)
  float radius;
  float r, g, b;     // intensita' per canale
} Light;

static Light g_lights[kMaxLights];
static int g_num_lights;
// Fate del frame (centro sullo schermo e intensita'): il loro bagliore si
// aggiunge ai colori invece di moltiplicarli, cosi' si vede anche sulla fata
// stessa, chiara, e con il raggio piccolo
enum { kMaxFairies = 16 };
static float g_fairy_x[kMaxFairies], g_fairy_y[kMaxFairies], g_fairy_m[kMaxFairies];
// raggio dell'alone (9 la fata piccola, 24 la grande fata: circa 2,7 volte piu'
// grande) e meta' lato del riquadro in cui si controlla che si veda
static float g_fairy_r[kMaxFairies];
static int g_fairy_box[kMaxFairies];
static int g_num_fairies;
 // Luce ambiente delle stanze buie: come nel gioco originale la stanza resta
// leggibile anche senza Lanterna, le fiamme e la Lanterna fanno il resto
static const float kDarkAmbient = 0.36f;
static float g_ambient = 1.0f;        // luce ambiente attuale (si avvicina gradualmente all'obiettivo)
// Contrasto delle stanze buie con 1-2 torce: si sottrae g_ambient_sub (0-255) da
// ogni colore e si moltiplica un po' di piu' (vedi HdLight_Apply), cosi' i colori
// scuri scendono di piu' e quelli chiari meno, come nel gioco originale
static float g_ambient_sub = 0.0f;
static int g_ambient_place = -1;      // stanza (o -2 all'aperto) a cui si riferisce g_ambient
static bool g_dark_override;          // buio originale sospeso durante il disegno
static bool g_lit_dark_room;          // stanza buia gia' illuminata del tutto dalle torce
static uint8 g_saved_fixed[3], g_saved_sub_enabled;
static float *g_factor;               // fattori di luce per pixel (3 canali)
static int g_factor_cap;
int g_hd_light_sub;

// la scena di gioco in vista, come per gli altri effetti HD (prima qui un
// controllo a parte: luci spente nel cerchio 15/16 e nelle aree speciali 11)
static bool InGameplay(int *mod_out) {
  return HdFx_InGameplay(mod_out);
}

// Stanza buia del gioco (serve la Lanterna o le torce)
static bool IsDarkRoom(int mod) {
  return mod == 7 && dung_want_lights_out;
}

// Luce durante i passaggi fra le stanze (Module07, sottomodulo diverso da 0).
// Tre casi, ognuno con il suo stato:
// - g_fade_dark: nella dissolvenza (Module07_02, da e verso le stanze buie) il
//   gioco carica subito la stanza nuova, buio compreso, mentre quella vecchia
//   sta ancora sfumando (fasi 0-2, la 2 e' Module07_02_FadedFilter, un filtro
//   della palette). Fino al nero la stanza vecchia tiene la sua luce (prima
//   diventava nera al primo fotogramma)
// - g_dark_hold: il gioco azzera per un po' hdr_dungeon_dark_with_lantern
//   anche se la stanza e' buia (la stanza da Lanterna saliva alla penombra
//   mentre Link usciva), e entrando in una stanza da Lanterna il valore arriva
//   gia' durante la dissolvenza (compariva in penombra e diventava nera di
//   colpo): per tutto il passaggio vale il piu' buio visto
// - g_snap_pending: nella stanza nuova la luce e' subito quella giusta, anche
//   durante il passaggio, invece di salire piano come accendendo le torce
// Una sola foto della luce a stanza ferma non basta: la stanza nuova deve
// avere la sua luce prima che il passaggio finisca
static bool g_fade_dark;              // stanza buia, quella di prima durante la dissolvenza
static float g_dark_hold;             // luce minima delle stanze buie nel passaggio
static bool g_snap_pending;           // stanza nuova: luce subito giusta

static bool FadeHoldsOldRoom(void) {
  return main_module_index == 7 && submodule_index == 2 && subsubmodule_index <= 2;
}

static bool RoomLooksDark(int mod) {
  return FadeHoldsOldRoom() ? g_fade_dark : IsDarkRoom(mod);
}

// Torce accendibili della stanza. Il gioco azzera la fine dell'elenco subito
// dopo aver caricato la stanza, ma le torce restano marcate nella tabella degli
// attributi dei tile con i valori 0xC0-0xCF (0xC0 + numero della torcia): e' da
// li' che il gioco stesso capisce quale torcia si sta accendendo.
static uint16 GetTorchMask() {
  uint16 mask = 0;
  const uint8 *attr = dung_bg2_attr_table;
  for (int i = 0; i < 0x2000; i++)
    if ((attr[i] & 0xf0) == 0xc0)
      mask |= 1 << (attr[i] & 0xf);
  return mask;
}

// Quante torce ci sono e quante sono accese
static void CountTorches(int *total, int *lit) {
  uint16 mask = GetTorchMask();
  int start = dung_index_of_torches_start >> 1;
  *total = *lit = 0;
  for (int k = 0; k < 16; k++) {
    if (!(mask >> k & 1))
      continue;
    (*total)++;
    if (dung_object_tilemap_pos[start + k] & 0x8000)
      (*lit)++;
  }
}

// Forme da cercare: sottotipo e numero dell'oggetto, posizione di due tile della
// sua grafica (per evitare falsi riconoscimenti) e centro della fiamma.
typedef struct DecorShape {
  uint8 subtype, idx;    // subtype 0: si usano direttamente i tile t0/t1
  uint8 src0, src1;      // indici dei due tile nella grafica dell'oggetto
  uint8 dx1, dy1;        // posizione del secondo tile rispetto al primo
  uint8 cx, cy;          // centro della fiamma in pixel rispetto al primo tile
  uint16 t0, t1;
  bool side;             // torcia su una parete laterale (vedi DecorLight.dir)
  bool down;             // torcia sulla parete sud, fiamma rivolta in basso
} DecorShape;

static const DecorShape kDecorShapes[] = {
  { 1, 0x55, 0, 1, 1, 0, 16, 8 },   // torce a muro sulle pareti nord/sud (4x2 tile)
  { 1, 0x56, 0, 1, 1, 0, 16, 8, 0, 0, false, true },  // parete sud: caselle ribaltate, fiamma in basso
  { 1, 0x7f, 0, 4, 1, 0, 8, 16, 0, 0, true },   // torce a muro sulle pareti ovest/est (2x4 tile)
  { 1, 0x80, 0, 4, 1, 0, 8, 16, 0, 0, true },
  { 3, 0x20, 0, 2, 1, 0, 8, 8 },    // torcia sempre accesa (2x2 tile)
  // Braciere (come quelli del Santuario): 2x4 tile, fiamma nei due tile in alto
  { 0, 0, 0, 0, 1, 0, 8, 8, 0x2dc2, 0x6dc2 },
};

// Confronto dei tile ignorando palette (bit 10-12) e priorita' (bit 13): la
// stessa torcia puo' essere disegnata sopra o sotto gli sprite
#define TILE_KEY(t) ((t) & ~0x3c00)

static void AddDecor(int x, int y, int dir) {
  for (int i = 0; i < g_num_decor; i++)
    if (abs(g_decor[i].x - x) < 12 && abs(g_decor[i].y - y) < 12)
      return;  // stessa fiamma trovata in entrambi i livelli
  if (g_num_decor >= kMaxDecor)
    return;
  DecorLight *d = &g_decor[g_num_decor++];
  d->x = x, d->y = y, d->phase = (uint8)(x * 7 + y * 13), d->dir = (int8)dir;
}

// Cerca torce e bracieri decorativi nella mappa della stanza (in memoria, quindi
// funziona anche dopo aver caricato un salvataggio)
static void ScanDecorLights() {
  g_num_decor = 0;
  g_decor_room = dungeon_room_index;
  for (int si = 0; si < (int)countof(kDecorShapes); si++) {
    const DecorShape *sh = &kDecorShapes[si];
    uint16 t0 = sh->t0, t1 = sh->t1;
    if (sh->subtype) {
      const uint16 *src = Dungeon_GetObjectTiles(sh->subtype, sh->idx);
      t0 = src[sh->src0], t1 = src[sh->src1];
    }
    t0 = TILE_KEY(t0), t1 = TILE_KEY(t1);
    if (t0 == t1)
      continue;
    for (int layer = 0; layer < 2; layer++) {
      const uint16 *tm = layer ? dung_bg1 : dung_bg2;
      for (int row = 0; row < 64 - sh->dy1; row++) {
        for (int col = 0; col < 64 - sh->dx1; col++) {
          // la torcia della parete est e' quella della parete ovest ribaltata
          // (Palazzo della Palude): la fiamma e' disegnata a destra
          if (TILE_KEY(tm[row * 64 + col]) == t0 && TILE_KEY(tm[(row + sh->dy1) * 64 + col + sh->dx1]) == t1)
            AddDecor(col * 8 + sh->cx, row * 8 + sh->cy, sh->down ? 2 : !sh->side ? 0 : (tm[row * 64 + col] & 0x4000) ? 1 : -1);
        }
      }
    }
  }
}

static void AddLight(float x, float y, float radius, float r, float g, float b) {
  if (g_num_lights >= kMaxLights)
    return;
  Light *l = &g_lights[g_num_lights++];
  l->x = x, l->y = y, l->radius = radius, l->r = r, l->g = g, l->b = b;
}

// Fiamme del frame (sorgenti delle braci in hd_fx.c), in coordinate dello schermo
HdFire g_hd_fire[kHdMaxFire];
int g_hd_num_fire;

static void AddFireLightDir(float x, float y, float radius, float r, float g, float b, float strength, int dir) {
  AddLight(x, y, radius, r, g, b);
  if (g_hd_num_fire < kHdMaxFire) {
    HdFire *f = &g_hd_fire[g_hd_num_fire++];
    f->x = x, f->y = y, f->strength = strength, f->dir = (int8)dir;
  }
}

static void AddFireLight(float x, float y, float radius, float r, float g, float b, float strength) {
  AddFireLightDir(x, y, radius, r, g, b, strength, 0);
}

// Tremolio di torce, bracieri e fate: tf = tempo / 2, ph = fase propria
static inline float FlamePulse(float tf, float ph) {
  return 0.82f + 0.12f * sinf(tf * 1.6f + ph) + 0.06f * sinf(tf * 4.1f + ph * 2.0f);
}

// Luce di una torcia o di un braciere: grande e calda nelle stanze buie,
// piccola altrove
static void TorchLight(float x, float y, float pulse, bool dark, int dir) {
  if (dark)
    AddFireLightDir(x, y, 100 * pulse, 1.30f * pulse, 1.05f * pulse, 0.72f * pulse, 0.8f, dir);
  else
    AddFireLightDir(x, y, 40 * (0.92f + 0.08f * pulse), 0.68f * pulse, 0.44f * pulse, 0.16f * pulse, 0.8f, dir);
}

static bool IsFireAncilla(int type, float *radius) {
  switch (type) {
  case 0x02: *radius = 36; return true;   // colpo della Bacchetta del Fuoco
  case 0x2f: *radius = 44; return true;   // fiamma della Lanterna
  case 0x19: *radius = 90; return true;   // Medaglione della Fiamma
  case 0x32: *radius = 34; return true;   // palla di fuoco del muro esplosivo
  case 0x33: case 0x0e: case 0x0f: case 0x10: case 0x12:
    *radius = 60; return true;            // esplosioni
  case 0x34: *radius = 50; return true;   // fuoco del Bosco d'Ossa
  case 0x3a: *radius = 80; return true;   // esplosione della super bomba
  }
  return false;
}

// Vero se il punto (coordinate del mondo) sta nella parte della stanza che la
// telecamera puo' mostrare: oltre i muri la mappa 64x64 della stanza ha tile
// vecchi, mai disegnati, che possono sembrare lava o torce
static bool InRoomView(int wx, int wy) {
  int qx = quadrant_fullsize_x >> 1, qy = quadrant_fullsize_y >> 1;
  return wx >= room_bounds_x.v[qx] && wx < room_bounds_x.v[qx + 2] + 256 &&
         wy >= room_bounds_y.v[qy] && wy < room_bounds_y.v[qy + 2] + 240;
}

// Vero se intorno al punto dello schermo (x, y) e' tutto nero: la mappa della
// stanza (64x64 tile) va oltre i muri, e nella parte non disegnata restano
// tile vecchi che possono sembrare torce (braci nel nero accanto alla stanza)
static bool BlackAround(const uint8 *pixels, int pitch, int width, int height, float fx, float fy) {
  static const int8 kPts[5][2] = { {0,0},{-3,-3},{3,-3},{-3,3},{3,3} };
  int seen = 0;
  for (int i = 0; i < 5; i++) {
    int x = (int)fx + kPts[i][0], y = (int)fy + kPts[i][1];
    if (x < 0 || y < 0 || x >= width || y >= height)
      continue;
    if (((const uint32 *)(pixels + y * pitch))[x] & 0xffffff)
      return false;
    seen++;
  }
  return seen > 0;
}

// Raccoglie le luci del frame in coordinate dello schermo
static void CollectLights(const uint8 *pixels, int pitch, int height, int mod, bool dark, int frame_w,
                          int bias_x, int bias_y) {
  g_num_lights = 0;
  g_num_fairies = 0;
  g_hd_num_fire = 0;
  int ext = (frame_w - 256) / 2;
  int cam_x = BG2HOFS_copy2 + bias_x, cam_y = BG2VOFS_copy2 + bias_y;
  float t = g_fx_frames * 0.21f;
  // le fiamme di torce e bracieri cambiano fotogramma a meta' velocita'
  // (misc.c): l'alone pulsa con lo stesso ritmo
  float tf = t * 0.5f;

  if (mod == 7) {
    // Torce accese della stanza
    int room = dungeon_room_index;
    // stanza della telecamera (nello scorrimento 7/2 per un po' quella vecchia)
    int ox = BG2HOFS_copy2 & ~0x1ff, oy = BG2VOFS_copy2 & ~0x1ff;
    int start = dung_index_of_torches_start >> 1;
    uint16 torch_mask = GetTorchMask();
    for (int i = start; i < start + 16; i++) {
      if (!(torch_mask >> (i - start) & 1))
        continue;
      uint16 pos = dung_object_tilemap_pos[i];
      if (!(pos & 0x8000))
        continue;
      int w = (pos >> 1) & 0xfff;
      float x = ox + (w & 63) * 8 + 8 - cam_x + ext;
      float y = oy + ((w >> 6) & 63) * 8 + 8 - cam_y;
      float flicker = 1.0f + 0.05f * sinf(tf + i * 1.7f) + 0.03f * sinf(tf * 2.3f + i);
      if (dark)
        AddFireLight(x, y - 4, 96 * flicker, 1.25f, 1.05f, 0.80f, 1.0f);
      else
        AddFireLight(x, y - 4, 60 * flicker, 0.45f, 0.30f, 0.12f, 1.0f);
    }
    // Torce a muro e bracieri decorativi: un piccolo alone che pulsa come la
    // fiamma, per far capire che fanno luce. Nelle stanze buie illuminano di piu'.
    if (g_decor_room != room || --g_decor_scan_timer <= 0) {
      ScanDecorLights();
      g_decor_scan_timer = 30;
    }
    for (int i = 0; i < g_num_decor; i++) {
      DecorLight *d = &g_decor[i];
      if (!InRoomView(ox + d->x, oy + d->y))
        continue;
      float x = ox + d->x - cam_x + ext, y = oy + d->y - cam_y;
      if (BlackAround(pixels, pitch, frame_w, height, x, y))
        continue;
      TorchLight(x, y, FlamePulse(tf, d->phase * 0.1f), dark, d->dir);
    }
    // Link con la Lanterna (nelle stanze buie); senza Lanterna una luce minima
    if (dark) {
      // con le transizioni veloci Link puo' essere disegnato poco piu' in la' (hd_trans.c)
      float lx = link_x_coord + g_trans_link_dx + 8 - cam_x + ext, ly = link_y_coord + g_trans_link_dy + 12 - cam_y;
      if (link_item_torch)
        AddLight(lx, ly, 72 * (1.0f + 0.03f * sinf(t * 1.3f)), 1.15f, 1.0f, 0.82f);
      else
        AddLight(lx, ly, 26, 0.40f, 0.38f, 0.36f);
    }
  }
  // Fuoco: fiamme, colpi di fuoco ed esplosioni
  for (int k = 0; k < kAncilla_Slots; k++) {
    float radius;
    // Bomba: fa luce solo mentre esplode (fase 1-10), con un lampo che si spegne
    if (ancilla_type[k] == 0x07) {
      int phase = ancilla_item_to_link[k];
      if (phase < 1 || phase > 10)
        continue;
      float fade = 1.0f - (phase - 1) / 11.0f;
      float x = (ancilla_x_hi[k] << 8 | ancilla_x_lo[k]) + 8 - cam_x + ext;
      float y = (ancilla_y_hi[k] << 8 | ancilla_y_lo[k]) + 8 - cam_y;
      float m = dark ? 1.3f : 1.0f;
      AddFireLight(x, y, 74 * m * (0.6f + 0.4f * fade), 1.0f * fade * m, 0.75f * fade * m, 0.40f * fade * m, 2.0f * fade);
      continue;
    }
    if (!IsFireAncilla(ancilla_type[k], &radius))
      continue;
    // La Lanterna non fa bagliore dove e' gia' tutto illuminato (all'aperto,
    // stanze normali, stanze buie con le torce accese): solo nel buio
    if (ancilla_type[k] == 0x2f && (!dark || g_ambient >= 0.999f))
      continue;
    float x = (ancilla_x_hi[k] << 8 | ancilla_x_lo[k]) + 8 - cam_x + ext;
    float y = (ancilla_y_hi[k] << 8 | ancilla_y_lo[k]) + 8 - cam_y;
    radius *= 1.0f + 0.08f * sinf(t * 1.9f + k);
    if (dark)
      AddFireLight(x, y, radius * 1.3f, 1.2f, 0.95f, 0.65f, 0.6f);
    else
      AddFireLight(x, y, radius, 0.55f, 0.32f, 0.10f, 0.6f);
  }
  // Cespugli ed erba in fiamme (hd_burn.c): un po' di luce calda e le braci
  if (mod == 9) {
    const HdBurnTile *tiles;
    int n = HdBurn_GetBurning(&tiles);
    // con il Medaglione della Fiamma le tile sono centinaia: una luce ogni
    // tanto basta
    int stride = n > 48 ? (n + 47) / 48 : 1;
    for (int i = 0; i < n; i += stride) {
      float age = tiles[i].age;
      float inten = (age < 10 ? age / 10.0f : 1.0f) * (age > kHdBurnTime - 20 ? (kHdBurnTime - age) / 20.0f : 1.0f);
      float x = tiles[i].x + 8 - cam_x + ext, y = tiles[i].y + 4 - cam_y;
      float flicker = 1.0f + 0.10f * sinf(t * 2.3f + tiles[i].seed);
      AddFireLight(x, y, 34 * sqrtf((float)stride) * inten * flicker, 0.40f * inten, 0.22f * inten, 0.06f * inten, 0.7f * inten);
    }
  }
}

// Nemici e proiettili infuocati (sprite)
static bool IsFireSprite(int type, float *radius, bool *cold) {
  *cold = false;
  switch (type) {
  case 0x70: *radius = 34; return true;  // palla di fuoco del Re Helmasaur
  case 0x7e: *radius = 64; return true;  // catena di fuoco rotante
  case 0x80: *radius = 30; return true;  // serpente di fuoco
  case 0x87: *radius = 30; return true;  // fiamma dei Kodongo
  case 0xcc: *radius = 48; return true;  // testa di fuoco di Trinexx
  case 0x7b: *radius = 36; *cold = true; return true;  // sfere di energia di Agahnim
  }
  return false;
}

static void CollectSpriteLights(bool dark, int ext, int cam_x, int cam_y, float t) {
  for (int k = 0; k < 16; k++) {
    float radius;
    bool cold;
    if (!sprite_state[k] || !IsFireSprite(sprite_type[k], &radius, &cold))
      continue;
    float x = (sprite_x_hi[k] << 8 | sprite_x_lo[k]) + 8 - cam_x + ext;
    float y = (sprite_y_hi[k] << 8 | sprite_y_lo[k]) + 8 - (int8)sprite_z[k] - cam_y;
    radius *= 1.0f + 0.08f * sinf(t * 2.1f + k);
    float m = dark ? 1.3f : 1.0f;
    if (cold)
      AddLight(x, y, radius * m, 0.30f * m, 0.40f * m, 0.75f * m);
    else
      AddFireLight(x, y, radius * m, 0.60f * m, 0.36f * m, 0.12f * m, 0.6f);
  }
  // Fate: un bagliore appena piu' piccolo dello sprite che pulsa come le torce
  // (stessa formula: intensita' e grandezza insieme, due onde sovrapposte)
  for (int k = 0; k < 16; k++) {
    // 0xe3 fata piccola, 0xc8 grande fata (non la sua nuvola che cura Link,
    // stesso tipo con sprite_head_dir 1)
    bool big = sprite_type[k] == 0xc8 && !sprite_head_dir[k];
    if (!sprite_state[k] || (sprite_type[k] != 0xe3 && !big))
      continue;
    float x = (sprite_x_hi[k] << 8 | sprite_x_lo[k]) + 8 - cam_x + ext;
    float y = (sprite_y_hi[k] << 8 | sprite_y_lo[k]) + 8 - (int8)sprite_z[k] - cam_y;
    float pulse = FlamePulse(t * 0.5f, k * 1.7f);
    if (g_num_fairies < kMaxFairies) {
      g_fairy_x[g_num_fairies] = x, g_fairy_y[g_num_fairies] = y;
      g_fairy_r[g_num_fairies] = big ? 24.0f : 9.0f;
      g_fairy_box[g_num_fairies] = big ? 11 : 6;
      g_fairy_m[g_num_fairies++] = pulse;
    }
  }
  // Regina delle fate (Cascata dei Desideri, Stagno della Felicita') e fata
  // grassa della Piramide: sprite 0x72 con sprite_B (con anche sprite_A sono
  // le scintille). Compare e scompare con la palette 5 degli sprite
  // (PaletteFilter_SP5F): l'alone si accende e si spegne con lei
  for (int k = 0; k < 16; k++) {
    if (!sprite_state[k] || sprite_type[k] != 0x72 || !sprite_B[k] || sprite_A[k] ||
        g_num_fairies >= kMaxFairies)
      continue;
    int now = 0, full = 0;
    for (int i = 1; i < 8; i++) {
      uint16 a = main_palette_buffer[0xd0 + i], b = aux_palette_buffer[0xd0 + i];
      now += (a & 31) + (a >> 5 & 31) + (a >> 10 & 31);
      full += (b & 31) + (b >> 5 & 31) + (b >> 10 & 31);
    }
    float shown = full ? (float)now / full : 1.0f;
    if (shown > 1.0f) shown = 1.0f;
    if (shown < 0.05f)
      continue;
    // alone grande: un respiro lento (un ciclo in circa 150 fotogrammi), senza
    // l'onda veloce delle fate piccole che su questo raggio lampeggiava
    float pulse = 0.88f + 0.10f * sinf(t * 0.2f);  // t = fotogrammi * 0.21
    // disegno di 32x40 (48 alto per la fata grassa) dall'angolo dello sprite
    g_fairy_x[g_num_fairies] = (sprite_x_hi[k] << 8 | sprite_x_lo[k]) + 16 - cam_x + ext;
    g_fairy_y[g_num_fairies] = (sprite_y_hi[k] << 8 | sprite_y_lo[k]) + 22 - (int8)sprite_z[k] - cam_y;
    g_fairy_r[g_num_fairies] = 34.0f;
    g_fairy_box[g_num_fairies] = 14;
    g_fairy_m[g_num_fairies++] = pulse * shown;
  }
}

// Lava (per esempio nella Rocca Tartaruga): sono tile animati del fondale (i
// caratteri 0x1B0-0x1CF contengono acqua o lava a seconda del labirinto, piu' le
// fiamme delle torce). Si considerano lava quelli la cui grafica e' rosso-
// arancio e si aggiunge un alone ogni due tile. L'alone sta sopra la lava:
// sui tile al bordo della pozza e' piu' piccolo e spostato verso l'interno,
// cosi' non illumina i muri e i pavimenti intorno.
// Tile gia' riconosciuti come lava o fiamma nella stanza attuale (un bit per
// tile, BG2 e BG1). Le fiamme e la lava sono animate e in alcuni fotogrammi
// dell'animazione i punti guardati sono trasparenti o poco rossi: un tile
// riconosciuto resta tale finche' Link e' nella stanza, cosi' la luce non lampeggia
static uint8 g_lava_known[2][64 * 64 / 8];
static int g_lava_room = -1;

static bool IsLavaCell(const uint16 *tm, int row, int col, int ox, int oy) {
  if (row < 0 || col < 0 || row >= 64 || col >= 64)
    return false;
  int ch = tm[row * 64 + col] & 0x3ff;
  if (ch < 0x1b0 || ch > 0x1cf || !InRoomView(ox + col * 8 + 4, oy + row * 8 + 4))
    return false;
  uint8 *known = &g_lava_known[tm == dung_bg1][(row * 64 + col) >> 3];
  uint8 bit = 1 << (col & 7);
  if (*known & bit)
    return true;
  // Il colore si legge dalla grafica del tile in VRAM con la sua palette, non
  // dallo schermo: cosi' la lava e le fiamme si trovano in tutta la stanza fin
  // dall'ingresso, anche fuori dalla vista, e fate o nemici rossi sopra
  // l'acqua non contano. Cinque punti del tile (simmetrici: il ribaltamento
  // non conta), decide la maggioranza di quelli non trasparenti.
  Ppu *ppu = g_zenv.ppu;
  uint16 e = tm[row * 64 + col];
  int ta = ppu->bgLayer[tm == dung_bg1 ? 0 : 1].tileAdr + ch * 16, pal = (e >> 10 & 7) * 16;
  static const int8 kPts[5][2] = { {4,4},{1,1},{6,1},{1,6},{6,6} };
  int seen = 0, red = 0;
  for (int i = 0; i < 5; i++) {
    int px = kPts[i][0], py = kPts[i][1];
    uint32 bits = ppu->vram[(ta + py) & 0x7fff] | ppu->vram[(ta + py + 8) & 0x7fff] << 16;
    int v = (bits >> (7 - px)) & 1 | (bits >> (14 - px)) & 2 | (bits >> (21 - px)) & 4 | (bits >> (28 - px)) & 8;
    if (!v)
      continue;
    uint16 c = HdFx_RealBg(ppu->cgram, pal + v);
    int r = (c & 31) << 3, g = (c >> 5 & 31) << 3, b = (c >> 10 & 31) << 3;
    seen++;
    red += r > 120 && r > g + 30 && b < r / 2;  // rosso-arancio: lava (non acqua o altro)
  }
  if (!seen || red * 2 <= seen)
    return false;
  *known |= bit;
  return true;
}

// Lava visibile sullo schermo nei due tile a partire da (sx-4, sy-4). Gli
// sprite non contano (Link su un tubo sopra la lava coperta la faceva
// accendere e spegnere mentre camminava). Si ricorda per la stanza: una
// cella vista rossa resta visibile (anche nei fotogrammi scuri
// dell'animazione), una mai vista rossa e' coperta; non ancora vista (fuori
// dallo schermo o sotto uno sprite) fa luce
static uint8 g_lava_vis[2][64 * 64];  // 0 non vista, 1 visibile, 2 coperta

static bool LavaShows(const uint8 *pixels, int pitch, int width, int height, int sx, int sy,
                      int layer, int row, int col) {
  static const int8 kPts[5][2] = { {4,4},{0,0},{12,0},{0,12},{12,12} };
  uint8 *vis = &g_lava_vis[layer][row * 64 + col];
  if (*vis == 1)
    return true;
  bool seen = false;
  for (int i = 0; i < 5; i++) {
    int x = sx - 4 + kPts[i][0] + 2, y = sy - 4 + kPts[i][1] + 2;
    if (x < 0 || y < 0 || x >= width || y >= height)
      continue;
    uint32 c = ((const uint32 *)(pixels + y * pitch))[x];
    int l = c >> 24, r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
    if (l == 2 || l == 4 || l == 6)
      continue;
    seen = true;
    if (r > 120 && r > g + 30 && b < r / 2) {
      *vis = 1;
      return true;
    }
  }
  if (seen)
    *vis = 2;
  return *vis != 2;
}

static void CollectLavaLights(const uint8 *pixels, int pitch, int width, int height,
                              bool dark, int ext, int cam_x, int cam_y, float t) {
  // stanza della telecamera (nello scorrimento 7/2 per un po' quella vecchia)
  int ox = BG2HOFS_copy2 & ~0x1ff, oy = BG2VOFS_copy2 & ~0x1ff;
  if (g_lava_room != dungeon_room_index) {
    g_lava_room = dungeon_room_index;
    memset(g_lava_known, 0, sizeof(g_lava_known));
    memset(g_lava_vis, 0, sizeof(g_lava_vis));
  }
  for (int layer = 0; layer < 2; layer++) {
    const uint16 *tm = layer ? dung_bg1 : dung_bg2;
    for (int row = 0; row < 64; row += 2) {
      for (int col = 0; col < 64; col += 2) {
        if (!IsLavaCell(tm, row, col, ox, oy))
          continue;
        int sx = ox + col * 8 + 4 - cam_x + ext, sy = oy + row * 8 + 4 - cam_y;
        // solo vicino allo schermo: in alcune stanze (Rocca Tartaruga) la lava
        // riempie tutta la mappa 64x64 e le prime file esaurivano le luci e le
        // sorgenti delle braci (braci solo a nord, lava piu' scura in basso)
        if (sx < -40 || sx > width + 40 || sy < -40 || sy > height + 40)
          continue;
        // lava coperta (tubi, pedane, muri del piano di sopra): nessuno dei
        // punti visibili e' rosso-arancio
        if (!LavaShows(pixels, pitch, width, height, sx, sy, layer, row, col))
          continue;
        // vicini a due tile di distanza: quanti sono lava e da che parte
        int n = 0, dx = 0, dy = 0;
        for (int k = 0; k < 8; k++) {
          static const int8 kDir[8][2] = { {-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1} };
          if (IsLavaCell(tm, row + kDir[k][1] * 2, col + kDir[k][0] * 2, ox, oy))
            n++, dx += kDir[k][0], dy += kDir[k][1];
        }
        float inner = n / 8.0f;  // 1 in mezzo alla pozza, meno sul bordo
        // Le fiamme di torce e bracieri hanno gia' il loro alone
        bool near_decor = false;
        for (int i = 0; i < g_num_decor && !near_decor; i++)
          near_decor = abs(g_decor[i].x - (col * 8 + 4)) < 16 && abs(g_decor[i].y - (row * 8 + 4)) < 16;
        if (near_decor)
          continue;
        if (n == 0) {
          // da sola non e' una pozza ma una fiamma che non si e' riconosciuta
          // fra le torce (per esempio i bracieri della casa del vecchio sulla
          // Montagna della Morte): la luce di una torcia
          TorchLight(sx + 4, sy + 4, FlamePulse(t * 0.5f, (col * 7 + row * 13) * 0.1f), dark, 0);
          continue;
        }
        float pulse = 0.85f + 0.15f * sinf(t * 0.9f + (col + row) * 0.6f);
        // le luci a due tile di distanza si sommano (circa quattro per punto)
        float m = (dark ? 1.4f : 1.0f) * (0.35f + 0.65f * inner);
        float lx = sx + 4 + (n ? dx * 8.0f / n : 0), ly = sy + 4 + (n ? dy * 8.0f / n : 0);
        AddFireLight(lx, ly, 26 * (0.5f + 0.5f * inner) * (dark ? 1.4f : 1.0f),
                     0.26f * pulse * m, 0.12f * pulse * m, 0.03f * pulse * m, 0.25f);
      }
    }
  }
}

void HdLight_BeforeRender(Ppu *ppu) {
  g_dark_override = false;
  int mod;
  if (!g_dynamic_light)
    return;
  // Uscendo da una stanza buia illuminata del tutto (Module0F) il buio del
  // gioco tornerebbe per la durata della dissolvenza: resta sospeso
  bool leaving_lit = main_module_index == 15 && g_lit_dark_room && dung_want_lights_out;
  if (!leaving_lit && (!InGameplay(&mod) || !RoomLooksDark(mod)))
    return;
  // Si sospende il buio originale: sottrazione del colore fisso e cono della
  // Lanterna (BG1 sullo schermo secondario). Lo sostituisce HdLight_Apply.
  g_dark_override = true;
  g_saved_fixed[0] = ppu->fixedColorR, g_saved_fixed[1] = ppu->fixedColorG, g_saved_fixed[2] = ppu->fixedColorB;
  g_saved_sub_enabled = ppu->screenEnabled[1];
  ppu->fixedColorR = ppu->fixedColorG = ppu->fixedColorB = 0;
  ppu->screenEnabled[1] &= ~1;
}

bool HdLight_DarkOverride(void) {
  return g_dark_override;
}

void HdLight_AfterRender(Ppu *ppu) {
  if (!g_dark_override)
    return;
  ppu->fixedColorR = g_saved_fixed[0], ppu->fixedColorG = g_saved_fixed[1], ppu->fixedColorB = g_saved_fixed[2];
  ppu->screenEnabled[1] = g_saved_sub_enabled;
}

// Bagliore delle fate: alone rosa chiaro solo sullo sfondo attorno alla fata
// (i pixel degli sprite, livello 4 e 6, restano con i loro colori), raggio fino
// a 9 pixel (24 per la grande fata), che cresce e cala con la pulsazione
static void ApplyFairyGlow(uint8 *pixels, int pitch, int width, int height) {
  for (int i = 0; i < g_num_fairies; i++) {
    int cx = (int)g_fairy_x[i], cy = (int)g_fairy_y[i];
    float m = g_fairy_m[i];
    int kRadius = (int)g_fairy_r[i] + 1, box = g_fairy_box[i];
    float rad = g_fairy_r[i] * m, inv_r2 = 1.0f / (rad * rad);
    // l'alone grande, piu' esteso, e' appena meno intenso al centro
    float peak = box > 6 ? m * 0.9f : m;
    // solo se la fata si vede: nascosta (per esempio sotto la chioma di un
    // albero, che il gioco disegna sopra gli sprite) non fa luce
    int seen = 0;
    for (int y = cy - box; y <= cy + box; y++) {
      if (y < 0 || y >= height)
        continue;
      const uint32 *row = (const uint32 *)(pixels + y * pitch);
      for (int x = cx - box; x <= cx + box; x++)
        if (x >= 0 && x < width && ((row[x] >> 24) == 4 || (row[x] >> 24) == 6))
          seen++;
    }
    if (seen < (box > 6 ? 40 : 12))
      continue;
    for (int y = cy - kRadius; y <= cy + kRadius; y++) {
      if (y < 0 || y >= height)
        continue;
      uint32 *row = (uint32 *)(pixels + y * pitch);
      for (int x = cx - kRadius; x <= cx + kRadius; x++) {
        int layer = x < 0 || x >= width ? 2 : row[x] >> 24;
        if (layer == 2 || layer == 4 || layer == 6)
          continue;  // non sull'HUD e non sugli sprite
        float d2 = (float)((x - cx) * (x - cx) + (y - cy) * (y - cy)) * inv_r2;
        if (d2 >= 1.0f)
          continue;
        float a = (1.0f - d2) * peak;
        row[x] = HdFx_AddRgb(row[x], 85, 65, 90, a);
      }
    }
  }
}

static void ApplyLights(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y);

void HdLight_Apply(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  ApplyLights(pixels, pitch, width, height, bias_x, bias_y);
}

// L'alone delle fate va dopo l'acqua HD (hd_water.c): fatto prima, sui pixel
// schiariti l'acqua HD non si disegnava e si vedeva quella del gioco
void HdLight_FairyGlow(uint8 *pixels, int pitch, int width, int height) {
  if (g_dynamic_light)
    ApplyFairyGlow(pixels, pitch, width, height);
  g_num_fairies = 0;
}

// Fata che fa rialzare Link dopo la morte (Module12, fasi 11-13): e' l'ancilla
// 0 (RevivalFairy_Main, ancilla.c), non uno sprite, e nel modulo 18 le luci
// non si calcolano. Si aggiunge da sola all'elenco delle fate
static void CollectRevivalFairy(int width, int bias_x, int bias_y) {
  if (main_module_index != 18 || submodule_index < 11 || submodule_index > 13 ||
      ancilla_step[0] > 2 || g_num_fairies >= kMaxFairies)
    return;
  int ext = (width - 256) / 2;
  int cam_x = BG2HOFS_copy2 + bias_x, cam_y = BG2VOFS_copy2 + bias_y;
  g_fairy_x[g_num_fairies] = (ancilla_x_hi[0] << 8 | ancilla_x_lo[0]) + 8 - cam_x + ext;
  g_fairy_y[g_num_fairies] = (ancilla_y_hi[0] << 8 | ancilla_y_lo[0]) + 8 - (int8)ancilla_z[0] - cam_y;
  // sprite da 16x16, piu' grande della fata 0xe3: con raggio 9 l'alone
  // restava quasi tutto sotto la fata
  g_fairy_r[g_num_fairies] = 16.0f;
  g_fairy_box[g_num_fairies] = 6;
  g_fairy_m[g_num_fairies++] = FlamePulse(g_fx_frames * 0.21f * 0.5f, 0.0f);
}

// Luce dell'ultimo ApplyLights, per la vista 3D (HdLight_Projected): il
// terreno oltre lo schermo e gli sprite la prendono dopo la proiezione
static struct {
  bool ready;
  int width, height, sub;
  float extra, light_scale;
  int box_x0, box_y0, box_x1, box_y1;
  bool room_box;                       // stanza buia: la luce resta nel riquadro room_*
  int room_x0, room_y0, room_x1, room_y1;   // la stanza, non tagliata ai bordi dello schermo
  int n_items, item_x[4], item_y[4], item_w[4], item_h[4];
} g_lp;

static void ApplyLights(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  int mod;
  g_lp.ready = false;
  if (g_dynamic_light)
    CollectRevivalFairy(width, bias_x, bias_y);
  g_hd_num_fire = 0;
  g_hd_light_sub = 0;
  if (!InGameplay(&mod) || (!g_dynamic_light && !g_fx_heat)) {
    g_ambient = 1.0f;
    g_ambient_sub = 0.0f;
    g_ambient_place = -1;
    return;
  }
  bool dark = IsDarkRoom(mod);
  bool hold_old = FadeHoldsOldRoom();
  if (hold_old)
    dark = g_fade_dark;
  else
    g_fade_dark = dark;
  // Le stanze buie da Lanterna (nel gioco si vede solo il cono di luce) sono
  // completamente al buio; le altre (per esempio il passaggio segreto sotto
  // il castello) restano solo in penombra
  float dark_ambient = hdr_dungeon_dark_with_lantern ? 0.0f : kDarkAmbient;
  // passaggio fra le stanze: vale il piu' buio visto (g_dark_hold, sopra)
  bool passing = HdFx_DungeonPassing();
  if (passing) {
    if (dark_ambient < g_dark_hold)
      g_dark_hold = dark_ambient;
    dark_ambient = g_dark_hold;
  } else {
    g_dark_hold = dark_ambient;
  }
  // Luce ambiente desiderata
  float target = 1.0f, target_sub = 0.0f;
  if (dark) {
    int total, lit;
    CountTorches(&total, &lit);
    // Torce sempre accese (oggetto 0x20, per esempio nella bottega delle pozioni):
    // il gioco le conta in dung_num_lit_torches ma non sono fra quelle accendibili.
    // Contano come accese, e se tutte le torce della stanza sono accese la
    // stanza e' illuminata del tutto
    int fixed = dung_num_lit_torches - lit;
    if (fixed > 0) {
      lit += fixed;
      total += fixed;
    }
    // Come nel gioco ogni torcia accesa, fino a 3, schiarisce un po' tutta la
    // stanza (colore fisso sottratto 31, 8, 4, 0: kLitTorchesColorPlus). In una
    // stanza con meno di 3 torce accenderle tutte la illumina del tutto.
    static const float kTorchAmbient[4] = { 0.0f, 0.40f, 0.70f, 1.0f };
    static const float kTorchSub[4] = { 0.0f, 100.0f, 50.0f, 0.0f };
    target = kTorchAmbient[lit < 3 ? lit : 3];
    target_sub = kTorchSub[lit < 3 ? lit : 3];
    if (total > 0 && lit >= total)
      target = 1.0f, target_sub = 0.0f;
    if (target < dark_ambient)
      target = dark_ambient;
  } else if (mod == 7) {
    target = 0.86f;  // labirinti: un po' piu' scuri
  }
  // Transizione morbida (circa un secondo per accendere tutta la stanza)
  // solo dentro la stessa stanza, accendendo le torce: entrando in una stanza
  // o in una grotta la luce e' subito quella giusta (una stanza buia parte buia)
  int place = mod == 7 ? dungeon_room_index : -2;
  if (hold_old) {
    target = g_ambient, target_sub = g_ambient_sub;
    place = g_ambient_place;
  }
  if (place != g_ambient_place) {
    g_ambient_place = place;
    g_ambient = target;
    g_ambient_sub = target_sub;
    g_snap_pending = passing;
  }
  // nella stanza nuova la luce e' subito quella giusta, anche durante il passaggio
  if (g_snap_pending) {
    g_snap_pending = passing;
    g_ambient = target;
    g_ambient_sub = target_sub;
  }
  g_ambient += (target - g_ambient) * 0.06f;
  if (fabsf(target - g_ambient) < 0.003f)
    g_ambient = target;
  g_ambient_sub += (target_sub - g_ambient_sub) * 0.06f;
  if (fabsf(target_sub - g_ambient_sub) < 0.5f)
    g_ambient_sub = target_sub;
  g_lit_dark_room = dark && g_ambient >= 0.999f && g_ambient_sub == 0.0f;

  CollectLights(pixels, pitch, height, mod, dark, width, bias_x, bias_y);
  {
    int ext = (width - 256) / 2;
    int cam_x = BG2HOFS_copy2 + bias_x, cam_y = BG2VOFS_copy2 + bias_y;
    float t = g_fx_frames * 0.21f;
    CollectSpriteLights(dark, ext, cam_x, cam_y, t);
    if (mod == 7)
      CollectLavaLights(pixels, pitch, width, height, dark, ext, cam_x, cam_y, t);
  }
  if (!g_dynamic_light) {
    g_ambient = 1.0f;
    g_ambient_sub = 0.0f;
    return;  // servivano solo le sorgenti delle braci
  }
  if (g_ambient >= 0.999f && g_ambient_sub == 0.0f && g_num_lights == 0)
    return;

  // moltiplicazione in piu' che accompagna la sottrazione del contrasto: con una
  // torcia (sub 100) un colore 100 diventa 25 e uno 140 diventa 75
  float extra = g_ambient_sub * (0.85f / 100.0f);
  int sub = (int)(g_ambient_sub + 0.5f);
  g_hd_light_sub = sub;
  int n = width * height;
  if (n * 3 > g_factor_cap) {
    free(g_factor);
    g_factor = malloc(sizeof(float) * n * 3);
    g_factor_cap = g_factor ? n * 3 : 0;
    if (!g_factor)
      return;
  }
  for (int i = 0; i < n * 3; i++)
    g_factor[i] = g_ambient + extra;
  // A stanza illuminata le fiamme fanno solo un alone leggero
  float light_scale = 1.0f;
  if (dark) {
    float k = (g_ambient - dark_ambient) / (1.0f - dark_ambient);
    light_scale = 1.0f - (k < 0 ? 0 : k > 1 ? 1 : k) * 0.75f;
  }
  // Nelle stanze buie la luce resta nella stanza (512x512 nel mondo) al centro
  // dello schermo, come il cono della Lanterna del gioco: passando da una porta
  // le fasce dello schermo largo mostrano gia' la stanza vicina, che la luce
  // illuminava (si vedeva un pezzo dell'altra stanza)
  int box_x0 = 0, box_y0 = 0, box_x1 = width - 1, box_y1 = height - 1;
  g_lp.room_box = dark;
  if (dark) {
    int ext = (width - 256) / 2;
    int cam_x = BG2HOFS_copy2 + bias_x, cam_y = BG2VOFS_copy2 + bias_y;
    int rx = (cam_x + 128) & ~511, ry = (cam_y + 112) & ~511;
    g_lp.room_x0 = rx - cam_x + ext, g_lp.room_x1 = rx + 511 - cam_x + ext;
    g_lp.room_y0 = ry - cam_y, g_lp.room_y1 = ry + 511 - cam_y;
    box_x0 = IntMax(rx - cam_x + ext, 0), box_x1 = IntMin(rx + 511 - cam_x + ext, width - 1);
    box_y0 = IntMax(ry - cam_y, 0), box_y1 = IntMin(ry + 511 - cam_y, height - 1);
  }
  for (int li = 0; li < g_num_lights; li++) {
    Light *l = &g_lights[li];
    int x0 = (int)(l->x - l->radius), x1 = (int)(l->x + l->radius);
    int y0 = (int)(l->y - l->radius), y1 = (int)(l->y + l->radius);
    if (x0 < box_x0) x0 = box_x0;
    if (y0 < box_y0) y0 = box_y0;
    if (x1 > box_x1) x1 = box_x1;
    if (y1 > box_y1) y1 = box_y1;
    float inv_r2 = 1.0f / (l->radius * l->radius);
    for (int y = y0; y <= y1; y++) {
      float dy = y - l->y;
      float *f = g_factor + (y * width + x0) * 3;
      for (int x = x0; x <= x1; x++, f += 3) {
        float dx = x - l->x;
        float d2 = (dx * dx + dy * dy) * inv_r2;
        if (d2 >= 1.0f)
          continue;
        float a = (1.0f - d2);
        a = a * a * light_scale;  // bordo morbido
        f[0] += l->r * a, f[1] += l->g * a, f[2] += l->b * a;
      }
    }
  }
  // L'oggetto appena ottenuto, sollevato da Link, resta con i suoi colori:
  // riquadri attorno all'oggetto ricevuto, all'Amuleto/Cristallo e alla Spada Suprema
  int item_x[4], item_y[4], item_w[4], item_h[4], n_items = 0;
  {
    int ext = (width - 256) / 2;
    int cam_x = BG2HOFS_copy2 + bias_x, cam_y = BG2VOFS_copy2 + bias_y;
    for (int k = 0; k < kAncilla_Slots && n_items < 4; k++) {
      int type = ancilla_type[k];
      if (type != 0x22 && type != 0x29 && type != 0x35)
        continue;
      bool big = type != 0x22;
      item_x[n_items] = (ancilla_x_hi[k] << 8 | ancilla_x_lo[k]) - cam_x + ext - (big ? 4 : 2);
      item_y[n_items] = (ancilla_y_hi[k] << 8 | ancilla_y_lo[k]) - cam_y - (big ? 4 : 2);
      item_w[n_items] = big ? 24 : 20;
      item_h[n_items] = big ? 36 : 20;
      n_items++;
    }
  }
  g_lp.ready = true, g_lp.width = width, g_lp.height = height, g_lp.sub = sub;
  g_lp.extra = extra, g_lp.light_scale = light_scale;
  g_lp.box_x0 = box_x0, g_lp.box_y0 = box_y0, g_lp.box_x1 = box_x1, g_lp.box_y1 = box_y1;
  g_lp.n_items = n_items;
  for (int i = 0; i < n_items; i++)
    g_lp.item_x[i] = item_x[i], g_lp.item_y[i] = item_y[i], g_lp.item_w[i] = item_w[i], g_lp.item_h[i] = item_h[i];
  // Applicazione: l'HUD e i testi (layer 2 nel byte alto) restano intatti
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    const float *f = g_factor + y * width * 3;
    for (int x = 0; x < width; x++, f += 3) {
      uint32 c = row[x];
      if ((c >> 24) == 2)
        continue;
      if (n_items && ((c >> 24) == 4 || (c >> 24) == 6)) {
        bool held = false;
        for (int i = 0; i < n_items; i++)
          held |= x >= item_x[i] && x < item_x[i] + item_w[i] && y >= item_y[i] && y < item_y[i] + item_h[i];
        if (held)
          continue;
      }
      float fr = f[0] < 1.35f + extra ? f[0] : 1.35f + extra;
      float fg = f[1] < 1.30f + extra ? f[1] : 1.30f + extra;
      float fb = f[2] < 1.25f + extra ? f[2] : 1.25f + extra;
      int r = (int)(((c >> 16) & 0xff) * fr) - sub, g = (int)(((c >> 8) & 0xff) * fg) - sub, b = (int)((c & 0xff) * fb) - sub;
      if (r < 0) r = 0;
      if (g < 0) g = 0;
      if (b < 0) b = 0;
      if (r > 255) r = 255;
      if (g > 255) g = 255;
      if (b > 255) b = 255;
      row[x] = (c & 0xff000000) | r << 16 | g << 8 | b;
    }
  }
}

void HdLight_Projected(uint8 *pixels, int pitch, int width, int height, const uint32 *uv, const uint8 *kind) {
  if (!g_lp.ready || width != g_lp.width || height != g_lp.height)
    return;
  float extra = g_lp.extra;
  int sub = g_lp.sub;
  // nelle stanze buie la luce resta nella stanza (tutta: con la telecamera
  // in terza persona si vede ben oltre lo schermo, e la luce delle torce e
  // della Lanterna si fermava al vecchio bordo)
  bool boxed = g_lp.room_box;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++) {
      int i = y * width + x;
      uint32 c = row[x];
      if (kind[i] < 2 || (c >> 24) == 2)
        continue;   // cielo, o terreno dello schermo che ce l'ha gia'
      int u = (int16)(uv[i] & 0xffff), v = (int16)(uv[i] >> 16);
      if (kind[i] == 3 && g_lp.n_items) {
        bool held = false;
        for (int k = 0; k < g_lp.n_items; k++)
          held |= u >= g_lp.item_x[k] && u < g_lp.item_x[k] + g_lp.item_w[k] && v >= g_lp.item_y[k] &&
                  v < g_lp.item_y[k] + g_lp.item_h[k];
        if (held)
          continue;
      }
      float f[3];
      if (u >= 0 && u < width && v >= 0 && v < height) {
        const float *g = g_factor + (v * width + u) * 3;
        f[0] = g[0], f[1] = g[1], f[2] = g[2];
      } else {
        f[0] = f[1] = f[2] = g_ambient + extra;
        bool in_box = !boxed || (u >= g_lp.room_x0 && u <= g_lp.room_x1 && v >= g_lp.room_y0 && v <= g_lp.room_y1);
        for (int li = 0; in_box && li < g_num_lights; li++) {
          const Light *l = &g_lights[li];
          float dx = u - l->x, dy = v - l->y;
          if (fabsf(dx) >= l->radius || fabsf(dy) >= l->radius)
            continue;
          float d2 = (dx * dx + dy * dy) / (l->radius * l->radius);
          if (d2 >= 1.0f)
            continue;
          float a = (1.0f - d2);
          a = a * a * g_lp.light_scale;
          f[0] += l->r * a, f[1] += l->g * a, f[2] += l->b * a;
        }
      }
      float fr = f[0] < 1.35f + extra ? f[0] : 1.35f + extra;
      float fg = f[1] < 1.30f + extra ? f[1] : 1.30f + extra;
      float fb = f[2] < 1.25f + extra ? f[2] : 1.25f + extra;
      int r = (int)(((c >> 16) & 0xff) * fr) - sub, g = (int)(((c >> 8) & 0xff) * fg) - sub, b = (int)((c & 0xff) * fb) - sub;
      r = r < 0 ? 0 : r > 255 ? 255 : r;
      g = g < 0 ? 0 : g > 255 ? 255 : g;
      b = b < 0 ? 0 : b > 255 ? 255 : b;
      row[x] = (c & 0xff000000) | r << 16 | g << 8 | b;
    }
  }
}
