// Effetti visivi aggiuntivi. Lavorano sull'immagine gia' disegnata dalla PPU
// (pixel 0xLLRRGGBB, dove LL e' il layer di provenienza: 2 = HUD, 4/6 = sprite)
// e non toccano la logica di gioco.
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "hd_fx.h"
#include "tile_detect.h"
#include "hd_light.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "snes/ppu.h"
#include "hd_world.h"
#include "hd_burn.h"
#include "hd_water.h"
#include "tagalong.h"

bool g_fx_heat, g_fx_window_rays, g_fx_item_glow, g_fx_rain;
bool g_fx_menu_blur, g_fx_sprite_shadows, g_fx_soft_iris;
bool g_fx_whirlpool;

const uint16 *Dungeon_GetObjectTiles(int subtype, int idx);

// ---------------------------------------------------------------------------
// Utilita'

static uint32 g_rng = 0x12345678;
static float Rand01() {
  g_rng = g_rng * 1664525 + 1013904223;
  return (g_rng >> 8) * (1.0f / 16777216.0f);
}

static inline bool IsHud(uint32 c) {
  return (c >> 24) == 2;
}

static inline bool IsSprite(uint32 c) {
  uint32 l = c >> 24;
  return l == 4 || l == 6;
}

// Somma un colore al pixel (luce additiva); l'HUD resta intatto
static inline void AddColor(uint32 *p, float r, float g, float b, float a) {
  if (!IsHud(*p))
    *p = HdFx_AddRgb(*p, r, g, b, a);
}

// Sposta il pixel verso un colore, tenendo il layer
static inline void MixPixel(uint32 *p, int r, int g, int b, float a) {
  *p = HdFx_MixRgb(*p, r, g, b, a);
}

// Segna il pixel come "sprite": lo shader four sword non lo dipinge (lo
// cancellerebbe, essendo largo un solo pixel) ma gli applica il colore
static inline void MarkSharp(uint32 *p) {
  if (!IsSprite(*p) && !IsHud(*p))
    *p = (*p & 0x00ffffff) | 4u << 24;
}

// Si vede il gioco: dentro (7), fuori (9) o in un'area speciale (11: Dominio
// di Zora, Piedistallo...), anche sotto inventario e dialoghi, nelle ricariche
// e mentre il cerchio si apre o si chiude (HdFx_SceneModule), altrimenti gli
// effetti comparivano dopo la schermata o sparivano prima dell'uscita.
// Prima acqua, luci ed effetti avevano ognuno il suo controllo, un po' diverso
bool HdFx_InGameplay(int *mod_out) {
  int mod = HdFx_SceneModule();
  *mod_out = mod;
  return mod == 7 || mod == 9 || mod == 11;
}

// Avanza le animazioni una volta per frame di gioco (non per ogni disegno)
static uint8 g_last_frame;
static bool NewGameFrame() {
  if (g_last_frame == frame_counter)
    return false;
  g_last_frame = frame_counter;
  return true;
}

// ---------------------------------------------------------------------------
// Riconoscimento della lava sullo schermo (celle di 8x8 pixel)

enum { kCellNone, kCellLava };
enum { kMaxCellsX = 64, kMaxCellsY = 32 };
static uint8 g_cells[kMaxCellsY][kMaxCellsX];
static int g_cells_w, g_cells_h;
static bool g_any_lava;

// Parola della mappa dei tile di un layer nel punto (x, y) dello schermo (x
// relativo alla parte centrale da 256 pixel), come la legge la PPU
static uint16 BgTileAt(Ppu *ppu, int layer, int x, int y, int bias_x, int bias_y) {
  BgLayer *bg = &ppu->bgLayer[layer];
  uint32 yy = y + bg->vScroll + bias_y, xx = x + bg->hScroll + bias_x;
  int sc = bg->tilemapAdr + (((yy >> 3) & 0x1f) << 5);
  if ((yy & 0x100) && bg->tilemapHigher)
    sc += bg->tilemapWider ? 0x800 : 0x400;
  if ((xx >> 8) & 1)
    sc += bg->tilemapWider ? 0x400 : 0;
  return ppu->vram[(sc + ((xx >> 3) & 0x1f)) & 0x7fff];
}

static void ClassifyCells(const uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  Ppu *ppu = g_zenv.ppu;
  int ext = (width - 256) / 2;
  g_cells_w = IntMin((width + 7) / 8, kMaxCellsX);
  g_cells_h = IntMin((height + 7) / 8, kMaxCellsY);
  g_any_lava = false;
  memset(g_cells, 0, sizeof(g_cells));
  if (ppu->mode != 1 || !animated_tile_vram_addr)
    return;
  for (int layer = 0; layer < 2; layer++) {
    // Tile animati del fondale: 32 caratteri caricati all'indirizzo
    // animated_tile_vram_addr (acqua, lava, fiamme a seconda del luogo)
    int first = (animated_tile_vram_addr - ppu->bgLayer[layer].tileAdr) >> 4;
    if (first < 0 || first > 0x3ff)
      continue;
    for (int cy = 0; cy < g_cells_h; cy++) {
      for (int cx = 0; cx < g_cells_w; cx++) {
        int sx = cx * 8 + 4, sy = cy * 8 + 4;
        if (sx >= width || sy >= height)
          continue;
        int ch = BgTileAt(ppu, layer, sx - ext, sy, bias_x, bias_y) & 0x3ff;
        if (ch < first || ch >= first + 32)
          continue;
        uint32 c = ((const uint32 *)(pixels + sy * pitch))[sx];
        if (IsHud(c) || IsSprite(c))
          continue;
        int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
        if (r > 120 && r > g + 30 && b < r / 2)
          g_cells[cy][cx] = kCellLava, g_any_lava = true;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 7. Calore sopra la lava: l'aria tremola appena, con un'onda ancorata al mondo
// (non scivola quando la telecamera si sposta) e spostamenti sotto il pixel

uint32 g_fx_frames;  // contatore dei frame di gioco che non si azzera (frame_counter e' a 8 bit)

// (k 0-1: il risultato sta fra i due colori)
static inline uint32 MixColor(uint32 a, uint32 b, float k) {
  return HdFx_MixRgb(a, (b >> 16) & 0xff, (b >> 8) & 0xff, b & 0xff, k);
}

static void ApplyHeatHaze(uint8 *pixels, int pitch, int width, int height, int cam_y) {
  // intensita' per cella: piena sulla lava, sfuma nelle due file sopra
  static float mask[kMaxCellsY][kMaxCellsX];
  memset(mask, 0, sizeof(mask));
  static const float kFade[3] = { 1.0f, 0.55f, 0.25f };
  for (int cy = 0; cy < g_cells_h; cy++)
    for (int cx = 0; cx < g_cells_w; cx++)
      if (g_cells[cy][cx] == kCellLava)
        for (int k = 0; k <= 2 && cy - k >= 0; k++)
          if (mask[cy - k][cx] < kFade[k])
            mask[cy - k][cx] = kFade[k];
  static uint32 tmp[kPpuXPixels + 8];
  float t = g_fx_frames * 0.06f;
  for (int y = 0; y < height; y++) {
    // riga dell'intensita' interpolata fra i centri delle celle
    float fy = (y - 3.5f) / 8.0f;
    int cy0 = (int)floorf(fy), cy1 = cy0 + 1;
    float ky = fy - cy0;
    if (cy0 >= g_cells_h)
      break;
    float wave = sinf((y + cam_y) * 0.30f + t) * 0.8f + sinf((y + cam_y) * 0.11f - t * 0.7f) * 0.35f;
    uint32 *row = (uint32 *)(pixels + y * pitch);
    memcpy(tmp, row, width * 4);
    for (int x = 0; x < width; x++) {
      float fx = (x - 3.5f) / 8.0f;
      int cx0 = (int)floorf(fx), cx1 = cx0 + 1;
      float kx = fx - cx0;
      float m00 = (cy0 >= 0 && cx0 >= 0) ? mask[cy0][cx0] : 0;
      float m01 = (cy0 >= 0 && cx1 < g_cells_w) ? mask[cy0][cx1] : 0;
      float m10 = (cy1 < g_cells_h && cx0 >= 0) ? mask[cy1][cx0] : 0;
      float m11 = (cy1 < g_cells_h && cx1 < g_cells_w) ? mask[cy1][cx1] : 0;
      float m = (m00 * (1 - kx) + m01 * kx) * (1 - ky) + (m10 * (1 - kx) + m11 * kx) * ky;
      if (m < 0.02f || IsHud(tmp[x]) || IsSprite(tmp[x]))
        continue;  // Link e i nemici non si deformano
      // spostamento fino a poco piu' di un pixel, fra due pixel vicini
      float shift = wave * m;
      int s0 = (int)floorf(shift);
      float k = shift - s0;
      int xa = x + s0, xb = xa + 1;
      if (xa < 0 || xb >= width || IsHud(tmp[xa]) || IsSprite(tmp[xa]) || IsHud(tmp[xb]) || IsSprite(tmp[xb]))
        continue;
      row[x] = MixColor(tmp[xa], tmp[xb], k);
    }
  }
}

void HdFx_BeforeLight(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  int mod;
  g_any_lava = false;
  if (!g_fx_heat || !HdFx_InGameplay(&mod))
    return;
  ClassifyCells(pixels, pitch, width, height, bias_x, bias_y);
  if (g_any_lava)
    ApplyHeatHaze(pixels, pitch, width, height, BG2VOFS_copy2 + bias_y);
}

// ---------------------------------------------------------------------------
// Particelle (braci e pulviscolo), in coordinate del mondo

typedef struct Particle {
  float x, y, vx, vy;
  uint16 age, life;
  uint8 kind;  // 0 = brace, 1 = pulviscolo, 2 = spruzzo di pioggia, 3 = fumo dell'incendio, 4 = bagliore di un vortice
  int8 dir;    // brace di una torcia: a sinistra (-1) o a destra (1) dalle pareti laterali, in basso (2) dalla parete sud
} Particle;

enum { kMaxParticles = 700 };
static Particle g_particles[kMaxParticles];
static int g_num_particles;

static Particle *NewParticle() {
  if (g_num_particles >= kMaxParticles)
    return NULL;
  Particle *p = &g_particles[g_num_particles++];
  memset(p, 0, sizeof(*p));
  return p;
}

static void UpdateParticles() {
  int n = 0;
  for (int i = 0; i < g_num_particles; i++) {
    Particle *p = &g_particles[i];
    if (++p->age >= p->life)
      continue;
    p->x += p->vx, p->y += p->vy;
    if (p->kind == 0 && (p->dir == -1 || p->dir == 1))
      p->vy += (Rand01() - 0.5f) * 0.06f;  // di lato ondeggiano in su e in giu'
    else if (p->kind == 0 || p->kind == 4)
      p->vx += (Rand01() - 0.5f) * 0.06f;  // le braci ondeggiano salendo
    else if (p->kind == 1)
      p->vx += (Rand01() - 0.5f) * 0.01f, p->vy += (Rand01() - 0.5f) * 0.01f;
    g_particles[n++] = *p;
  }
  g_num_particles = n;
}

static void DrawParticles(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y, int ext) {
  for (int i = 0; i < g_num_particles; i++) {
    Particle *p = &g_particles[i];
    int x = (int)floorf(p->x - cam_x + ext + 0.5f), y = (int)floorf(p->y - cam_y + 0.5f);
    if (x < 1 || y < 1 || x >= width - 1 || y >= height - 1)
      continue;
    // le fasce nere fuori dalla stanza si ripuliscono dopo (ClearHiddenBands)
    float k = (float)p->age / p->life;
    uint32 *c = (uint32 *)(pixels + y * pitch) + x;
    if (IsHud(*c))
      continue;
    if (p->kind == 0) {
      // brace: dal giallo al rosso mentre si spegne
      // nucleo luminoso con un piccolo alone a croce, ben visibile anche con lo shader
      float a = k < 0.15f ? k / 0.15f : (1.0f - k) / 0.85f;
      uint32 *up = (uint32 *)((uint8 *)c - pitch), *dn = (uint32 *)((uint8 *)c + pitch);
      AddColor(c, 255, 225 - 140 * k, 120 - 100 * k, a * 1.2f);
      AddColor(c - 1, 255, 140 - 60 * k, 40, a * 0.55f);
      AddColor(c + 1, 255, 140 - 60 * k, 40, a * 0.55f);
      AddColor(up, 255, 140 - 60 * k, 40, a * 0.55f);
      AddColor(dn, 255, 140 - 60 * k, 40, a * 0.55f);
      MarkSharp(c), MarkSharp(c - 1), MarkSharp(c + 1), MarkSharp(up), MarkSharp(dn);
    } else if (p->kind == 4) {
      // bagliore magico di un vortice: come una brace, ma bianco azzurrino
      float a = k < 0.15f ? k / 0.15f : (1.0f - k) / 0.85f;
      uint32 *up = (uint32 *)((uint8 *)c - pitch), *dn = (uint32 *)((uint8 *)c + pitch);
      AddColor(c, 255, 255, 255, a * 1.2f);
      AddColor(c - 1, 170 + 40 * k, 200 + 20 * k, 255, a * 0.55f);
      AddColor(c + 1, 170 + 40 * k, 200 + 20 * k, 255, a * 0.55f);
      AddColor(up, 170 + 40 * k, 200 + 20 * k, 255, a * 0.55f);
      AddColor(dn, 170 + 40 * k, 200 + 20 * k, 255, a * 0.55f);
      MarkSharp(c), MarkSharp(c - 1), MarkSharp(c + 1), MarkSharp(up), MarkSharp(dn);
    } else if (p->kind == 1) {
      // pulviscolo nella luce: compare e scompare lentamente
      float a = sinf(k * 3.14159f) * 0.45f;
      AddColor(c, 255, 245, 215, a);
      MarkSharp(c);
    } else if (p->kind == 3) {
      // fumo dell'incendio: sbuffo grigio chiaro che sale, si allarga e svanisce
      float a = (k < 0.15f ? k / 0.15f : (1.0f - k) / 0.85f) * 0.40f;
      float r = 1.5f + 3.5f * k;
      int ir = (int)ceilf(r);
      for (int dy = -ir; dy <= ir; dy++) {
        for (int dx = -ir; dx <= ir; dx++) {
          float d2 = (float)(dx * dx + dy * dy);
          int qx = x + dx, qy = y + dy;
          if (d2 > r * r || qx < 0 || qy < 0 || qx >= width || qy >= height)
            continue;
          uint32 *q = (uint32 *)(pixels + qy * pitch) + qx;
          if (!IsHud(*q))
            MixPixel(q, 214, 208, 198, a * (1.0f - 0.5f * sqrtf(d2) / r));
        }
      }
    } else {
      // spruzzo di pioggia: piccola croce che svanisce
      float a = (1.0f - k) * 0.45f;
      AddColor(c - 1, 190, 210, 255, a);
      AddColor(c + 1, 190, 210, 255, a);
      AddColor((uint32 *)((uint8 *)c - pitch), 190, 210, 255, a * 0.7f);
      MarkSharp(c - 1), MarkSharp(c + 1);
    }
  }
}

// ---------------------------------------------------------------------------
// 1. Braci che salgono dalle fiamme

// Vista dall'alto: una brace che sale tanto sembra scivolare sul pavimento.
// Ogni brace (torce, bracieri, fiamme) fa al massimo kEmberReach pixel in
// qualunque direzione: la vita si accorcia secondo la velocita', cosi' si
// spegne sfumando come prima, solo piu' vicino alla fiamma
enum { kEmberReach = 20 };

static void LimitEmberReach(Particle *p) {
  float sp = sqrtf(p->vx * p->vx + p->vy * p->vy);
  int life = sp > 0.01f ? (int)(kEmberReach / sp) : p->life;
  if (p->life > life)
    p->life = (uint16)IntMax(life, 8);
}

static void SpawnEmbers(int cam_x, int cam_y, int ext) {
  for (int i = 0; i < g_hd_num_fire; i++) {
    HdFire *f = &g_hd_fire[i];
    if (Rand01() > 0.09f * f->strength)
      continue;
    Particle *p = NewParticle();
    if (!p)
      return;
    p->kind = 0;
    p->dir = f->dir;
    p->life = 45 + (int)(Rand01() * 55);
    if (f->dir == 2) {
      // torcia sulla parete sud, rivolta in basso: le braci escono sotto la
      // fiamma e scendono ondeggiando
      p->x = f->x - ext + cam_x + (Rand01() - 0.5f) * 8;
      p->y = f->y + cam_y + 4 + (Rand01() - 0.5f) * 4;
      p->vx = (Rand01() - 0.5f) * 0.3f;
      p->vy = 0.40f + Rand01() * 0.50f;
      LimitEmberReach(p);
      continue;
    }
    if (f->dir) {
      // torcia su una parete laterale: le braci escono dal lato della
      // fiamma, salendo appena
      p->x = f->x - ext + cam_x + f->dir * 4 + (Rand01() - 0.5f) * 4;
      p->y = f->y + cam_y + (Rand01() - 0.5f) * 8;
      p->vx = f->dir * (0.40f + Rand01() * 0.50f);
      p->vy = -0.05f - Rand01() * 0.12f;
      LimitEmberReach(p);
      continue;
    }
    p->x = f->x - ext + cam_x + (Rand01() - 0.5f) * 8;
    p->y = f->y + cam_y - 4 + (Rand01() - 0.5f) * 4;
    p->vx = (Rand01() - 0.5f) * 0.3f;
    p->vy = -0.40f - Rand01() * 0.50f;
    LimitEmberReach(p);
  }
}

// Bagliori magici dai vortici di teletrasporto (sprite 0xBA), cosi' non si
// confondono con le increspature degli Zora che stanno per uscire dall'acqua.
// Nell'area 0x1b (castello, 2x2 schermate del Mondo della Luce) il vortice
// non si disegna: niente bagliori; nemmeno se e' coperto dal ghiaccio (non
// gira e non trasporta). Col mondo continuo il vortice puo' essere di
// un'area vicina: conta la sua posizione, non l'area di Link
static void SpawnWhirlpoolSparkles() {
  // durante i passaggi (specchio, vortice, cambio d'area) niente scintille:
  // il vortice invisibile del castello resta vivo anche quando l'area e' gia'
  // la Piramide e le scintille apparivano nel punto d'arrivo
  if (submodule_index)
    return;
  for (int k = 0; k < 16; k++) {
    if (sprite_state[k] != 9 || sprite_type[k] != 0xba || Rand01() > 0.12f)
      continue;
    int sx = sprite_x_hi[k] << 8 | sprite_x_lo[k], sy = sprite_y_hi[k] << 8 | sprite_y_lo[k];
    // centro del disegno: Sprite_BA_Whirlpool lo sposta di 5 pixel a sinistra
    float cx = sx + 3.0f, cy = sy + 8.0f;
    if ((sx >> 9) >= 3 && (sx >> 9) <= 4 && (sy >> 9) >= 3 && (sy >> 9) <= 4)
      continue;
    if (HdBurn_IsFrozenAt(sx + 8, sy + 8))
      continue;
    Particle *p = NewParticle();
    if (!p)
      return;
    float ang = Rand01() * 6.2832f, r = 2.0f + Rand01() * 5.0f;
    p->kind = 4;
    p->life = 40 + (int)(Rand01() * 45);
    p->x = cx + cosf(ang) * r;
    p->y = cy + sinf(ang) * r * 0.6f;
    p->vx = (Rand01() - 0.5f) * 0.3f;
    p->vy = -0.35f - Rand01() * 0.45f;
  }
}

// ---------------------------------------------------------------------------
// 3. Raggi di luce e pulviscolo dalle finestre

static bool IsLightTile(const uint16 *light, uint16 v) {
  for (int i = 0; i < 16; i++)
    if (light[i] == v)
      return true;
  return false;
}

static bool IsFloorLightTile(uint16 v, int pal) {
  int t = v & 0x3ff;
  return ((v >> 10) & 7) == pal && (t == 0x144 || t == 0x145 || t == 0x154 || t == 0x155 || t == 0x176 || t == 0x12d);
}

typedef struct WindowShaft {
  uint16 x, y, w, h;  // zona di luce della finestra, in pixel nella stanza
  uint8 layer;        // 0 = BG2, 1 = BG1
  uint8 floor;        // 1 = luce sul pavimento: il fascio scende dall'alto fin li'
} WindowShaft;

enum { kMaxShafts = 8 };
static WindowShaft g_shafts[kMaxShafts];
static int g_num_shafts, g_shaft_room = -1, g_shaft_timer;

// La luce di una finestra ("Window Light", oggetto 0x70 del terzo gruppo) e'
// una zona rettangolare di tile di luce, spesso tutti uguali: ogni zona
// (angolo in alto a sinistra senza luce a sinistra e sopra) diventa un fascio.
static void ScanWindows() {
  g_num_shafts = 0;
  g_shaft_room = dungeon_room_index;
  const uint16 *src = Dungeon_GetObjectTiles(3, 0x70);
  uint16 light[16];
  for (int i = 0; i < 16; i++)
    light[i] = src[i];
  for (int layer = 0; layer < 2; layer++) {
    const uint16 *tm = layer ? dung_bg1 : dung_bg2;
#define IS_LIGHT(r, c) ((r) >= 0 && (c) >= 0 && (r) < 64 && (c) < 64 && IsLightTile(light, tm[(r) * 64 + (c)]))
    for (int row = 0; row < 64; row++) {
      for (int col = 0; col < 64; col++) {
        if (!IS_LIGHT(row, col) || IS_LIGHT(row, col - 1) || IS_LIGHT(row - 1, col))
          continue;
        int w = 1, h = 1;
        while (w < 8 && IS_LIGHT(row, col + w))
          w++;
        while (h < 16 && IS_LIGHT(row + h, col))
          h++;
        if (w < 2 || h < 3 || g_num_shafts >= kMaxShafts)
          continue;
        WindowShaft *sh = &g_shafts[g_num_shafts++];
        sh->x = col * 8, sh->y = row * 8, sh->w = w * 8, sh->h = h * 8, sh->layer = layer, sh->floor = 0;
      }
    }
#undef IS_LIGHT
  }
  // Luce sul pavimento (oggetto 0x70 + 1, es. Villaggio dei Ladroni): un
  // riquadro con l'angolo 0x144 in alto a sinistra, illuminato da sopra
  int pal = (Dungeon_GetObjectTiles(3, 0x71)[0] >> 10) & 7;
  for (int layer = 0; layer < 2; layer++) {
    const uint16 *tm = layer ? dung_bg1 : dung_bg2;
#define IS_FLOOR(r, c) ((r) >= 0 && (c) >= 0 && (r) < 64 && (c) < 64 && IsFloorLightTile(tm[(r) * 64 + (c)], pal))
    for (int row = 0; row < 64; row++) {
      for (int col = 0; col < 64; col++) {
        if ((tm[row * 64 + col] & 0x3ff) != 0x144 || !IS_FLOOR(row, col) || IS_FLOOR(row, col - 1) || IS_FLOOR(row - 1, col))
          continue;
        int w = 1, h = 1;
        while (w < 16 && IS_FLOOR(row, col + w))
          w++;
        while (h < 16 && IS_FLOOR(row + h, col))
          h++;
        if (w < 2 || h < 2 || g_num_shafts >= kMaxShafts)
          continue;
        WindowShaft *sh = &g_shafts[g_num_shafts++];
        sh->x = col * 8, sh->y = row * 8, sh->w = w * 8, sh->h = h * 8, sh->layer = layer, sh->floor = 1;
      }
    }
#undef IS_FLOOR
  }
}

// Fascio verticale che scende dal soffitto e si posa sul riquadro di luce
// (x0, y0 sullo schermo, w x h pixel): dritto da nord a sud, piu' forte
// vicino al pavimento
static void ApplyFloorShaft(uint8 *pixels, int pitch, int width, int height, float x0, float y0, int w, int h,
                            float t, bool spawn, int cam_x, int cam_y, int ext) {
  const int kTop = 72;  // altezza del fascio sopra il riquadro
  for (int dy = -kTop; dy < h; dy++) {
    int y = (int)y0 + dy;
    if (y < 0 || y >= height)
      continue;
    uint32 *row = (uint32 *)(pixels + y * pitch);
    // si accende scendendo, pieno sul riquadro e sfuma sul bordo basso
    float fall = dy < 0 ? powf((float)(dy + kTop) / kTop, 1.5f) : 1.0f - IntMax(0, dy - (h - 6)) / 6.0f;
    for (int dx = -6; dx < w + 6; dx++) {
      float rx = (float)dx;
      float edge = rx < 0 ? 1.0f + rx / 6.0f : rx > w ? 1.0f - (rx - w) / 6.0f : 1.0f;
      if (edge <= 0)
        continue;
      int x = (int)x0 + dx;
      if (x < 0 || x >= width || IsHud(row[x]))
        continue;
      float streak = 0.75f + 0.25f * sinf(rx * 0.55f + t * 3.0f) * sinf(rx * 0.21f - t * 2.0f);
      AddColor(&row[x], 255, 240, 205, (dy < 0 ? 0.24f : 0.12f) * fall * edge * streak);
    }
  }
  if (spawn && Rand01() < 0.10f) {
    Particle *p = NewParticle();
    if (p) {
      p->kind = 1;
      p->x = x0 - ext + cam_x + Rand01() * w;
      p->y = y0 + cam_y - kTop + 8 + Rand01() * (kTop + h - 16);
      p->vx = (Rand01() - 0.5f) * 0.08f;
      p->vy = (Rand01() - 0.5f) * 0.06f;
      p->life = 90 + (int)(Rand01() * 90);
    }
  }
}

static void ApplyWindowRays(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y, int ext, bool spawn) {
  int room = dungeon_room_index;
  if (g_shaft_room != room || --g_shaft_timer <= 0) {
    ScanWindows();
    g_shaft_timer = 30;
  }
  float t = g_fx_frames * 0.02f;
  for (int i = 0; i < g_num_shafts; i++) {
    WindowShaft *sh = &g_shafts[i];
    // posizione sullo schermo con lo scorrimento del layer su cui sta la luce
    int hs = (sh->layer ? BG1HOFS_copy2 : BG2HOFS_copy2) + (cam_x - BG2HOFS_copy2);
    int vs = (sh->layer ? BG1VOFS_copy2 : BG2VOFS_copy2) + (cam_y - BG2VOFS_copy2);
    float x0 = sh->x - (hs & 0x1ff) + ext, y0 = sh->y - (vs & 0x1ff);
    int kW = sh->w, kLen = sh->h + 40;
    if (sh->floor) {
      ApplyFloorShaft(pixels, pitch, width, height, x0, y0, kW, sh->h, t, spawn, cam_x, cam_y, ext);
      continue;
    }
    for (int dy = 0; dy < kLen; dy++) {
      int y = (int)y0 + dy;
      if (y < 0 || y >= height)
        continue;
      uint32 *row = (uint32 *)(pixels + y * pitch);
      // entra piano dall'alto e si spegne sotto la zona di luce originale
      float fall = IntMin(dy, 8) / 8.0f * powf(1.0f - (float)dy / kLen, 0.8f);
      // il fascio si allarga un po' verso il basso e pende leggermente
      float skew = dy * 0.12f, spread = dy * 0.10f;
      for (int dx = -8; dx < kW + 8 + (int)skew; dx++) {
        float rx = dx - skew;
        float edge = 1.0f;
        if (rx < -spread * 0.5f || rx > kW + spread * 0.5f) {
          float d = rx < 0 ? -spread * 0.5f - rx : rx - kW - spread * 0.5f;
          edge = 1.0f - d / 6.0f;
          if (edge <= 0)
            continue;
        }
        int x = (int)x0 + dx;
        if (x < 0 || x >= width || IsHud(row[x]))
          continue;
        // strisce di luce piu' intense che tremolano lentamente
        float streak = 0.75f + 0.25f * sinf(rx * 0.55f + t * 3.0f) * sinf(rx * 0.21f - t * 2.0f);
        AddColor(&row[x], 255, 240, 205, 0.26f * fall * edge * streak);
      }
    }
    // pulviscolo che fluttua nel fascio (in coordinate del mondo)
    if (spawn && Rand01() < 0.10f) {
      Particle *p = NewParticle();
      if (p) {
        p->kind = 1;
        p->x = x0 - ext + cam_x + Rand01() * kW;
        p->y = y0 + cam_y + 8 + Rand01() * (kLen - 16);
        p->vx = (Rand01() - 0.5f) * 0.08f;
        p->vy = (Rand01() - 0.5f) * 0.06f;
        p->life = 90 + (int)(Rand01() * 90);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 5. Bagliore a raggi quando Link solleva un oggetto ricevuto

static void ApplyItemGlow(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y, int ext) {
  for (int k = 0; k < kAncilla_Slots; k++) {
    int type = ancilla_type[k];
    if (type != 0x22 && type != 0x29 && type != 0x35)
      continue;  // oggetto ricevuto, Amuleto/Cristallo, Spada Suprema
    // Centro dell'oggetto come lo disegna il gioco (Ancilla_ReceiveItem_Draw):
    // largo 16 pixel, oppure 8 (spade, bacchette...) se kReceiveItem_Tab1 e' 0;
    // alto 16; gli Amuleti e i Cristalli si sollevano di ancilla_z; gli sprite
    // compaiono una riga sotto la loro coordinata.
    int item = ancilla_item_to_link[k];
    float half_w = (type == 0x35 || (item < 76 && kReceiveItem_Tab1[item])) ? 8.0f : 4.0f;
    float lift = (type == 0x29) ? (float)(int8)ancilla_z[k] : 0.0f;
    float cx = (ancilla_x_hi[k] << 8 | ancilla_x_lo[k]) + half_w - cam_x + ext;
    float cy = (ancilla_y_hi[k] << 8 | ancilla_y_lo[k]) + 9 - lift - cam_y;
    float big = (type == 0x22) ? 1.0f : 1.5f;
    float radius = 52 * big;
    float rot = g_fx_frames * 0.03f;
    int x0 = IntMax((int)(cx - radius), 0), x1 = IntMin((int)(cx + radius), width - 1);
    int y0 = IntMax((int)(cy - radius), 0), y1 = IntMin((int)(cy + radius), height - 1);
    for (int y = y0; y <= y1; y++) {
      uint32 *row = (uint32 *)(pixels + y * pitch);
      for (int x = x0; x <= x1; x++) {
        if (IsHud(row[x]) || IsSprite(row[x]))
          continue;  // i raggi stanno dietro a Link e all'oggetto
        float dx = x - cx, dy = y - cy;
        float d = sqrtf(dx * dx + dy * dy) / radius;
        if (d >= 1.0f)
          continue;
        float a = atan2f(dy, dx);
        float ray = powf(fabsf(cosf(a * 4.0f + rot)), 12.0f);
        float core = (1.0f - d) * (1.0f - d);
        float v = (ray * 0.9f + core * 0.5f) * (1.0f - d);
        AddColor(&row[x], 255, 235, 160, v * 0.75f);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 8. Pioggia piu' fitta con spruzzi e lampi che illuminano la scena

typedef struct RainDrop {
  float x, y, speed, len;
} RainDrop;

enum { kNumDrops = 220 };
static RainDrop g_drops[kNumDrops];
static bool g_drops_init;
static float g_flash;
static bool g_flash_rise;

static bool IsRaining(int mod) {
  if (mod != 9)
    return false;
  int si = BYTE(overworld_screen_index);
  if (si == 0x70)
    return !(save_ow_event_info[0x70] & 0x20);  // Palude della Miseria
  // Prima del salvataggio di Zelda piove nel Mondo della Luce, tranne dove ci
  // sono le nuvole del Monte Morte
  return sram_progress_indicator < 2 && si != 3 && si != 5 && si != 7;
}

// Piove nell'area esterna attuale (anche con l'effetto della pioggia spento):
// sotto la pioggia il fuoco non attecchisce (hd_burn.c)
bool HdFx_IsRainingOutdoors(void) {
  return !player_is_indoors && IsRaining(9);
}

// Acqua: i pixel del fondale che cadono su caselle d'acqua (profonda 0x08,
// bassa 0x09) vengono marcati con il layer 7, cosi' lo shader sa dove non
// mettere le pennellate (in Four Swords l'acqua non le ha).
enum { kLayerWater = 7 };

static bool IsWaterAttr(uint8 a) {
  return a == 0x08 || a == 0x09;
}

// Aree in cui lo sfondo e' il cielo con il panorama sotto le nuvole: la
// Montagna della Morte (chiara e oscura) e la Piramide
static bool IsBackdropScene(void) {
  int a = BYTE(overworld_screen_index), sc = a & 0x3f;
  return a < 0x80 && (sc == 0x03 || sc == 0x05 || sc == 0x07 || a == 0x5b);
}

// Montagna della Morte (chiara e oscura): le nuvole sotto i dirupi sono
// caselle del terreno con la palette 3. Come l'acqua non hanno pennellate
// (lo shader le dipingerebbe come terra); vale anche per le immagini delle
// aree vicine (hd_world.c)
static bool IsMountainScene(void) {
  int a = BYTE(overworld_screen_index), sc = a & 0x3f;
  return a < 0x80 && (sc == 0x03 || sc == 0x05 || sc == 0x07);
}

// Colore v della casella e (della mappa) sulla montagna: le caselle con la
// palette 3 sono il bordo delle nuvole (tutta la casella); in quelle con la
// palette 7 l'interno delle nuvole sono solo i due rosa (colori 13 e 15), gli
// altri colori sono ponti, pilastri e dirupi. Nel Mondo delle Tenebre le
// nuvole grigie sono le stesse caselle dell'interno (0x165, 0x175) con la
// palette 4, e il loro bordo (l'ombra) sono le caselle del bordo rosa
// (0x1c6-0x1cd) con la palette 7, tutta la casella
static bool IsGreyCloudBorder(uint16 e) {
  int t = e & 0x3ff;
  return (e >> 10 & 7) == 7 && t >= 0x1c6 && t <= 0x1cd;
}

bool HdFx_IsCloudTile(uint16 e) {
  int pal = e >> 10 & 7, t = e & 0x3ff;
  return pal == 3 || pal == 7 || (pal == 4 && (t == 0x165 || t == 0x175));
}

bool HdFx_IsCloudColor(uint16 e, int v) {
  int pal = e >> 10 & 7;
  return HdFx_IsCloudTile(e) && (pal == 3 || IsGreyCloudBorder(e) || v == 13 || v == 15);
}

// Pixel di nuvola nella riga row della casella e della mappa (bit 7 = primo
// pixel a sinistra), letti dalla grafica in VRAM come fa la PPU
static uint8 CloudRowMask(uint16 e, int row) {
  int pal = e >> 10 & 7;
  if (pal == 3)
    return 0xff;
  if (!HdFx_IsCloudTile(e))
    return 0;
  Ppu *ppu = g_zenv.ppu;
  const uint16 *t = &ppu->vram[(ppu->bgLayer[1].tileAdr + (e & 0x3ff) * 16) & 0x7fff];
  int rr = (e & 0x8000) ? 7 - row : row;
  uint16 p01 = t[rr], p23 = t[rr + 8];
  uint8 mask = 0;
  for (int col = 0; col < 8; col++) {
    int bit = (e & 0x4000) ? col : 7 - col;
    int v = HdFx_Pixel4bpp(p01, p23, bit);
    if (HdFx_IsCloudColor(e, v))
      mask |= 0x80 >> col;
  }
  return mask;
}

int HdFx_SceneModule(void) {
  int m = main_module_index, sub = submodule_index;
  if (m == 14) {
    // inventario (1), dialoghi (2) e menu salva/esci (11) si aprono sopra il
    // gioco, che resta sotto
    if (sub == 1 || sub == 2 || sub == 11)
      return saved_module_for_menu;
    // pozioni (4 rossa, 8 verde, 9 blu e d'oro) e preghiera nel deserto (5):
    // il gioco resta in vista mentre si riempiono cuori e magia (prima
    // sparivano acqua, shader, luci e particelle per tutta la ricarica)
    if (sub == 4 || sub == 5 || sub == 8 || sub == 9)
      return saved_module_for_menu;
    // Ocarina: prima (0) il mondo si scurisce, dalla fase 7 l'area di arrivo
    // e' gia' caricata e compare, poi scende l'uccello. In mezzo c'e' la mappa
    if (sub == 10 && (overworld_map_state == 0 || overworld_map_state >= 7))
      return 9;
    // mappa del mondo: nella fase 0 il mondo si scurisce ancora sotto
    if (sub == 7 && overworld_map_state == 0)
      return saved_module_for_menu;
    // mappa del labirinto: il labirinto si scurisce (0) e, alla chiusura, si
    // ricarica e si riaccende (dalla fase 6)
    if (sub == 3 && (overworld_map_state == 0 || overworld_map_state >= 6))
      return saved_module_for_menu;
    return 14;
  }
  // cerchio che si chiude (15) o si riapre (16): la scena dentro o fuori
  if (m == 15 || m == 16)
    return player_is_indoors ? 7 : 9;
  // caduta all'ingresso di un labirinto (Module11: buche all'aperto, Link preso
  // dalla Mano): nelle fasi 0-1 si scurisce ancora la scena di prima, dalla
  // fase 3 la stanza nuova e' gia' caricata e si vede mentre Link cade
  if (m == 17)
    return subsubmodule_index >= 3 || player_is_indoors ? 7 : 9;
  // vittoria su un boss (19 pendente, 22 cristallo): la stanza resta in vista
  // mentre si riempiono cuori e magia, Link gira e lo schermo si scurisce
  if ((m == 19 || m == 22) && player_is_indoors)
    return 7;
  // Salva ed esci (23): il gioco si scurisce fermo
  if (m == 23)
    return player_is_indoors ? 7 : 9;
  // morte di Link (18): finche' si vede il mondo e' la scena di prima
  if (HdFx_DeathShowsWorld())
    return saved_module_for_menu;
  // (transizione dello specchio, 9/35 e 9/44: le aggiunte HD restano e
  // seguono onda e bianco del gioco, HdWorld_WarpPost in hd_world.c)
  return m;
}

// Morte di Link (modulo 18): il mondo resta sullo schermo mentre Link cade e
// il cerchio si chiude, poi il gioco spegne BG1 e BG2 (TM, schermo rosso e
// nero) e li riaccende quando la fata lo rianima. Shader, effetti, schermo
// largo e mondo continuo seguono il mondo: niente salti
bool HdFx_DeathShowsWorld(void) {
  return main_module_index == 18 && (TM_copy & 3) != 0;
}

// Il gioco e' in vista ma fermo sotto qualcos'altro (menu, dialoghi, pozioni,
// preghiera, cerchio che si chiude, Mano e buche che si scuriscono, vittorie,
// Salva ed esci): la vista resta dov'e'. Non vale quando arriva una scena
// nuova (cerchio che si apre, 17 dalla fase 3): li' la vista riparte
bool HdFx_GameHeldInView(void) {
  int m = main_module_index, scene = HdFx_SceneModule();
  if (m == 7 || m == 9 || m == 11 || m == 16 || (m == 17 && subsubmodule_index >= 3))
    return false;
  return scene == 7 || scene == 9 || scene == 11;
}

bool HdFx_LiveOverworld(void) {
  return main_module_index == 9 && submodule_index == 0;
}

bool HdFx_GameLoaded(void) {
  return main_module_index > 5 && main_module_index != 20;
}

bool HdFx_DungeonPassing(void) {
  return main_module_index == 7 && submodule_index != 0;
}

bool HdFx_RoomScroll(void) {
  return main_module_index == 7 && (submodule_index == 1 || submodule_index == 2);
}

// Allagamento della diga: l'acqua del piano di sopra si vede solo dentro la
// finestra (per riga sinistra e destra in hdma_table_dynamic) che scende.
// Con lo schermo alto 240 righe la PPU usa la tabella anche per le righe
// 225-239 (AdjustWaterHDMAWindow e FloodDam_PrepFloodHDMA la riempiono fino
// a 240): leggendo solo fino alla 224 l'acqua della diga restava quella del
// gioco nelle ultime righe, in basso
bool HdFx_FloodWindowShows(int sx, int sy) {
  sy = sy < 0 ? 0 : sy > 239 ? 239 : sy;
  sx = sx < 0 ? 0 : sx > 255 ? 255 : sx;
  uint16 v = hdma_table_dynamic[sy];
  return sx >= (v & 0xff) && sx <= (v >> 8);
}

void HdFx_MarkWater(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  // anche durante il cerchio di transizione e l'arrivo con l'Ocarina:
  // altrimenti lo shader dipingerebbe l'acqua per qualche fotogramma
  // 11: aree speciali all'aperto (Dominio di Zora, bosco della Spada Suprema...)
  HdWater_PrepareAttrs();
  int mod = HdFx_SceneModule();
  if (mod != 7 && mod != 9 && mod != 11)
    return;
  // sulla Montagna della Morte e alla Piramide il vuoto oltre i dirupi e'
  // "acqua profonda" per il gioco, ma sullo schermo e' il cielo: lo sfondo
  // resta sfondo (HdFx_MarkBackdrop), invece di diventare acqua a blocchi
  bool sky = mod == 9 && IsBackdropScene();
  bool mountain = mod == 9 && IsMountainScene();
  int ext = (width - 256) / 2;
  // piano di sopra solo nello schermo secondario, sommato in trasparenza al
  // pavimento (stanza della diga): i suoi pixel risultano del pavimento (BG2)
  bool bg1_sub = mod == 7 && (TS_copy & 1) && !(TM_copy & 1);
  // (non nel cerchio che si apre o si chiude: la finestra e' il cerchio, e
  // dopo lo specchio l'acqua HD arrivava solo alla fine)
  bool iris = main_module_index == 15 || main_module_index == 16 || (main_module_index == 7 && submodule_index == 15);
  bool flood_win = mod == 7 && !iris && (TSW_copy & 1) && (W12SEL_copy & 2);
  // leva della diga appena tirata (RoomTag_WaterGate): finestra e allagamento
  // sono gia' accesi, ma la tabella della finestra la prepara
  // FloodDam_PrepFloodHDMA solo dal fotogramma dopo. Per questo fotogramma come
  // prima della leva (tutto il canale diventava acqua per un attimo)
  bool gate_start = main_module_index == 7 && submodule_index == 13 && subsubmodule_index == 0;
  if (gate_start)
    flood_win = false;
  // leva che svuota la vasca (Module07_0B): le caselle della scala sul
  // pavimento restano acqua per il gioco fino alla fine, l'acqua si vede
  // solo dentro la finestra che si stringe
  bool draining = main_module_index == 7 && submodule_index == 11;
  for (int layer = 0; layer < 2; layer++) {
    // all'aperto conta solo il terreno (BG2); nei labirinti anche il piano di sopra (BG1)
    if (layer == 0 && mod != 7)
      continue;
    int cam_x = (layer ? BG2HOFS_copy2 : BG1HOFS_copy2) + bias_x;
    int cam_y = (layer ? BG2VOFS_copy2 : BG1VOFS_copy2) + bias_y;
    int x0 = (cam_x - ext) & ~7, y0 = cam_y & ~7;
    for (int wy = y0; wy < cam_y + height; wy += 8) {
      for (int wx = x0; wx < cam_x - ext + width; wx += 8) {
        uint8 attr;
        uint16 cloud_e = 0;  // casella della mappa, se puo' contenere nuvole
        bool flood = false;
        if (mod == 7) {
          int pos = ((wy & 0x1f8) << 3) + ((wx & 0x1f8) >> 3);
          // acqua della chiusa: per il gioco le caselle non cambiano
          flood = !layer && !gate_start && HdWater_IsFlood(pos);
          attr = flood ? 0x08 : HdWater_DungAttr(pos + (layer ? 0 : 0x1000), wx, wy);
        } else if (HdWorld_Active() && (HdWorld_BackgroundFromImages() || !HdWorld_InsideCurrentArea(wx, wy)))
          continue;  // fondale dalle immagini delle aree, con l'acqua gia' segnata; nel
                     // passaggio la mappa del gioco e' ancora quella dell'area vecchia
        else {
          attr = Overworld_GetTileAttributeAtLocation((uint16)wx >> 3, (uint16)wy);
          if (mountain) {
            uint16 e = Overworld_GetMap8AtLocation((uint16)wx >> 3, (uint16)wy);
            if (HdFx_IsCloudTile(e))
              cloud_e = e;
          }
        }
        bool water = IsWaterAttr(attr);
        // vasche blu nelle case: solo i pixel blu dell'interno
        bool tub = mod == 7 && layer && !water && HdWater_IsTubCell(((wy & 0x1f8) << 3) + ((wx & 0x1f8) >> 3));
        water |= tub;
        // acqua congelata (hd_burn.c): per il gioco e' ghiaccio (0x0f), ma sotto
        // le lastre (HdFx_FrozenWater) si disegna l'acqua
        if (attr == 0x0f && mod == 9 && HdBurn_IsFrozenAt((uint16)wx, (uint16)wy))
          water = true;
        if (!water && !cloud_e)
          continue;
        for (int y = wy - cam_y; y < wy - cam_y + 8; y++) {
          if (y < 0 || y >= height)
            continue;
          uint32 *row = (uint32 *)(pixels + y * pitch);
          uint8 mask = water ? 0xff : CloudRowMask(cloud_e, (y + cam_y) & 7);
          for (int x = wx - cam_x + ext; x < wx - cam_x + ext + 8; x++) {
            if (x < 0 || x >= width || !(mask & 0x80 >> ((x - ext + cam_x) & 7)))
              continue;
            // anche l'acqua del piano di sopra che si accende con la leva
            // (Palazzo della Palude): si vede solo dentro la finestra, prima
            // una colonna stretta sotto la bocca, poi tutta la vasca
            if ((flood || !layer || draining) && flood_win && !HdFx_FloodWindowShows(x - ext, y))
              continue;
            uint32 l = row[x] >> 24;
            // solo il fondale del layer giusto: HUD (2) e personaggi (4, 6) restano
            if (tub && (l != 1 || !HdWater_IsTubColor(row[x])))
              continue;
            if (l == (uint32)(layer ? 1 : 0) || (layer && l == 5 && !sky) || (!layer && bg1_sub && (l == 1 || l == 5)))
              row[x] = (row[x] & 0x00ffffff) | (uint32)kLayerWater << 24;
          }
        }
      }
    }
  }
}

// Bianco del lampo in corso (0 se non c'e'), solo mentre si vede il gioco
float HdFx_LightningWhite(void) {
  int mod;
  return g_flash >= 0.02f && HdFx_InGameplay(&mod) ? g_flash : 0.0f;
}

// All'aperto con il sole: niente pioggia, niente nebbia del Bosco Perduto
// o della Foresta Maledetta, niente nuvole del Monte della Morte (chiaro e
// oscuro), niente aree speciali come il bosco della Spada Suprema.
bool HdFx_SunnyOutdoors(void) {
  int mod;
  // Anche mentre si carica l'esterno e il cerchio si apre o si chiude
  // all'aperto (uscendo da una casa, entrandoci): altrimenti le nuvole
  // comparirebbero di colpo un attimo dopo
  // Anche con l'Ocarina: mentre il mondo si scurisce e mentre l'area di
  // arrivo compare e scende l'uccello (HdFx_SceneModule)
  bool iris = (main_module_index == 8 || main_module_index == 15 || main_module_index == 16) && !player_is_indoors;
  bool flute = main_module_index == 14 && submodule_index == 10 && HdFx_SceneModule() == 9;
  if (iris || flute)
    mod = 9;
  else if (!HdFx_InGameplay(&mod) || mod != 9)
    return false;
  if (IsRaining(mod))
    return false;
  int si = BYTE(overworld_screen_index);
  if (si >= 0x80)
    return false;
  switch (si) {
  case 0x00: case 0x03: case 0x05: case 0x07:
  case 0x40: case 0x43: case 0x45: case 0x47:
    return false;
  }
  return true;
}

static void ApplyRain(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y, int ext,
                      bool step) {
  if (!g_drops_init) {
    for (int i = 0; i < kNumDrops; i++) {
      g_drops[i].x = Rand01() * width, g_drops[i].y = Rand01() * height;
      g_drops[i].speed = 4.5f + Rand01() * 2.5f, g_drops[i].len = 6 + Rand01() * 5;
    }
    g_drops_init = true;
  }
  for (int i = 0; i < kNumDrops; i++) {
    RainDrop *d = &g_drops[i];
    if (step) {
      d->y += d->speed, d->x -= d->speed * 0.35f;
      // la goccia tocca terra in un punto a caso: spruzzo
      if (d->y > height || Rand01() < 0.012f) {
        if (d->y <= height) {
          Particle *p = NewParticle();
          if (p) {
            p->kind = 2;
            p->x = d->x - ext + cam_x, p->y = d->y + cam_y;
            p->life = 6;
          }
        }
        d->y = -Rand01() * 20, d->x = Rand01() * (width + 40);
      }
    }
    for (int j = 0; j < (int)d->len; j++) {
      int x = (int)(d->x + j * 0.35f), y = (int)(d->y - j);
      if (x < 0 || y < 0 || x >= width || y >= height)
        continue;
      uint32 *c = (uint32 *)(pixels + y * pitch) + x;
      if (!IsHud(*c)) {
        // la goccia schiarisce verso un azzurro chiaro: si vede su ogni fondo
        float k = 0.62f * (1.0f - (float)j / d->len);
        *c = HdFx_MixRgb(*c, 205, 220, 245, k);
        MarkSharp(c);
      }
    }
  }
}

static void StepLightning(bool step) {
  // Con l'effetto attivo il gioco non fa piu' i suoi lampeggi rapidi
  // (OverworldOverlay_HandleRain): resta un solo lampo, sul tuono, che si
  // accende in pochi frame e si spegne lentamente.
  if (step) {
    if (frame_counter == 36)
      g_flash_rise = true;
    if (g_flash_rise) {
      g_flash += 0.25f;
      if (g_flash >= 1.0f)
        g_flash = 1.0f, g_flash_rise = false;
    } else {
      g_flash *= 0.95f;
    }
  }
}

// Lo schiarimento del lampo va dopo l'acqua HD (hd_water.c) e le lastre di
// ghiaccio: fatto prima, l'acqua HD si disegnava sopra a un fondale gia'
// schiarito e nel lampeggio si vedeva l'acqua del gioco sotto
void HdFx_Lightning(uint8 *pixels, int pitch, int width, int height) {
  int mod;
  if (g_flash < 0.02f || !HdFx_InGameplay(&mod))
    return;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++) {
      if (IsHud(row[x]))
        continue;
      row[x] = HdFx_MixRgb(row[x], 235, 240, 255, g_flash * 0.40f);
    }
  }
}

// ---------------------------------------------------------------------------
// 7. Incendio di cespugli ed erba (hd_burn.c): le fiamme del gioco su ogni
// tile che brucia, fumo quando si spegne e terreno annerito

static inline float Hash01(uint32 v) {
  v ^= v >> 16, v *= 0x7feb352d, v ^= v >> 15, v *= 0x846ca68b, v ^= v >> 16;
  return (v & 0xffffff) * (1.0f / 16777216.0f);
}

// Annerisce la tile: s = quanto (0-1), con un bordo morbido che sborda un po'
static void DrawScorchTile(uint8 *pixels, int pitch, int width, int height, int sx, int sy, uint8 seed, float s) {
  for (int y = -2; y < 18; y++) {
    int py = sy + y;
    if (py < 0 || py >= height)
      continue;
    uint32 *row = (uint32 *)(pixels + py * pitch);
    for (int x = -2; x < 18; x++) {
      int px = sx + x;
      if (px < 0 || px >= width || IsHud(row[px]) || IsSprite(row[px]))
        continue;
      float dx = x - 7.5f, dy = y - 7.5f;
      float d = sqrtf(dx * dx + dy * dy);
      float e = d < 7.0f ? 1.0f : d > 11.0f ? 0.0f : (11.0f - d) * 0.25f;
      if (e <= 0)
        continue;
      float n = 0.65f + 0.35f * Hash01(seed * 977 + (x + 2) * 31 + (y + 2));
      MixPixel(&row[px], 48, 34, 22, s * e * n);
    }
  }
}

// Casella 8x8 degli sprite (prima tabella della memoria video) con la palette
// pal degli sprite, letta da VRAM e CGRAM come la disegnerebbe la PPU
static void DrawObjTile(uint8 *pixels, int pitch, int width, int height, int sx, int sy,
                        int chr, int pal, bool hflip, bool behind_link) {
  Ppu *ppu = g_zenv.ppu;
  int adr = ppu->objTileAdr1 + chr * 16;
  for (int r = 0; r < 8; r++) {
    int py = sy + r;
    if (py < 0 || py >= height)
      continue;
    uint32 *row = (uint32 *)(pixels + py * pitch);
    uint16 p01 = ppu->vram[(adr + r) & 0x7fff], p23 = ppu->vram[(adr + r + 8) & 0x7fff];
    for (int c = 0; c < 8; c++) {
      int px = sx + c, bit = hflip ? c : 7 - c;
      int v = HdFx_Pixel4bpp(p01, p23, bit);
      if (v == 0 || px < 0 || px >= width || IsHud(row[px]))
        continue;
      // Link (e chi e' con lui) davanti al fuoco resta visibile
      if (behind_link && IsSprite(row[px]))
        continue;
      uint16 col = ppu->cgram[0x80 + pal * 16 + v];
      uint32 cr = col & 31, cg = col >> 5 & 31, cb = col >> 10 & 31;
      row[px] = 4u << 24 | (cr << 3 | cr >> 2) << 16 | (cg << 3 | cg >> 2) << 8 | (cb << 3 | cb >> 2);
    }
  }
}

// Le fiamme del gioco: quelle dei nemici bruciati dalla Bacchetta del Fuoco
// (SpriteModule_Burn e Flame_Draw), con la stessa sequenza di fotogrammi
// e la palette 1 degli sprite, come se la tile fosse un nemico che brucia
static void DrawFlamesTile(uint8 *pixels, int pitch, int width, int height, int sx, int sy,
                           const HdBurnTile *b, bool behind_link) {
  static const uint8 kFlame_Gfx[9] = { 5, 4, 3, 1, 2, 0, 3, 0, 1 };
  static const uint8 kFlame_Char[5] = { 0x8e, 0xa0, 0x8e, 0xa0, 0xa2 };
  int j = kHdBurnTime - 1 - b->age;
  if (j < 0)
    return;
  int gfx = kFlame_Gfx[j >> 3 > 8 ? 8 : j >> 3];
  sy -= 2;  // un poco sopra la tile, come la fiamma sopra i piedi del nemico
  if (gfx == 5) {
    DrawObjTile(pixels, pitch, width, height, sx, sy - 6, 0xa4, 1, false, behind_link);
    DrawObjTile(pixels, pitch, width, height, sx + 8, sy - 6, 0xa5, 1, false, behind_link);
    return;
  }
  int chr = kFlame_Char[gfx];
  bool hflip = gfx == 2 || gfx == 3;
  for (int t = 0; t < 4; t++) {
    int tx = t & 1, ty = t >> 1;
    DrawObjTile(pixels, pitch, width, height, sx + (hflip ? 1 - tx : tx) * 8, sy + ty * 8,
                chr + tx + ty * 16, 1, hflip, behind_link);
  }
}

// Colori del ghiaccio: i colori 1-7 della palette 2 degli sprite, con cui il
// gioco disegna i nemici congelati dalla Bacchetta del Ghiaccio
// (Sprite_DrawMultiple; gli altri sono bianco, rosa e arancio), ordinati dal
// piu' scuro al piu' chiaro
static int IceRamp(uint32 *ramp) {
  Ppu *ppu = g_zenv.ppu;
  int lum[15], n = 0;
  for (int i = 1; i < 8; i++) {
    uint16 col = ppu->cgram[0x80 + 2 * 16 + i];
    uint32 cr = col & 31, cg = col >> 5 & 31, cb = col >> 10 & 31;
    uint32 c = (cr << 3 | cr >> 2) << 16 | (cg << 3 | cg >> 2) << 8 | (cb << 3 | cb >> 2);
    int l = (int)(cr * 3 + cg * 6 + cb);
    int j = n++;
    while (j > 0 && lum[j - 1] > l)
      lum[j] = lum[j - 1], ramp[j] = ramp[j - 1], j--;
    lum[j] = l, ramp[j] = c;
  }
  return n;
}

// Congela la tile: i colori diventano quelli del ghiaccio secondo la loro
// luminosita' (piu' chiari), con un bordo morbido; ogni tanto un luccichio
static void DrawFrozenTile(uint8 *pixels, int pitch, int width, int height, int sx, int sy,
                           const HdBurnTile *f, const uint32 *ramp, int nramp) {
  float grow = f->age < 8 ? (f->age + 1) / 8.0f : 1.0f;
  for (int y = -1; y < 17; y++) {
    int py = sy + y;
    if (py < 0 || py >= height)
      continue;
    uint32 *row = (uint32 *)(pixels + py * pitch);
    for (int x = -1; x < 17; x++) {
      int px = sx + x;
      if (px < 0 || px >= width || IsHud(row[px]) || IsSprite(row[px]))
        continue;
      float dx = x - 7.5f, dy = y - 7.5f;
      float d = sqrtf(dx * dx + dy * dy);
      float e = d < 8.0f ? 1.0f : d > 10.5f ? 0.0f : (10.5f - d) * 0.4f;
      if (e <= 0)
        continue;
      uint32 c = row[px];
      int l = (int)(((c >> 16) & 0xff) * 3 + ((c >> 8) & 0xff) * 6 + (c & 0xff)) / 10;
      float t = 0.30f + 0.70f * l / 255.0f;
      int idx = (int)(t * (nramp - 1) + 0.5f);
      uint32 ic = ramp[idx < 0 ? 0 : idx >= nramp ? nramp - 1 : idx];
      MixPixel(&row[px], (ic >> 16) & 0xff, (ic >> 8) & 0xff, ic & 0xff, 0.85f * e * grow);
    }
  }
  // luccichio: un punto bianco a forma di croce che si accende e si spegne
  int ph = (int)((g_fx_frames + f->seed * 7) % 96);
  if (ph < 12) {
    float a = (ph < 6 ? ph : 12 - ph) / 6.0f;
    int gx = sx + 3 + f->seed % 10, gy = sy + 3 + (f->seed >> 4) % 10;
    static const int8 kCross[5][2] = { {0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1} };
    for (int i = 0; i < 5; i++) {
      int px = gx + kCross[i][0], py = gy + kCross[i][1];
      if (px < 0 || py < 0 || px >= width || py >= height)
        continue;
      uint32 *p = (uint32 *)(pixels + py * pitch) + px;
      if (!IsHud(*p) && !IsSprite(*p))
        MixPixel(p, 255, 255, 255, a * (i == 0 ? 0.95f : 0.6f));
    }
  }
}

// Rumore a valori morbido (0-1): cella gx, gy e posizione fx, fy (0-1) dentro
// la cella. Lo usano il ghiaccio (IceNoise) e la nebbia (FogNoise)
static float ValueNoise(int gx, int gy, float fx, float fy, uint32 salt) {
  fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
  float a = Hash01((uint32)gx * 73856093u ^ (uint32)gy * 19349663u ^ salt);
  float b = Hash01((uint32)(gx + 1) * 73856093u ^ (uint32)gy * 19349663u ^ salt);
  float c = Hash01((uint32)gx * 73856093u ^ (uint32)(gy + 1) * 19349663u ^ salt);
  float d = Hash01((uint32)(gx + 1) * 73856093u ^ (uint32)(gy + 1) * 19349663u ^ salt);
  return a + (b - a) * fx + (c - a + (a - b - c + d) * fx) * fy;
}

// sui pixel del mondo, a celle di "scale" pixel
static float IceNoise(int wx, int wy, int scale, uint32 salt) {
  int gx = wx / scale, gy = wy / scale;
  return ValueNoise(gx, gy, (float)(wx - gx * scale) / scale, (float)(wy - gy * scale) / scale, salt);
}

// Una tile d'acqua ghiacciata: lastra azzurra quasi bianca (una per tile),
// con velature leggere, qualche crepa sottile, il bordo irregolare con un filo
// di brina; negli ultimi kHdThawTime frame si apre a chiazze e torna acqua.
// Il colore segue la luce del pixel d'acqua sotto (di notte e' piu' scuro)
static void DrawIceWaterTile(uint8 *pixels, int pitch, int width, int height, int sx, int sy,
                             const HdBurnTile *f) {
  static const float kDeep[3] = { 120, 168, 214 }, kLight[3] = { 222, 240, 252 };
  uint8 cells = HdBurn_WaterCells(f->x, f->y);
  float grow = f->age < 8 ? (f->age + 1) / 8.0f : 1.0f;
  int left = HdBurn_IceLeft(f);
  float rem = left <= 0 ? 0.0f : (float)left / HdBurn_ThawTime(f);
  for (int c = 0; c < 4; c++) {
    if (!(cells >> c & 1))
      continue;
    int cx = (c & 1) * 8, cy = (c >> 1) * 8;
    int wx0 = f->x + cx, wy0 = f->y + cy;
    // lati della cella che non toccano un'altra cella d'acqua della stessa
    // tile: ogni tile e' una lastra a se', col suo bordo
    bool open_l = cx == 0 || !(cells >> (c - 1) & 1), open_r = cx == 8 || !(cells >> (c + 1) & 1);
    bool open_u = cy == 0 || !(cells >> (c - 2) & 1), open_d = cy == 8 || !(cells >> (c + 2) & 1);
    for (int y = 0; y < 8; y++) {
      int py = sy + cy + y;
      if (py < 0 || py >= height)
        continue;
      uint32 *row = (uint32 *)(pixels + py * pitch);
      for (int x = 0; x < 8; x++) {
        int px = sx + cx + x;
        if (px < 0 || px >= width || IsHud(row[px]) || IsSprite(row[px]))
          continue;
        int wx = wx0 + x, wy = wy0 + y;
        // distanza dal bordo verso l'acqua, mossa dal rumore: bordo irregolare
        float e = 9.0f;
        if (open_l && x < e) e = (float)x;
        if (open_r && 7 - x < e) e = (float)(7 - x);
        if (open_u && y < e) e = (float)y;
        if (open_d && 7 - y < e) e = (float)(7 - y);
        if (e < 9.0f)
          e += (IceNoise(wx, wy, 5, 0x5ce) - 0.5f) * 3.0f;
        if (e < -0.5f)
          continue;
        float a = (e < 0.5f ? 0.45f : 0.88f) * grow;
        if (rem < 1.0f) {
          // disgelo: chiazze sempre piu' grandi tornano acqua
          float hole = IceNoise(wx, wy, 6, 0x3ce + f->seed);
          float k = (rem * 1.15f - hole) / 0.12f;
          a *= (k < 0 ? 0 : k > 1 ? 1 : k) * (0.6f + 0.4f * rem);
        }
        if (a <= 0)
          continue;
        uint32 col = row[px];
        int l = (int)(((col >> 16) & 0xff) * 3 + ((col >> 8) & 0xff) * 6 + (col & 0xff)) / 10;
        float lit = (l + 50) / 150.0f;
        lit = lit < 0.3f ? 0.3f : lit > 1.05f ? 1.05f : lit;
        // velature: ghiaccio piu' trasparente (piu' blu) e piu' opaco (piu' bianco)
        float n = IceNoise(wx, wy, 13, 0x1ce) * 0.75f + IceNoise(wx, wy, 5, 0x2ce) * 0.25f;
        float t = 0.35f + 0.45f * n;
        float cr = IceNoise(wx + 37, wy + 11, 10, 0x4ce + f->seed);
        if (cr > 0.493f && cr < 0.507f)
          t = 0.95f;  // crepa
        if (e < 1.5f)
          t = 0.85f + 0.1f * t;  // brina sul bordo
        float rgb[3];
        for (int j = 0; j < 3; j++)
          rgb[j] = (kDeep[j] + (kLight[j] - kDeep[j]) * t) * lit;
        MixPixel(&row[px], (int)rgb[0], (int)rgb[1], (int)rgb[2], a);
        if ((row[px] >> 24) == kLayerWater)
          row[px] = (row[px] & 0x00ffffff) | 1u << 24;  // non piu' acqua per lo shader
      }
    }
  }
  // luccichio, come sui cespugli congelati
  int ph = (int)((g_fx_frames + f->seed * 7) % 128);
  if (ph < 12 && rem >= 1.0f && (cells & 9) == 9) {
    float a = (ph < 6 ? ph : 12 - ph) / 6.0f;
    int gx = sx + 3 + f->seed % 10, gy = sy + 3 + (f->seed >> 4) % 10;
    static const int8 kCross[5][2] = { {0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1} };
    for (int i = 0; i < 5; i++) {
      int px = gx + kCross[i][0], py = gy + kCross[i][1];
      if (px < 0 || py < 0 || px >= width || py >= height)
        continue;
      uint32 *p = (uint32 *)(pixels + py * pitch) + px;
      if (!IsHud(*p) && !IsSprite(*p))
        MixPixel(p, 255, 255, 255, a * (i == 0 ? 0.9f : 0.5f));
    }
  }
}

void HdFx_FrozenWater(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  if (HdFx_SceneModule() != 9)
    return;
  const HdBurnTile *tiles;
  int n = HdBurn_GetFrozen(&tiles);
  if (n == 0)
    return;
  int ext = (width - 256) / 2;
  int cam_x = BG2HOFS_copy2 + bias_x, cam_y = BG2VOFS_copy2 + bias_y;
  for (int i = 0; i < n; i++) {
    const HdBurnTile *f = &tiles[i];
    if (!f->water || !HdBurn_TileShown(f))
      continue;
    int sx = f->x - cam_x + ext, sy = f->y - cam_y;
    if (sx > -20 && sy > -20 && sx < width + 4 && sy < height + 4)
      DrawIceWaterTile(pixels, pitch, width, height, sx, sy, f);
  }
}

static void ApplyBurning(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y, int ext, bool step) {
  const HdBurnTile *tiles;
  int n = HdBurn_GetFrozen(&tiles);
  if (n) {
    uint32 ramp[15];
    int nramp = IceRamp(ramp);
    for (int i = 0; i < n; i++) {
      const HdBurnTile *f = &tiles[i];
      if (f->water || !HdBurn_TileShown(f))
        continue;  // l'acqua ghiacciata la disegna HdFx_FrozenWater
      int sx = f->x - cam_x + ext, sy = f->y - cam_y;
      if (sx > -20 && sy > -20 && sx < width + 4 && sy < height + 4)
        DrawFrozenTile(pixels, pitch, width, height, sx, sy, f, ramp, nramp);
    }
  }
  n = HdBurn_GetScorched(&tiles);
  bool live = HdFx_LiveOverworld();
  for (int i = 0; i < n; i++) {
    const HdBurnTile *s = &tiles[i];
    if (!HdBurn_TileShown(s))
      continue;
    float k = (float)s->age / kHdScorchTime;
    int sx = s->x - cam_x + ext, sy = s->y - cam_y;
    if (sx > -20 && sy > -20 && sx < width + 4 && sy < height + 4)
      DrawScorchTile(pixels, pitch, width, height, sx, sy, s->seed, 0.62f - 0.10f * k);
    // fumo appena il fuoco si spegne
    if (step && live && s->age == 1) {
      for (int j = 0; j < 4; j++) {
        Particle *p = NewParticle();
        if (!p)
          break;
        p->kind = 3;
        p->x = s->x + 3 + Rand01() * 10, p->y = s->y + 2 + Rand01() * 10;
        p->vx = (Rand01() - 0.5f) * 0.25f, p->vy = -0.25f - Rand01() * 0.3f;
        p->life = 50 + (int)(Rand01() * 40);
      }
    }
  }
  n = HdBurn_GetBurning(&tiles);
  if (n == 0)
    return;
  // dall'alto in basso: le fiamme delle tile piu' in basso coprono quelle dietro
  static int order[kHdMaxBurning];
  int m = 0;
  for (int i = 0; i < n && m < kHdMaxBurning; i++) {
    int j = m++;
    while (j > 0 && tiles[order[j - 1]].y > tiles[i].y)
      order[j] = order[j - 1], j--;
    order[j] = i;
  }
  int link_feet = link_y_coord + 20;
  for (int o = 0; o < m; o++) {
    const HdBurnTile *b = &tiles[order[o]];
    int sx = b->x - cam_x + ext, sy = b->y - cam_y;
    if (sx < -24 || sy < -16 || sx >= width + 8 || sy >= height + 16)
      continue;
    // la pianta si annerisce mentre brucia
    DrawScorchTile(pixels, pitch, width, height, sx, sy, b->seed, 0.55f * b->age / kHdBurnTime);
    DrawFlamesTile(pixels, pitch, width, height, sx, sy, b, b->y + 15 < link_feet);
    if (step && live && Rand01() < 0.05f) {
      Particle *p = NewParticle();
      if (p) {
        p->kind = 3;
        p->x = b->x + 4 + Rand01() * 8, p->y = b->y - 2;
        p->vx = (Rand01() - 0.5f) * 0.2f, p->vy = -0.35f - Rand01() * 0.25f;
        p->life = 40 + (int)(Rand01() * 30);
      }
    }
  }
}

void HdFx_AfterLight(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  int mod;
  if (!HdFx_InGameplay(&mod)) {
    g_num_particles = 0;
    return;
  }
  int ext = (width - 256) / 2;
  int cam_x = BG2HOFS_copy2 + bias_x, cam_y = BG2VOFS_copy2 + bias_y;
  bool step = NewGameFrame();
  if (step) {
    g_fx_frames++;
    UpdateParticles();
    if (g_fx_heat)
      SpawnEmbers(cam_x, cam_y, ext);
    if (g_fx_whirlpool && mod == 9)
      SpawnWhirlpoolSparkles();
  }
  if (g_fx_window_rays && mod == 7)
    ApplyWindowRays(pixels, pitch, width, height, cam_x, cam_y, ext, step);
  if (g_fx_item_glow)
    ApplyItemGlow(pixels, pitch, width, height, cam_x, cam_y, ext);
  if (mod == 9)
    ApplyBurning(pixels, pitch, width, height, cam_x, cam_y, ext, step);
  DrawParticles(pixels, pitch, width, height, cam_x, cam_y, ext);
  if (g_fx_rain && IsRaining(mod)) {
    ApplyRain(pixels, pitch, width, height, cam_x, cam_y, ext, step);
    StepLightning(step);
  } else {
    g_flash = 0, g_flash_rise = false;
  }
}

// ---------------------------------------------------------------------------
// Nebbia del Bosco Perduto (e del bosco della Spada Suprema) prima della
// Spada Suprema. Il gioco la disegna con uno strato BG1 che copre tutto lo
// schermo, fermo, che scivola in diagonale: ogni pixel diventa (terreno +
// nebbia) / 2, con la nebbia quasi ovunque blu scuro (il bosco cupo) e a
// chiazze squadrate chiare. Qui la stessa somma, con gli stessi due colori
// letti dalla memoria video, ma le chiazze sono due veli legati al mondo:
// banchi grandi che scorrono lenti a terra e sbuffi piu' vicini alla
// telecamera (si spostano di piu' quando la vista si muove), morbidi, piu'
// fitti in certi punti e piu' radi in altri, e piu' radi attorno a Link.
// Segue la luminosita' del gioco (dissolvenze) e passa dal mosaico dei
// passaggi fra le aree come il resto.

bool g_fx_fog;
static bool g_fog_on, g_fog_scene;
static uint8 g_fog_saved_sub, g_fog_saved_math, g_fog_last_frame;
static uint32 g_fog_t;
// colori della nebbia del gioco (5 bit, fino a 62: dove lo strato e'
// trasparente la PPU somma il colore fisso senza dimezzare = 2 volte il fisso)
// tre tonalita': scuro, medio (il piu' usato) e chiaro
static int g_fog_shade[3], g_fog_mid[3], g_fog_light[3];

// densita' calcolata ogni kFogCell pixel e poi interpolata
enum { kFogCell = 4, kFogMaxW = 160, kFogMaxH = 72 };
static float g_fog_alpha[kFogMaxH][kFogMaxW];
// quanto si schiariscono in piu' le nuvole chiare nel Bosco d'Ossa
static const float kFogDarkWorldLight = 1.8f;

// La nebbia del bosco (del gioco) e' sullo schermo
static bool ForestFogScene(Ppu *ppu) {
  if (player_is_indoors || ppu->mode == 7)
    return false;
  // 0x9d: Bosco Perduto prima della Spada Suprema (dopo e' 0x9e, le chiome)
  // e Bosco d'Ossa (0x40, stessa nebbia con i colori del Mondo delle Tenebre,
  // presi dallo strato come nel Bosco Perduto). 0x97: bosco della Spada Suprema
  uint8 oi = BYTE(overlay_index);
  if (oi != 0x9d && oi != 0x97)
    return false;
  // come OverlayBegin (hd_world.c): lo strato e' davvero sommato a meta'
  return (ppu->screenEnabled[1] & 1) && !(ppu->screenEnabled[0] & 1) && ppu->addSubscreen &&
         !ppu->subtractColor && (ppu->mathEnabled & 2) && ppu->halfColor;
}

// Il colore piu' frequente dello strato (il medio) e, fra quelli usati in
// almeno il 2% dei punti, il piu' scuro e il piu' chiaro (le chiazze)
static void FogSampleColors(Ppu *ppu) {
  enum { kMax = 32 };
  int key[kMax], cnt[kMax], n = 0;
  int w = 256 + 2 * ppu->extraLeftRight;
  for (int sy = 4; sy < 224; sy += 10) {
    for (int sx = 3; sx < w; sx += 10) {
      int c = HdWorld_OverlayColorAt(sx, sy), k;
      if (c < 0)
        k = ppu->fixedColorR * 2 | ppu->fixedColorG * 2 << 6 | ppu->fixedColorB * 2 << 12;
      else
        k = (c & 31) | (c >> 5 & 31) << 6 | (c >> 10 & 31) << 12;
      int i = 0;
      while (i < n && key[i] != k) i++;
      if (i == n) {
        if (n == kMax)
          continue;
        key[n] = k, cnt[n++] = 0;
      }
      cnt[i]++;
    }
  }
  if (n == 0)
    return;
  #define FOG_LUM(k) (((k) & 63) * 3 + ((k) >> 6 & 63) * 6 + ((k) >> 12 & 63))
  int mid = 0, total = 0;
  for (int i = 0; i < n; i++) {
    total += cnt[i];
    if (cnt[i] > cnt[mid])
      mid = i;
  }
  int shade = mid, light = mid;
  for (int i = 0; i < n; i++) {
    if (cnt[i] * 50 < total)
      continue;
    if (FOG_LUM(key[i]) < FOG_LUM(key[shade])) shade = i;
    if (FOG_LUM(key[i]) > FOG_LUM(key[light])) light = i;
  }
  #undef FOG_LUM
  for (int c = 0; c < 3; c++) {
    g_fog_shade[c] = key[shade] >> (6 * c) & 63;
    g_fog_mid[c] = key[mid] >> (6 * c) & 63;
    g_fog_light[c] = key[light] >> (6 * c) & 63;
  }
}

void HdFx_FogBeforeRender(Ppu *ppu) {
  g_fog_scene = ForestFogScene(ppu);
  g_fog_on = g_fx_fog && g_fog_scene;
  if (!g_fog_on)
    return;
  FogSampleColors(ppu);
  // la PPU non disegna la nebbia del gioco
  g_fog_saved_sub = ppu->screenEnabled[1], g_fog_saved_math = ppu->mathEnabled;
  ppu->screenEnabled[1] &= ~1;
  ppu->mathEnabled = 0;
}

void HdFx_FogAfterRender(Ppu *ppu) {
  if (g_fog_on)
    ppu->screenEnabled[1] = g_fog_saved_sub, ppu->mathEnabled = g_fog_saved_math;
}

bool HdFx_FogReplacesOverlay(void) {
  return g_fog_on;
}

bool HdFx_ForestFogScene(void) {
  return g_fog_scene;
}

static inline float Smooth01(float a, float b, float v) {
  float t = (v - a) / (b - a);
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  return t * t * (3 - 2 * t);
}

// Rumore a valori morbido (0-1, ValueNoise), coordinate in celle
static float FogNoise(float x, float y, uint32 salt) {
  float flx = floorf(x), fly = floorf(y);
  return ValueNoise((int)flx, (int)fly, x - flx, y - fly, salt);
}

void HdFx_Fog(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  Ppu *ppu = g_zenv.ppu;
  int bri = ppu->forcedBlank ? 0 : ppu->brightness;
  if (!g_fog_on || bri == 0)
    return;
  // la nebbia si muove solo mentre si gioca (non nei menu)
  if (g_fog_last_frame != frame_counter) {
    g_fog_last_frame = frame_counter;
    if (main_module_index == 9)
      g_fog_t++;
  }
  int ext = (width - 256) / 2;
  float cam_x = (float)(BG2HOFS_copy2 + bias_x - ext), cam_y = (float)(BG2VOFS_copy2 + bias_y);
  float t = (float)g_fog_t;
  // centro di Link sullo schermo
  float lx = link_x_coord + 8 - cam_x, ly = link_y_coord + 12 - (int16)link_z_coord - cam_y;
  int gw = width / kFogCell + 2, gh = height / kFogCell + 2;
  if (gw > kFogMaxW) gw = kFogMaxW;
  if (gh > kFogMaxH) gh = kFogMaxH;
  for (int gy = 0; gy < gh; gy++) {
    for (int gx = 0; gx < gw; gx++) {
      float sx = (float)(gx * kFogCell), sy = (float)(gy * kFogCell);
      // banchi grandi a terra, legati al mondo, scorrono lenti verso sud-est
      float wx = cam_x + sx - t * 0.12f, wy = cam_y + sy - t * 0.05f;
      // tre ottave: nuvole grandi con il bordo a sbuffi
      float n1 = FogNoise(wx / 112.0f, wy / 112.0f, 0x51) * 0.55f + FogNoise(wx / 48.0f, wy / 48.0f, 0x52) * 0.30f +
                 FogNoise(wx / 20.0f, wy / 20.0f, 0x55) * 0.15f;
      // sbuffi vicini alla telecamera: si spostano piu' del terreno
      float px = cam_x * 1.35f + sx + t * 0.22f, py = cam_y * 1.35f + sy - t * 0.09f;
      float n2 = FogNoise(px / 40.0f, py / 40.0f, 0x53) * 0.7f + FogNoise(px / 18.0f, py / 18.0f, 0x54) * 0.3f;
      // nuvole piene (il chiaro dell'originale, al centro anche un po' di piu')
      float d = 1.15f * Smooth01(0.54f, 0.68f, n1) + 0.50f * Smooth01(0.62f, 0.80f, n2);
      // piu' rada attorno a Link
      float dx = sx - lx, dy = (sy - ly) * 1.25f;
      d *= 0.45f + 0.55f * Smooth01(18.0f, 84.0f, sqrtf(dx * dx + dy * dy));
      if (d > 1.15f) d = 1.15f;
      // chiazze scure a terra, fra una nuvola e l'altra (valori negativi)
      float n3 = FogNoise(wx / 72.0f, wy / 72.0f, 0x56) * 0.7f + FogNoise(wx / 28.0f, wy / 28.0f, 0x57) * 0.3f;
      float e = Smooth01(0.58f, 0.70f, n3) * (1.0f - (d < 1.0f ? d : 1.0f));
      g_fog_alpha[gy][gx] = d - e;
    }
  }
  // colori della nebbia a 8 bit, con la luminosita' del gioco: densita' 0 =
  // medio, verso 1 il chiaro, verso -1 lo scuro
  // nel Bosco d'Ossa (Mondo delle Tenebre) il chiaro dello strato e' poco
  // distante dal medio: le nuvole chiare si fanno un po' piu' evidenti
  float mid[3], up[3], down[3];
  float up_k = (overworld_screen_index & 0x40) ? kFogDarkWorldLight : 1.0f;
  for (int c = 0; c < 3; c++) {
    mid[c] = g_fog_mid[c] * 8.25f * bri / 15;
    up[c] = (g_fog_light[c] * 8.25f * bri / 15 - mid[c]) * up_k;
    down[c] = mid[c] - g_fog_shade[c] * 8.25f * bri / 15;
  }
  for (int y = 0; y < height; y++) {
    int gy = y / kFogCell;
    if (gy + 1 >= gh)
      break;
    float fy = (float)(y - gy * kFogCell) * (1.0f / kFogCell);
    const float *a0 = g_fog_alpha[gy], *a1 = g_fog_alpha[gy + 1];
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++) {
      int gx = x / kFogCell;
      if (gx + 1 >= gw)
        break;
      // solo dove la PPU somma lo strato: non l'HUD ne' gli sprite con le
      // palette 0-3 (layer 6), che nell'originale restano fuori dalla nebbia
      uint32 l = row[x] >> 24;
      if (l == 2 || l == 6 || (l < 6 && !(g_fog_saved_math >> l & 1)))
        continue;
      float fx = (float)(x - gx * kFogCell) * (1.0f / kFogCell);
      float top = a0[gx] + (a0[gx + 1] - a0[gx]) * fx, bot = a1[gx] + (a1[gx + 1] - a1[gx]) * fx;
      float a = top + (bot - top) * fy;
      // come la PPU: (terreno + nebbia) / 2
      uint32 v = row[x];
      const float *dir = a < 0 ? down : up;  // con a < 0: medio - (medio - scuro) * |a|
      int r = ((int)(v >> 16 & 255) + (int)(mid[0] + dir[0] * a)) >> 1;
      int g = ((int)(v >> 8 & 255) + (int)(mid[1] + dir[1] * a)) >> 1;
      int b = ((int)(v & 255) + (int)(mid[2] + dir[2] * a)) >> 1;
      row[x] = (v & 0xff000000) | HdFx_ClampByte(r) << 16 | HdFx_ClampByte(g) << 8 | HdFx_ClampByte(b);
    }
  }
}

// ---------------------------------------------------------------------------
// Sfocatura del gioco dietro inventario, dialoghi, menu di salvataggio e menu
// opzioni: il gioco sotto si sfoca e si scurisce con una breve dissolvenza,
// mentre l'interfaccia (layer 2) resta nitida. I pixel dell'interfaccia non
// entrano nella media, cosi' i riquadri non "sbavano" sul gioco.

bool HdMenu_IsOpen();

static int32 *g_blur_buf, *g_blur_tmp;
static int g_blur_cap;

// Media mobile di raggio r su una riga o colonna di n elementi (4 canali)
static void BoxBlur1D(const int32 *src, int32 *dst, int n, int stride, int r) {
  int32 sum[4] = { 0 };
  for (int i = -r; i <= r; i++) {
    int k = i < 0 ? 0 : i >= n ? n - 1 : i;
    for (int c = 0; c < 4; c++)
      sum[c] += src[k * stride * 4 + c];
  }
  for (int i = 0; i < n; i++) {
    for (int c = 0; c < 4; c++)
      dst[i * stride * 4 + c] = sum[c];
    int out = i - r < 0 ? 0 : i - r, in = i + r + 1 >= n ? n - 1 : i + r + 1;
    for (int c = 0; c < 4; c++)
      sum[c] += src[in * stride * 4 + c] - src[out * stride * 4 + c];
  }
}

// Dialoghi: dentro il riquadro (trasparente) il gioco si sfoca, testo e
// cornice (interfaccia, strato 2) restano nitidi. Solo con la cornice: la
// telepatia di Zelda non ha riquadro e resta com'e'.
static int32 *g_dlg_acc, *g_dlg_tmp;
static int g_dlg_cap;

static void DialogGlass(uint8 *pixels, int pitch, int width, int height) {
  if (!g_fx_menu_blur || main_module_index != 14 || submodule_index != 2)
    return;
  // riquadro del dialogo (come HdText_GetRect): 24 x 8 caselle di BG3
  int off = (text_msgbox_topleft - 0x6000) & 0x3ff;
  Ppu *ppu = g_zenv.ppu;
  int ext = (width - 256) / 2;
  int x0 = ext + (off & 31) * 8 - (ppu->bgLayer[2].hScroll & 0xff), y0 = (off >> 5) * 8 - (ppu->bgLayer[2].vScroll & 0xff);
  int x1 = IntMin(x0 + 24 * 8, width), y1 = IntMin(y0 + 8 * 8, height);
  x0 = IntMax(x0, 0), y0 = IntMax(y0, 0);
  int w = x1 - x0, h = y1 - y0;
  if (w < 16 || h < 16)
    return;
  // la cornice: quasi tutta la riga 2 del riquadro e' interfaccia
  int frame = 0;
  const uint32 *fr = (const uint32 *)(pixels + (y0 + 2) * pitch);
  for (int x = x0; x < x1; x++)
    frame += IsHud(fr[x]);
  if (frame * 10 < w * 7)
    return;
  int n = w * h;
  if (n > g_dlg_cap) {
    free(g_dlg_acc), free(g_dlg_tmp);
    g_dlg_acc = malloc(n * 16);
    g_dlg_tmp = malloc(n * 16);
    g_dlg_cap = g_dlg_acc && g_dlg_tmp ? n : 0;
    if (!g_dlg_cap)
      return;
  }
  // come dietro i menu: l'interfaccia non entra nella media
  for (int y = 0; y < h; y++) {
    const uint32 *row = (const uint32 *)(pixels + (y0 + y) * pitch) + x0;
    int32 *b = g_dlg_acc + y * w * 4;
    for (int x = 0; x < w; x++, b += 4) {
      uint32 c = row[x];
      bool ui = IsHud(c);
      b[0] = ui ? 0 : (c >> 16) & 0xff, b[1] = ui ? 0 : (c >> 8) & 0xff, b[2] = ui ? 0 : c & 0xff, b[3] = ui ? 0 : 1;
    }
  }
  for (int pass = 0; pass < 2; pass++) {
    for (int y = 0; y < h; y++)
      BoxBlur1D(g_dlg_acc + y * w * 4, g_dlg_tmp + y * w * 4, w, 1, 3);
    for (int x = 0; x < w; x++)
      BoxBlur1D(g_dlg_tmp + x * 4, g_dlg_acc + x * 4, h, w, 3);
  }
  for (int y = 0; y < h; y++) {
    uint32 *row = (uint32 *)(pixels + (y0 + y) * pitch) + x0;
    const int32 *b = g_dlg_acc + y * w * 4;
    for (int x = 0; x < w; x++, b += 4) {
      uint32 c = row[x];
      if (IsHud(c) || b[3] == 0)
        continue;
      // strato 2 (interfaccia): lo shader lascia il riquadro sfocato com'e',
      // senza pennellate e ombre nitide sopra
      row[x] = 2u << 24 | HdFx_ClampByte(b[0] / b[3]) << 16 | HdFx_ClampByte(b[1] / b[3]) << 8 | HdFx_ClampByte(b[2] / b[3]);
    }
  }
}

static float g_blur_amount;  // 0 = niente sfocatura, 1 = piena

bool HdFx_MenuBlurActive(void) {
  // sulla mappa del mondo (Mode 7) la sfocatura non viene disegnata
  return g_blur_amount > 0 && g_zenv.ppu->mode != 7;
}

void HdFx_MenuBlur(uint8 *pixels, int pitch, int width, int height) {
  DialogGlass(pixels, pitch, width, height);
  float amount = g_blur_amount;
  static uint8 last_frame;
  int mod = main_module_index, sub = submodule_index;
  // inventario (1), menu di salvataggio (11) e menu opzioni; non i dialoghi
  bool want = g_fx_menu_blur &&
      ((mod == 14 && (sub == 1 || sub == 11)) || HdMenu_IsOpen());
  if (!want) {
    g_blur_amount = 0;
    return;
  }
  // con il menu opzioni aperto il gioco e' fermo: la dissolvenza va avanti
  // comunque, un passo per ogni disegno
  if (last_frame != frame_counter || HdMenu_IsOpen()) {
    last_frame = frame_counter;
    amount = amount + 1.0f / 8 > 1 ? 1 : amount + 1.0f / 8;
    g_blur_amount = amount;
  }
  int n = width * height;
  if (n > g_blur_cap) {
    free(g_blur_buf), free(g_blur_tmp);
    g_blur_buf = malloc(n * 16);
    g_blur_tmp = malloc(n * 16);
    g_blur_cap = g_blur_buf && g_blur_tmp ? n : 0;
    if (!g_blur_cap)
      return;
  }
  for (int y = 0; y < height; y++) {
    const uint32 *row = (const uint32 *)(pixels + y * pitch);
    int32 *b = g_blur_buf + y * width * 4;
    for (int x = 0; x < width; x++, b += 4) {
      uint32 c = row[x];
      bool ui = IsHud(c);
      b[0] = ui ? 0 : (c >> 16) & 0xff;
      b[1] = ui ? 0 : (c >> 8) & 0xff;
      b[2] = ui ? 0 : c & 0xff;
      b[3] = ui ? 0 : 1;
    }
  }
  // due passate orizzontali e verticali di raggio 3: quasi una gaussiana
  for (int pass = 0; pass < 2; pass++) {
    for (int y = 0; y < height; y++)
      BoxBlur1D(g_blur_buf + y * width * 4, g_blur_tmp + y * width * 4, width, 1, 3);
    for (int x = 0; x < width; x++)
      BoxBlur1D(g_blur_tmp + x * 4, g_blur_buf + x * 4, height, width, 3);
  }
  float keep = 1.0f - amount, dark = 1.0f - 0.30f * amount;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    const int32 *b = g_blur_buf + y * width * 4;
    for (int x = 0; x < width; x++, b += 4) {
      uint32 c = row[x];
      if (IsHud(c) || b[3] == 0)
        continue;
      float inv = amount / b[3];
      int r = (int)((((c >> 16) & 0xff) * keep + b[0] * inv) * dark);
      int g = (int)((((c >> 8) & 0xff) * keep + b[1] * inv) * dark);
      int bl = (int)(((c & 0xff) * keep + b[2] * inv) * dark);
      row[x] = (c & 0xff000000) | HdFx_ClampByte(r) << 16 | HdFx_ClampByte(g) << 8 | HdFx_ClampByte(bl);
    }
  }
}

// ---------------------------------------------------------------------------
// Cerchio di transizione morbido. Il gioco disegna un'ellisse con le finestre
// dello SNES, che arrivano solo ai 256 pixel centrali: in 16:9 le fasce
// laterali restavano nere e si accendevano di colpo alla fine. Qui, mentre il
// cerchio e' in corso, la PPU disegna tutto (HdFx_IrisBeforeRender) e il
// cerchio viene tracciato a parte su tutta la larghezza, con il bordo sfumato.
// Il raggio e' la frazione di quello del gioco (0-126) rispetto alla distanza
// dell'angolo piu' lontano: aperto del tutto copre l'intero schermo.

extern int g_hd_iris_x, g_hd_iris_y, g_hd_iris_r;
extern uint8 g_hd_iris_frame;
extern bool g_hd_iris_valid;

static uint16 g_iris_saved_table[240];
static bool g_iris_override;

// Preghiera nel deserto con lo schermo largo: la finestra del gioco (ellisse
// attorno a Link) sta nei 256 pixel originali, quindi le fasce ai lati
// restavano nere e l'ellisse si vedeva tagliata dritta. Si apre la finestra
// su tutta la riga e l'ellisse si applica dopo su tutta la larghezza, con i
// bordi veri calcolati dal gioco (messaging.c); fuori resta nero
extern int16 g_prayer_iris_l[240], g_prayer_iris_r[240];
static bool g_prayer_override;

static bool PrayerIrisActive(Ppu *ppu) {
  return main_module_index == 14 && submodule_index == 5 && subsubmodule_index >= 3 &&
         (HDMAEN_copy & 0x80) && ppu->extraLeftRight > 0;
}

static bool IrisActive(void) {
  if (!g_fx_soft_iris || !g_hd_iris_valid)
    return false;
  if ((uint8)(frame_counter - g_hd_iris_frame) > 1) {
    g_hd_iris_valid = false;
    return false;
  }
  return true;
}

void HdFx_IrisBeforeRender(Ppu *ppu) {
  g_iris_override = IrisActive();
  g_prayer_override = !g_iris_override && PrayerIrisActive(ppu);
  if (!g_iris_override && !g_prayer_override)
    return;
  memcpy(g_iris_saved_table, hdma_table_dynamic, sizeof(g_iris_saved_table));
  for (int i = 0; i < 240; i++)
    hdma_table_dynamic[i] = 0xff00;  // finestra su tutta la riga
  ppu->windowFullSides = true;
}

void HdFx_IrisAfterRender(Ppu *ppu) {
  if (!g_iris_override && !g_prayer_override)
    return;
  memcpy(hdma_table_dynamic, g_iris_saved_table, sizeof(g_iris_saved_table));
  ppu->windowFullSides = false;
}

// ---------------------------------------------------------------------------
// Dissolvenze delle palette (scale, cambi di piano, porte che scuriscono,
// ritorno con la fata): il gioco avvicina a passi i colori del fondale
// (main_palette_buffer) al nero partendo da quelli veri (aux_palette_buffer,
// ApplyPaletteFilter_bounce). Acqua, luci e ombre riconoscono le cose dai
// colori: con i colori scuriti i muri diventavano acqua e la schiuma bianca
// restava sullo schermo quasi nero. Stessa regola dello specchio: la PPU
// disegna il fondale con i colori veri, le aggiunte HD lavorano su un
// fotogramma normale e alla fine HdFx_FadePost scurisce tutto insieme come
// avrebbe fatto il gioco. Alla morte di Link i colori veri sono in
// mapbak_palette (Death_Func1): il cerchio li scurisce, la fata li riaccende.
// Il passo dipende solo dal valore (0-31) di ogni
// componente, uguale per rosso, verde e blu: la tabella si ricava dalle
// coppie (vero, scurito) della palette. Sprite e HUD restano quelli della PPU.

static bool g_fade_on;
static uint8 g_fade_map[256];  // componente a 8 bit vera -> scurita
static uint16 g_fade_cgram[0x80];
static uint16 g_fade_real[0x80];  // i colori veri del fondale
static uint8 *g_fade_mask;     // 1: fondale da scurire (dal frame della PPU)
static int g_fade_cap;

static bool FadeEntry(int i) {
  return i == 0 || (i >= 0x20 && i < 0x80);
}

static const uint16 *FadeRealPalette(void) {
  return main_module_index == 18 ? mapbak_palette : aux_palette_buffer;
}

static bool PrepareFadeMap(const Ppu *ppu) {
  const uint16 *real = FadeRealPalette();
  int m5[32];
  for (int v = 0; v < 32; v++)
    m5[v] = -1;
  bool differ = false;
  for (int i = 0; i < 0x80; i++) {
    if (!FadeEntry(i))
      continue;
    uint16 c = ppu->cgram[i], a = real[i];
    for (int k = 0; k < 15; k += 5) {
      int av = a >> k & 31, cv = c >> k & 31;
      if (cv > av || (m5[av] >= 0 && m5[av] != cv))
        return false;  // non e' una dissolvenza verso il nero
      m5[av] = cv;
      differ |= cv != av;
    }
  }
  if (!differ)
    return false;
  // i valori che la palette non usa: in mezzo fra i vicini
  if (m5[0] < 0)
    m5[0] = 0;
  int prev = 0;
  for (int v = 1; v < 32; v++) {
    if (m5[v] < 0)
      continue;
    for (int j = prev + 1; j < v; j++)
      m5[j] = m5[prev] + (m5[v] - m5[prev]) * (j - prev) / (v - prev);
    prev = v;
  }
  for (int j = prev + 1; j < 32; j++)
    m5[j] = m5[prev] * j / (prev ? prev : 1);
  for (int x = 0; x < 256; x++) {
    float fv = x * 31.0f / 255.0f;
    int lo = (int)fv, hi = lo < 31 ? lo + 1 : 31;
    float a = (float)(m5[lo] << 3 | m5[lo] >> 2), b = (float)(m5[hi] << 3 | m5[hi] >> 2);
    int out = (int)(a + (b - a) * (fv - lo) + 0.5f);
    g_fade_map[x] = (uint8)(out > x ? x : out);
  }
  return true;
}

void HdFx_FadeBeforeRender(Ppu *ppu) {
  g_fade_on = false;
  int mod;
  // (lo specchio ha la sua regola, HdWorld_WarpBeforeRender)
  if (main_module_index == 9 && (submodule_index == 0x23 || submodule_index == 0x2c))
    return;
  if ((!HdFx_InGameplay(&mod) && !HdFx_DeathShowsWorld()) || ppu->forcedBlank || ppu->brightness != 15)
    return;
  if (!PrepareFadeMap(ppu))
    return;
  g_fade_on = true;
  memcpy(g_fade_cgram, ppu->cgram, sizeof(g_fade_cgram));
  memcpy(g_fade_real, FadeRealPalette(), sizeof(g_fade_real));
  for (int i = 0; i < 0x80; i++)
    if (FadeEntry(i))
      ppu->cgram[i] = g_fade_real[i];
}

void HdFx_FadeAfterRender(Ppu *ppu) {
  if (g_fade_on)
    memcpy(ppu->cgram, g_fade_cgram, sizeof(g_fade_cgram));
}

bool HdFx_FadeActive(void) {
  return g_fade_on;
}

// Colore i della palette pal (main_palette_buffer o la CGRAM) come lo vede
// la PPU nel disegno: durante la dissolvenza quello vero per il fondale
uint16 HdFx_RealBg(const uint16 *pal, int i) {
  return g_fade_on && i < 0x80 && FadeEntry(i) ? g_fade_real[i] : pal[i];
}

// Prima delle aggiunte HD: quali pixel sono fondale (alcuni effetti poi
// segnano i loro pixel come sprite, per lo shader)
void HdFx_FadeMask(uint8 *pixels, int pitch, int width, int height) {
  if (!g_fade_on)
    return;
  int n = width * height;
  if (n > g_fade_cap) {
    free(g_fade_mask);
    g_fade_mask = malloc(n);
    g_fade_cap = g_fade_mask ? n : 0;
  }
  if (!g_fade_mask) {
    g_fade_on = false;
    return;
  }
  for (int y = 0; y < height; y++) {
    const uint32 *row = (const uint32 *)(pixels + y * pitch);
    uint8 *m = g_fade_mask + y * width;
    for (int x = 0; x < width; x++) {
      uint32 l = row[x] >> 24;
      m[x] = !(l == 2 || l == 4 || l == 6);
    }
  }
}

void HdFx_FadePost(uint8 *pixels, int pitch, int width, int height) {
  if (!g_fade_on || width * height > g_fade_cap)
    return;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    const uint8 *m = g_fade_mask + y * width;
    for (int x = 0; x < width; x++) {
      if (!m[x])
        continue;
      uint32 c = row[x];
      row[x] = (c & 0xff000000) | g_fade_map[c >> 16 & 255] << 16 | g_fade_map[c >> 8 & 255] << 8 | g_fade_map[c & 255];
    }
  }
}

void HdFx_Mosaic(uint8 *pixels, int pitch, int width, int height) {
  // mosaico del gioco (mulinello, specchio nei labirinti): la PPU sgrana gli
  // sfondi, ma acqua e luci HD sono disegnate dopo, pixel per pixel. Si
  // sgrana tutto con gli stessi quadrati della PPU, cosi' l'HD resta e non
  // c'e' lo scatto all'immagine originale
  Ppu *ppu = g_zenv.ppu;
  int m = ppu->mosaicSize;
  if (m <= 1 || !(ppu->mosaicEnabled & 3))
    return;
  int ext = (width - 256) / 2;
  for (int y = 0; y < height; y++) {
    // riga della PPU: linea y + 1, che prende la linea (y + 1) - (y + 1) % m
    int sy = (y + 1) - (y + 1) % m - 1;
    if (sy < 0)
      sy = 0;
    uint32 *row = (uint32 *)(pixels + y * pitch);
    const uint32 *src = (const uint32 *)(pixels + sy * pitch);
    for (int x = 0; x < width; x++) {
      uint32 l = row[x] >> 24;
      if (l == 2 || l == 4 || l == 6)
        continue;  // HUD e sprite restano
      int sx = x - (((x - ext) % m + m) % m);
      if (sx < 0)
        sx = 0;
      if (sx == x && sy == y)
        continue;
      uint32 c = src[sx], lc = c >> 24;
      if (lc == 2 || lc == 4 || lc == 6)
        continue;  // sotto uno sprite il colore del fondale non si sa
      row[x] = c;
    }
  }
}

static void PrayerIris(uint8 *pixels, int pitch, int width, int height) {
  Ppu *ppu = g_zenv.ppu;
  int ext = (width - 256) / 2;
  // fuori dalla finestra il gioco taglia a nero (CGWSEL)
  uint32 back = 5u << 24;
  // come gli sprite, l'ellisse segue Link se la vista e' spostata
  for (int y = 0; y < height; y++) {
    int gy = y + ppu->objBiasY;
    int l = 1, r = 0;
    if (gy >= 0 && gy < 240)
      l = g_prayer_iris_l[gy], r = g_prayer_iris_r[gy];
    l += ext - ppu->objBiasX, r += ext - ppu->objBiasX;
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++)
      if (x < l || x > r)
        row[x] = back;
  }
}

void HdFx_SoftIris(uint8 *pixels, int pitch, int width, int height) {
  if (g_prayer_override)
    PrayerIris(pixels, pitch, width, height);
  if (!g_iris_override)
    return;
  const float kSoft = 8.0f;
  int ext = (width - 256) / 2;
  // con la vista spostata (mondo continuo) Link, e con lui il centro del
  // cerchio, si disegna spostato come gli sprite
  Ppu *ppu = g_zenv.ppu;
  float cx = g_hd_iris_x - ppu->objBiasX + ext + 0.5f, cy = g_hd_iris_y - ppu->objBiasY + 0.5f;
  float far2 = 0;
  for (int k = 0; k < 4; k++) {
    float dx = (k & 1 ? width : 0) - cx, dy = (k & 2 ? height : 0) - cy;
    if (dx * dx + dy * dy > far2)
      far2 = dx * dx + dy * dy;
  }
  float r = g_hd_iris_r * (1.0f / 126) * (sqrtf(far2) + kSoft);
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    float dy = y - cy;
    for (int x = 0; x < width; x++) {
      float dx = x - cx, d2 = dx * dx + dy * dy;
      if (r > kSoft && d2 < (r - kSoft) * (r - kSoft))
        continue;
      float k = (r - sqrtf(d2)) / kSoft;
      if (k >= 1)
        continue;
      if (k < 0) k = 0;
      k = k * k * (3 - 2 * k);
      uint32 c = row[x];
      row[x] = (c & 0xff000000) | (int)(((c >> 16) & 0xff) * k) << 16 |
               (int)(((c >> 8) & 0xff) * k) << 8 | (int)((c & 0xff) * k);
    }
  }
}

// ---------------------------------------------------------------------------
// Ombre di Link e dei nemici a terra: la sagoma di ogni sprite viene
// "sdraiata" a terra partendo dai piedi (il pixel piu' basso di ogni colonna),
// inclinata verso il basso a destra come le ombre dello shader, sfocata e
// usata per scurire il fondale. Sprite e HUD non vengono scuriti.
// Fanno ombra solo i pezzi di Link a terra e dei nemici a terra che nel gioco
// hanno l'ombra: per questi l'ombra del gioco si nasconde. I nemici in volo
// (o mentre saltano) tengono l'ombra del gioco; effetti, oggetti in volo e
// resti non fanno ombra. La PPU segna pixel per pixel da quale voce OAM viene
// lo sprite (objShadowOut).

static float *g_shadow, *g_shadow_tmp;
static int g_shadow_cap;
static uint8 *g_shadow_src;   // 1 = lo sprite di questo pixel fa ombra
static int g_shadow_src_cap;
static bool g_shadow_src_valid;
static uint16 g_hidden_oam[128];
static uint8 g_hidden_idx[128];
static int g_num_hidden;

extern uint8 g_link_oam_mask[128], g_sprite_oam_owner[128];
extern uint8 g_link_dive_timer;  // player.c

// Ombra del gioco (SpriteDraw_Shadow): casella 0x6c (grande) o 0x38 (piccola)
// della prima tabella, palette 4
static bool IsGameShadowTile(uint16 oam1) {
  return (oam1 >> 9 & 7) == 4 && ((oam1 & 0x1ff) == 0x6c || (oam1 & 0x1ff) == 0x38);
}

// Nemici che non camminano: volano o fluttuano (corvi, avvoltoi, pipistrelli,
// api, fate, Poe, Bari, Zirro...), stanno in acqua o sui muri, o sono oggetti
// fissi (interruttori, statue, Beamos, trappole, barre di fuoco). Non fanno
// l'ombra morbida: tengono quella del gioco, se ce l'hanno.
static bool IsNotWalkingType(uint8 t) {
  static const uint8 kTypes[] = {
    0x00, 0x01, 0x02, 0x15, 0x19, 0x1c, 0x1e, 0x23, 0x24, 0x2d, 0x38, 0x3a, 0x52,
    0x54, 0x55, 0x59, 0x5b, 0x5c, 0x61, 0x6f, 0x72, 0x79, 0x7d, 0x7e, 0x7f, 0x81,
    0x82, 0x88, 0x8a, 0x8c, 0x90, 0x95, 0x96, 0x97, 0x98, 0x9a, 0xa0, 0xa2, 0xa3,
    0xa4, 0xa8, 0xa9, 0xb2, 0xb3, 0xbe, 0xc5, 0xc6, 0xc8, 0xcf, 0xe3,
    0x14, 0x21,  // oggetti: grata della Citta' dei Ladri, leva dell'acqua
    0xba,        // vortice che porta Link nell'acqua
    0x3b,        // albero da colpire con lo scatto (sopra quello del fondale)
  };
  for (int i = 0; i < (int)sizeof(kTypes); i++)
    if (kTypes[i] == t)
      return true;
  return false;
}

// Nemici travestiti o nascosti: finche' sembrano un oggetto o non sono usciti
// non fanno ombra morbida, altrimenti l'ombra li tradirebbe (restano col
// disegno originale); la fanno solo da attivi
static bool HiddenEnemy(int k) {
  int ai = sprite_ai_state[k];
  switch (sprite_type[k]) {
  case 0x17: case 0x3e: return ai == 0;  // Hoarder sotto un cespuglio o una roccia
  case 0xd3: return ai == 0;             // Stal, il sasso a teschio, mentre dorme
  case 0x8f: return ai < 2;              // Zol: nascosto o mentre esce dal pavimento
  case 0x64: return ai != 2;             // Debirando: fuori solo quando sputa fuoco
  case 0xa1: return ai < 2;              // Freezor: statua nel muro finche' non si stacca
  case 0x73: return sprite_E[k] == 2;    // non e' un nemico: il blocco del Santuario
  }
  return false;
}

// Riga dello schermo della voce OAM i (come in ppu_evaluateSprites)
static int OamScreenY(Ppu *ppu, int i, int bottom) {
  int yy = ppu->oam[i * 2] >> 8;
  int sy = ppu->objYValid ? ppu->objY[i] : yy >= bottom ? yy - 256 : yy;
  return sy - ppu->objBiasY;
}

void HdFx_ShadowsBeforeRender(Ppu *ppu, int width, int height) {
  int mod;
  ppu->objShadowOut = NULL;
  g_num_hidden = 0;
  g_shadow_src_valid = false;
  if (!g_fx_sprite_shadows || !HdFx_InGameplay(&mod) || ppu->mode == 7)
    return;
  int n = width * height;
  if (n > g_shadow_src_cap) {
    free(g_shadow_src);
    g_shadow_src = malloc(n);
    g_shadow_src_cap = g_shadow_src ? n : 0;
    if (!g_shadow_src)
      return;
  }
  memset(g_shadow_src, 0, n);
  bool link_on_ground = (int16)link_z_coord <= 0;
  // Per ogni nemico: dov'e' la sua ombra del gioco e dove finisce il corpo.
  // Un nemico che cammina e' a terra se non e' in aria (sprite_z) e, quando
  // ha l'ombra del gioco, se questa tocca i piedi: in volo fra corpo e ombra
  // c'e' spazio, anche per chi si alza senza usare sprite_z
  int bottom = height > 224 ? 240 : 224;
  int shadow_top[16], body_bottom[16];
  for (int k = 0; k < 16; k++)
    shadow_top[k] = 1 << 20, body_bottom[k] = -(1 << 20);
  for (int i = 0; i < 128; i++) {
    int o = g_sprite_oam_owner[i];
    if (o >= 16 || g_link_oam_mask[i] || (ppu->oam[i * 2] >> 8) == 0xf0)
      continue;
    int y = OamScreenY(ppu, i, bottom);
    int size = (ppu->oam[0x100 + (i >> 3)] >> ((i & 7) * 2) & 2) ? 16 : 8;
    if (IsGameShadowTile(ppu->oam[i * 2 + 1])) {
      if (y < shadow_top[o]) shadow_top[o] = y;
    } else if (y + size > body_bottom[o]) {
      body_bottom[o] = y + size;
    }
  }
  bool ground[16];
  for (int k = 0; k < 16; k++)
    ground[k] = (sprite_state[k] == 9 || sprite_state[k] == 11) && sprite_type[k] < 0xd8 &&
                !IsNotWalkingType(sprite_type[k]) && sprite_z[k] == 0 &&
                !HiddenEnemy(k) &&
                (shadow_top[k] == (1 << 20) || shadow_top[k] <= body_bottom[k] + 2);
  for (int i = 0; i < 128; i++) {
    bool shadow = IsGameShadowTile(ppu->oam[i * 2 + 1]);
    int o = g_sprite_oam_owner[i];
    uint8 cast = 0;
    if (g_link_oam_mask[i]) {
      cast = !shadow && link_on_ground && !(g_link_dive_timer && player_is_indoors);  // sott'acqua al chiuso niente ombra, come su GBA
    } else if (o < 16 && ground[o]) {
      if (shadow) {
        g_hidden_idx[g_num_hidden] = i;
        g_hidden_oam[g_num_hidden++] = ppu->oam[i * 2];
        ppu->oam[i * 2] = (ppu->oam[i * 2] & 0xff) | 0xf000;  // y = 0xF0: non si disegna
        ppu->objShowF0[i] = 0;
      } else {
        cast = 1;
      }
    }
    ppu->objCastShadow[i] = cast;
  }
  ppu->objShadowOut = g_shadow_src;
  ppu->objShadowStride = width;
}

// Pezzi di ghiaccio: il cespuglio congelato portato o lanciato (sprite 0xEC con
// sprite_unk5) e i suoi pezzi (resti 0x48/0x58 della prima tabella con la
// palette 2, che il gioco non usa cosi'). I loro colori sono quelli della
// palette 6 (vasi, rocce e cespugli): ognuno diventa il blu dei nemici
// congelati (colori 1-7 della palette 2) con la luminosita' piu' vicina
void HdFx_IceBeforeRender(Ppu *ppu) {
  int mod;
  memset(ppu->objIce, 0, sizeof(ppu->objIce));
  if (!(enhanced_features0 & kFeatures0_BurnAndFreeze) || player_is_indoors || !HdFx_InGameplay(&mod))
    return;
  // i blu 1-7 della palette 2 in ordine di luminosita', scelti con la stessa
  // formula di DrawFrozenTile: sollevato il cespuglio resta del colore che
  // aveva a terra
  int8 order[7];
  int lum[7];
  for (int i = 0; i < 7; i++) {
    uint16 c = ppu->cgram[0x80 + 2 * 16 + 1 + i];
    int l = (c & 31) * 3 + (c >> 5 & 31) * 6 + (c >> 10 & 31), j = i;
    while (j > 0 && lum[j - 1] > l)
      lum[j] = lum[j - 1], order[j] = order[j - 1], j--;
    lum[j] = l, order[j] = 1 + i;
  }
  ppu->iceRemap[0] = 0;
  for (int v = 1; v < 16; v++) {
    uint16 c = ppu->cgram[0x80 + 6 * 16 + v];
    int l = (c & 31) * 3 + (c >> 5 & 31) * 6 + (c >> 10 & 31);
    float t = 0.30f + 0.70f * l / 310.0f;
    int idx = (int)(t * 6 + 0.5f);
    ppu->iceRemap[v] = order[idx < 0 ? 0 : idx > 6 ? 6 : idx];
  }
  for (int i = 0; i < 128; i++) {
    uint16 oam1 = ppu->oam[i * 2 + 1];
    if ((oam1 >> 9 & 7) != 2 || g_link_oam_mask[i])
      continue;
    int o = g_sprite_oam_owner[i];
    if (o < 16)
      ppu->objIce[i] = sprite_type[o] == 0xec && sprite_unk5[o] != 0;
    else
      ppu->objIce[i] = (oam1 & 0x1ff) == 0x48 || (oam1 & 0x1ff) == 0x58;
  }
}

// Oggetti spinti che da fermi sono caselle del fondale: il blocco dei
// labirinti (Sprite_HandlePushedBlocks_One, Sprite_Dungeon_DrawSinglePushBlock)
// e la lapide (Ancilla24_Gravestone) mentre si muovono sono sprite, e lo
// shader Four Swords smetteva di dipingerli fino a quando si fermavano.
// Chi li disegna segna le voci OAM con il fotogramma (frame_counter | 0x100):
// valgono solo per il fotogramma in cui sono state scritte
static uint16 g_oam_as_bg[128];

void HdFx_OamAsBackground(const void *oam, int n) {
  int i = (int)((const uint8 *)oam - &g_ram[0x800]) >> 2;
  for (; n > 0 && i >= 0 && i < 128; n--, i++)
    g_oam_as_bg[i] = frame_counter | 0x100;
}

void HdFx_AsBgBeforeRender(Ppu *ppu) {
  for (int i = 0; i < 128; i++)
    ppu->objAsBg[i] = g_oam_as_bg[i] == (frame_counter | 0x100);
}

// Immersione al chiuso come su GBA (0x80a5298): Link resta sopra il fondale
// della vasca ma la PPU lo mescola a meta' con l'acqua (BG1 sullo schermo
// secondario nelle stanze allagate); prima spariva sotto il fondale
void HdFx_DiveBeforeRender(Ppu *ppu) {
  bool on = g_link_dive_timer && player_is_indoors;
  for (int i = 0; i < 128; i++)
    ppu->objUnderWater[i] = on && g_link_oam_mask[i];
}

// Super bomba come su GBA (0x8116afc, sprite affine a doppia dimensione):
// segue Link gonfiandosi e sgonfiandosi con il contatore dei fotogrammi,
// lato = (288 -/+ seno / 4) / 256 (0,875x - 1,375x, periodo 256 fotogrammi);
// posata, nell'ultimo secondo prima dello scoppio cresce da 1x a 2x
void HdFx_SuperBombBeforeRender(Ppu *ppu) {
  memset(ppu->objScale, 0, sizeof(ppu->objScale));
  if (!(enhanced_features0 & kFeatures0_SwordAndItems) || follower_indicator != 13 ||
      g_super_bomb_oam >= 128 || (ppu->oam[g_super_bomb_oam * 2 + 1] & 0xff) != 0x22)
    return;
  int s;
  if (super_bomb_indicator_unk2 <= 1) {
    s = 512 - super_bomb_indicator_unk1 * 256 / 62;
  } else {
    int v = (int)(256 * sinf((frame_counter & 127) * 3.14159265f / 128)) >> 2;
    s = frame_counter & 128 ? 288 + v : 288 - v;
  }
  ppu->objScale[g_super_bomb_oam] = (uint16)IntMax(s, 1);
}

void HdFx_ShadowsAfterRender(Ppu *ppu) {
  for (int j = 0; j < g_num_hidden; j++)
    ppu->oam[g_hidden_idx[j] * 2] = g_hidden_oam[j];
  g_num_hidden = 0;
  g_shadow_src_valid = ppu->objShadowOut != NULL;
  ppu->objShadowOut = NULL;
}

static void BoxBlurF(const float *src, float *dst, int w, int h, int dx, int dy) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      float s = 0;
      for (int k = -1; k <= 1; k++) {
        int xx = x + k * dx, yy = y + k * dy;
        if (xx >= 0 && xx < w && yy >= 0 && yy < h)
          s += src[yy * w + xx];
      }
      dst[y * w + x] = s * (1.0f / 3);
    }
}

void HdFx_SpriteShadows(uint8 *pixels, int pitch, int width, int height) {
  int mod;
  if (!g_fx_sprite_shadows || !HdFx_InGameplay(&mod) || !g_shadow_src_valid)
    return;
  int n = width * height;
  if (n > g_shadow_cap) {
    free(g_shadow), free(g_shadow_tmp);
    g_shadow = malloc(n * sizeof(float));
    g_shadow_tmp = malloc(n * sizeof(float));
    g_shadow_cap = g_shadow && g_shadow_tmp ? n : 0;
    if (!g_shadow_cap)
      return;
  }
  memset(g_shadow, 0, n * sizeof(float));
  bool any = false;
  for (int x = 0; x < width; x++) {
    int y = 0;
    while (y < height) {
      if (!IsSprite(*(uint32 *)(pixels + y * pitch + x * 4)) || !g_shadow_src[y * width + x]) {
        y++;
        continue;
      }
      int y0 = y;
      while (y < height && IsSprite(*(uint32 *)(pixels + y * pitch + x * 4)) && g_shadow_src[y * width + x])
        y++;
      int foot = y - 1;
      // ogni pixel alto h sopra i piedi finisce spostato a destra e in basso
      for (int yy = y0; yy <= foot; yy++) {
        int h = foot - yy;
        int sx = x + (h * 5 + 5) / 10, sy = foot + (h * 3 + 10) / 20;
        if (sx < width && sy < height)
          g_shadow[sy * width + sx] = 1, any = true;
      }
    }
  }
  if (!any)
    return;
  for (int pass = 0; pass < 2; pass++) {
    BoxBlurF(g_shadow, g_shadow_tmp, width, height, 1, 0);
    BoxBlurF(g_shadow_tmp, g_shadow, width, height, 0, 1);
  }
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    const float *m = g_shadow + y * width;
    for (int x = 0; x < width; x++) {
      if (m[x] < 0.01f || IsSprite(row[x]) || IsHud(row[x]))
        continue;
      float k = 1.0f - 0.42f * (m[x] > 1 ? 1 : m[x]);
      uint32 c = row[x];
      row[x] = (c & 0xff000000) | (int)(((c >> 16) & 0xff) * k) << 16 |
               (int)(((c >> 8) & 0xff) * k) << 8 | (int)((c & 0xff) * k);
    }
  }
}

// ---------------------------------------------------------------------------
// Fondali: nelle aree della Montagna della Morte e della Piramide lo sfondo
// (layer 5) e' il cielo con le nuvole, e lo shader lo deve lasciare com'e'.
// Altrove lo sfondo e' spesso il prato fra le caselle e va dipinto: per questo
// si marca come 3 (layer non usato all'aperto) solo in queste aree.
// Anche qui pero' fra le caselle del terreno (attorno alle chiome degli
// alberi) lo sfondo si vede a strisce sottili: quelle sono prato e vanno
// dipinte, altrimenti ombre e nuvole dello shader le saltano e si vedono i
// quadretti. Cielo = sfondo entro kSkyReach pixel da un punto che ha attorno
// (quadrato di lato 2 * kSkyCore + 1) quasi solo sfondo.
enum { kSkyCore = 8, kSkyReach = 10 };

// Somme del quadrato attorno a ogni pixel, dalla tabella delle somme s
// (larghezza w + 1)
static int BoxSum(const int *s, int w, int h, int x, int y, int r) {
  int x0 = x - r < 0 ? 0 : x - r, x1 = x + r + 1 > w ? w : x + r + 1;
  int y0 = y - r < 0 ? 0 : y - r, y1 = y + r + 1 > h ? h : y + r + 1;
  return s[y1 * (w + 1) + x1] - s[y0 * (w + 1) + x1] - s[y1 * (w + 1) + x0] + s[y0 * (w + 1) + x0];
}

static void Integral(int *s, int w, int h, const uint8 *m) {
  memset(s, 0, (w + 1) * sizeof(int));
  for (int y = 0; y < h; y++) {
    int acc = 0;
    s[(y + 1) * (w + 1)] = 0;
    for (int x = 0; x < w; x++) {
      acc += m[y * w + x];
      s[(y + 1) * (w + 1) + x + 1] = s[y * (w + 1) + x + 1] + acc;
    }
  }
}

void HdFx_MarkBackdrop(uint8 *pixels, int pitch, int width, int height) {
  int mod;
  if (!HdFx_InGameplay(&mod) || mod != 9 || !IsBackdropScene())
    return;
  static uint8 *mask, *core;
  static int *sum, cap;
  if (cap < width * height) {
    free(mask), free(core), free(sum);
    mask = malloc(width * height), core = malloc(width * height);
    sum = malloc((width + 1) * (height + 1) * sizeof(int));
    cap = mask && core && sum ? width * height : 0;
    if (!cap)
      return;
  }
  for (int y = 0; y < height; y++) {
    const uint32 *row = (const uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++)
      mask[y * width + x] = (row[x] >> 24) == 5;
  }
  int side = 2 * kSkyCore + 1;
  Integral(sum, width, height, mask);
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      core[y * width + x] = mask[y * width + x] && BoxSum(sum, width, height, x, y, kSkyCore) * 10 >= side * side * 9;
  Integral(sum, width, height, core);
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++)
      if (mask[y * width + x] && BoxSum(sum, width, height, x, y, kSkyReach))
        row[x] = (row[x] & 0x00ffffff) | 3u << 24;
  }
}

