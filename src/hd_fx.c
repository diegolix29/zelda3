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

bool g_fx_embers, g_fx_window_rays, g_fx_item_glow, g_fx_heat_haze, g_fx_rain;
bool g_fx_menu_blur, g_fx_sprite_shadows, g_fx_soft_iris, g_fx_button_hints;

const uint16 *Dungeon_GetObjectTiles(int subtype, int idx);

// ---------------------------------------------------------------------------
// Utilita'

static uint32 g_rng = 0x12345678;
static float Rand01() {
  g_rng = g_rng * 1664525 + 1013904223;
  return (g_rng >> 8) * (1.0f / 16777216.0f);
}

static int ClampByte(int v) {
  return v < 0 ? 0 : v > 255 ? 255 : v;
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
  uint32 c = *p;
  if (IsHud(c))
    return;
  int cr = ClampByte(((c >> 16) & 0xff) + (int)(r * a));
  int cg = ClampByte(((c >> 8) & 0xff) + (int)(g * a));
  int cb = ClampByte((c & 0xff) + (int)(b * a));
  *p = (c & 0xff000000) | cr << 16 | cg << 8 | cb;
}

// Sposta il pixel verso un colore, tenendo il layer
static inline void MixPixel(uint32 *p, int r, int g, int b, float a) {
  uint32 c = *p;
  int cr = (c >> 16) & 0xff, cg = (c >> 8) & 0xff, cb = c & 0xff;
  cr += (int)((r - cr) * a), cg += (int)((g - cg) * a), cb += (int)((b - cb) * a);
  *p = (c & 0xff000000) | ClampByte(cr) << 16 | ClampByte(cg) << 8 | ClampByte(cb);
}

// Segna il pixel come "sprite": lo shader four sword non lo dipinge (lo
// cancellerebbe, essendo largo un solo pixel) ma gli applica il colore
static inline void MarkSharp(uint32 *p) {
  if (!IsSprite(*p) && !IsHud(*p))
    *p = (*p & 0x00ffffff) | 4u << 24;
}

static bool InGameplay(int *mod_out) {
  int mod = main_module_index;
  if (mod == 14) {
    int sub = submodule_index;
    if (sub != 1 && sub != 2 && sub != 11)
      return false;
    mod = saved_module_for_menu;
  }
  *mod_out = mod;
  return mod == 7 || mod == 9;
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

static inline uint32 MixColor(uint32 a, uint32 b, float k) {
  int ra = (a >> 16) & 0xff, ga = (a >> 8) & 0xff, ba = a & 0xff;
  int rb = (b >> 16) & 0xff, gb = (b >> 8) & 0xff, bb = b & 0xff;
  return (a & 0xff000000) | (uint32)(ra + (int)((rb - ra) * k)) << 16 |
         (uint32)(ga + (int)((gb - ga) * k)) << 8 | (uint32)(ba + (int)((bb - ba) * k));
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
    float wave = sinf((y + cam_y) * 0.30f + t) * 0.55f;
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
      float shift = wave * m;
      int sx = x + (shift > 0 ? 1 : -1);
      if (sx < 0 || sx >= width || IsHud(tmp[sx]) || IsSprite(tmp[sx]))
        continue;
      row[x] = MixColor(tmp[x], tmp[sx], fabsf(shift));
    }
  }
}

void HdFx_BeforeLight(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  int mod;
  g_any_lava = false;
  if (!g_fx_heat_haze || !InGameplay(&mod))
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
  uint8 kind;  // 0 = brace, 1 = pulviscolo, 2 = spruzzo di pioggia, 3 = fumo dell'incendio
} Particle;

enum { kMaxParticles = 400 };
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
    if (p->kind == 0)
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

static void SpawnEmbers(int cam_x, int cam_y, int ext) {
  for (int i = 0; i < g_hd_num_fire; i++) {
    HdFire *f = &g_hd_fire[i];
    if (Rand01() > 0.09f * f->strength)
      continue;
    Particle *p = NewParticle();
    if (!p)
      return;
    p->kind = 0;
    p->x = f->x - ext + cam_x + (Rand01() - 0.5f) * 8;
    p->y = f->y + cam_y - 4 + (Rand01() - 0.5f) * 4;
    p->vx = (Rand01() - 0.5f) * 0.3f;
    p->vy = -0.40f - Rand01() * 0.50f;
    p->life = 45 + (int)(Rand01() * 55);
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

typedef struct WindowShaft {
  uint16 x, y, w, h;  // zona di luce della finestra, in pixel nella stanza
  uint8 layer;        // 0 = BG2, 1 = BG1
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
        sh->x = col * 8, sh->y = row * 8, sh->w = w * 8, sh->h = h * 8, sh->layer = layer;
      }
    }
#undef IS_LIGHT
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
  for (int k = 0; k < 10; k++) {
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
static float g_flash, g_flash_target;
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

int HdFx_SceneModule(void) {
  int m = main_module_index, sub = submodule_index;
  if (m == 14) {
    // inventario (1), dialoghi (2) e menu salva/esci (11) si aprono sopra il
    // gioco, che resta sotto
    if (sub == 1 || sub == 2 || sub == 11)
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
  // transizione dello specchio (ondeggiamento e caricamento dell'altro mondo)
  if (m == 9 && (sub == 35 || sub == 44))
    return 14;
  return m;
}

void HdFx_MarkWater(uint8 *pixels, int pitch, int width, int height, int bias_x, int bias_y) {
  // anche durante il cerchio di transizione e l'arrivo con l'Ocarina:
  // altrimenti lo shader dipingerebbe l'acqua per qualche fotogramma
  int mod = HdFx_SceneModule();
  if (mod != 7 && mod != 9)
    return;
  // sulla Montagna della Morte e alla Piramide il vuoto oltre i dirupi e'
  // "acqua profonda" per il gioco, ma sullo schermo e' il cielo: lo sfondo
  // resta sfondo (HdFx_MarkBackdrop), invece di diventare acqua a blocchi
  bool sky = mod == 9 && IsBackdropScene();
  bool mountain = mod == 9 && IsMountainScene();
  int ext = (width - 256) / 2;
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
        bool cloud = false;
        if (mod == 7)
          attr = dung_bg2_attr_table[((wy & 0x1f8) << 3) + ((wx & 0x1f8) >> 3) + (layer ? 0 : 0x1000)];
        else if (HdWorld_Active() && (HdWorld_BackgroundFromImages() || !HdWorld_InsideCurrentArea(wx, wy)))
          continue;  // fondale dalle immagini delle aree, con l'acqua gia' segnata; nel
                     // passaggio la mappa del gioco e' ancora quella dell'area vecchia
        else {
          attr = Overworld_GetTileAttributeAtLocation((uint16)wx >> 3, (uint16)wy);
          cloud = mountain && (Overworld_GetMap8AtLocation((uint16)wx >> 3, (uint16)wy) >> 10 & 7) == 3;
        }
        if (!IsWaterAttr(attr) && !cloud)
          continue;
        for (int y = wy - cam_y; y < wy - cam_y + 8; y++) {
          if (y < 0 || y >= height)
            continue;
          uint32 *row = (uint32 *)(pixels + y * pitch);
          for (int x = wx - cam_x + ext; x < wx - cam_x + ext + 8; x++) {
            if (x < 0 || x >= width)
              continue;
            uint32 l = row[x] >> 24;
            // solo il fondale del layer giusto: HUD (2) e personaggi (4, 6) restano
            if (l == (uint32)(layer ? 1 : 0) || (layer && l == 5 && !sky))
              row[x] = (row[x] & 0x00ffffff) | (uint32)kLayerWater << 24;
          }
        }
      }
    }
  }
}

// All'aperto con il sole: niente pioggia, niente nebbia del Bosco Perduto
// o della Foresta Maledetta, niente nuvole del Monte della Morte (chiaro e
// oscuro), niente aree speciali come il bosco della Spada Suprema.
bool HdFx_SunnyOutdoors(void) {
  int mod;
  // Anche mentre si carica l'esterno e il cerchio si apre o si chiude
  // all'aperto (uscendo da una casa, entrandoci): altrimenti le nuvole
  // comparirebbero di colpo un attimo dopo
  bool iris = (main_module_index == 8 || main_module_index == 15 || main_module_index == 16) && !player_is_indoors;
  if (iris)
    mod = 9;
  else if (!InGameplay(&mod) || mod != 9)
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
        uint32 v = *c;
        int r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
        r += (int)((205 - r) * k), g += (int)((220 - g) * k), b += (int)((245 - b) * k);
        *c = (v & 0xff000000) | ClampByte(r) << 16 | ClampByte(g) << 8 | ClampByte(b);
        MarkSharp(c);
      }
    }
  }
}

static void ApplyLightning(uint8 *pixels, int pitch, int width, int height, bool step) {
  // Con l'effetto attivo il gioco non fa piu' i suoi lampeggi rapidi
  // (OverworldOverlay_HandleRain): resta un solo lampo, sul tuono, che si
  // accende in pochi frame e si spegne lentamente.
  if (step) {
    if (frame_counter == 36)
      g_flash_target = 1.0f, g_flash_rise = true;
    if (g_flash_rise) {
      g_flash += 0.25f;
      if (g_flash >= g_flash_target)
        g_flash = g_flash_target, g_flash_rise = false;
    } else {
      g_flash *= 0.95f;
    }
  }
  if (g_flash < 0.02f)
    return;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++) {
      if (IsHud(row[x]))
        continue;
      uint32 c = row[x];
      int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
      float a = g_flash * 0.40f;
      r += (int)((235 - r) * a), g += (int)((240 - g) * a), b += (int)((255 - b) * a);
      row[x] = (c & 0xff000000) | ClampByte(r) << 16 | ClampByte(g) << 8 | ClampByte(b);
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
      int v = (p01 >> bit & 1) | (p01 >> (8 + bit) & 1) << 1 | (p23 >> bit & 1) << 2 | (p23 >> (8 + bit) & 1) << 3;
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

static void ApplyBurning(uint8 *pixels, int pitch, int width, int height, int cam_x, int cam_y, int ext, bool step) {
  const HdBurnTile *tiles;
  int n = HdBurn_GetScorched(&tiles);
  bool live = main_module_index == 9 && submodule_index == 0;
  for (int i = 0; i < n; i++) {
    const HdBurnTile *s = &tiles[i];
    if (s->area != (uint8)overworld_area_index)
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
  int order[96];
  int m = 0;
  for (int i = 0; i < n && m < 96; i++) {
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
  if (!InGameplay(&mod)) {
    g_num_particles = 0;
    return;
  }
  int ext = (width - 256) / 2;
  int cam_x = BG2HOFS_copy2 + bias_x, cam_y = BG2VOFS_copy2 + bias_y;
  bool step = NewGameFrame();
  if (step) {
    g_fx_frames++;
    UpdateParticles();
    if (g_fx_embers)
      SpawnEmbers(cam_x, cam_y, ext);
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
    ApplyLightning(pixels, pitch, width, height, step);
  } else {
    g_flash = 0, g_flash_rise = false;
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

static float g_blur_amount;  // 0 = niente sfocatura, 1 = piena

bool HdFx_MenuBlurActive(void) {
  // sulla mappa del mondo (Mode 7) la sfocatura non viene disegnata
  return g_blur_amount > 0 && g_zenv.ppu->mode != 7;
}

void HdFx_MenuBlur(uint8 *pixels, int pitch, int width, int height) {
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
      row[x] = (c & 0xff000000) | ClampByte(r) << 16 | ClampByte(g) << 8 | ClampByte(bl);
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
  if (!g_iris_override)
    return;
  memcpy(g_iris_saved_table, hdma_table_dynamic, sizeof(g_iris_saved_table));
  for (int i = 0; i < 240; i++)
    hdma_table_dynamic[i] = 0xff00;  // finestra su tutta la riga
  ppu->windowFullSides = true;
}

void HdFx_IrisAfterRender(Ppu *ppu) {
  if (!g_iris_override)
    return;
  memcpy(hdma_table_dynamic, g_iris_saved_table, sizeof(g_iris_saved_table));
  ppu->windowFullSides = false;
}

void HdFx_SoftIris(uint8 *pixels, int pitch, int width, int height) {
  if (!g_iris_override)
    return;
  const float kSoft = 8.0f;
  int ext = (width - 256) / 2;
  float cx = g_hd_iris_x + ext + 0.5f, cy = g_hd_iris_y + 0.5f;
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
// Ombre di personaggi, nemici e oggetti: la sagoma di ogni sprite viene
// "sdraiata" a terra partendo dai piedi (il pixel piu' basso di ogni colonna),
// inclinata verso il basso a destra come le ombre dello shader, sfocata e
// usata per scurire il fondale. Sprite e HUD non vengono scuriti.

static float *g_shadow, *g_shadow_tmp;
static int g_shadow_cap;

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
  if (!g_fx_sprite_shadows || !InGameplay(&mod))
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
      if (!IsSprite(*(uint32 *)(pixels + y * pitch + x * 4))) {
        y++;
        continue;
      }
      int y0 = y;
      while (y < height && IsSprite(*(uint32 *)(pixels + y * pitch + x * 4)))
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
void HdFx_MarkBackdrop(uint8 *pixels, int pitch, int width, int height) {
  int mod;
  if (!InGameplay(&mod) || mod != 9 || !IsBackdropScene())
    return;
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + y * pitch);
    for (int x = 0; x < width; x++)
      if ((row[x] >> 24) == 5)
        row[x] = (row[x] & 0x00ffffff) | 3u << 24;
  }
}
