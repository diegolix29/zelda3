// 3D view: the world tilted in perspective, optionally curved like a small
// planet, with characters standing up as flat cutouts.
//
// How a 3D frame is made (all in software, so it works with every output
// method and shader):
// - the PPU draws the frame WITHOUT sprites and BG3 (skipLayers). That is the
//   ground, and every HD effect (water, light, shadows, continuous world)
//   still runs on it as usual;
// - after each scanline the same line is drawn again with everything into a
//   side buffer (ppu_redrawLine), remembering which OAM entry owns each sprite
//   pixel (ppu->objOwnerLine);
// - at the end of the frame every screen pixel casts a ray from the camera.
//   It hits a flat ground, or a big sphere when the world curve is on (rays
//   that miss the sphere are sky: a round horizon). Where it lands inside the
//   SNES screen it takes the PPU ground; beyond it, outdoors it reads the area
//   pictures of the continuous world (hd_world.c), indoors the room tiles in
//   video memory;
// - sprites are grouped by owner (Link, each enemy, each item) and stood up at
//   their feet, scaled with distance; BG3 (HUD, dialogue boxes) is put back
//   flat on the screen.
//
// Nothing has height yet: walls, cliffs and trees are still painted on the
// ground.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <SDL.h>
#include "hd_3d.h"
#include "hd_world.h"
#include "hd_fx.h"
#include "zelda_rtl.h"
#include "snes/ppu.h"
#include "variables.h"
#include "config.h"
#include "features.h"

// same as in snes/ppu.c: the smooth camera offset applies to BG1 and BG2
#define BG_BIAS_X(ppu, layer) ((layer) < 2 ? (ppu)->bgBiasX : 0)
#define BG_BIAS_Y(ppu, layer) ((layer) < 2 ? (ppu)->bgBiasY : 0)

extern uint8 g_link_oam_mask[128];      // player_oam.c
extern uint8 g_sprite_oam_owner[128];   // sprite.c

int g_view3d;
int g_view3d_curve;
int g_view3d_dist;

static const char *const kDistNames[kHd3d_NumDists] = { "Screen", "2x", "3x" };

static const float kLevelDegrees[kHd3d_NumLevels] = { 0.0f, 20.0f, 35.0f, 50.0f, 65.0f, 75.0f };
static const char *const kLevelNames[kHd3d_NumLevels] = { "Off", "20", "35", "50", "65", "75" };
// sphere radius in SNES pixels per curve level (0 = flat)
static const float kCurveRadius[kHd3d_NumCurves] = { 0.0f, 3000.0f, 1600.0f, 900.0f };
static const char *const kCurveNames[kHd3d_NumCurves] = { "Off", "Light", "Medium", "Strong" };

// Distance of the camera from the ground, in SNES pixels. Also the focal
// length, so with no tilt the picture is exactly the 2D one.
static const float kCamDist = 320.0f;
// Distance fog: starts at kFogStart, all sky at kFogEnd (camera distance)
static const float kFogStart = 450.0f, kFogEnd = 1100.0f;

static FILE *g_log;
static float g_angle;            // current tilt in degrees (eases toward the target)
static float g_radius_inv;       // current 1 / sphere radius (eases too)
static bool g_active_last;
static bool g_weather;           // this frame: weather layer kept out of the ground, added flat at the end
static bool g_weather_last;
static int g_last_reason = -1;
static int g_last_source = -1;

// frame buffers (size of the frame being drawn)
static int g_w, g_h;
static uint32 *g_side;           // full frame, sprites and BG3 included
static uint8 *g_owner;           // OAM index per pixel of g_side, 0xff = no sprite
static uint32 *g_floor;          // copy of the ground before re-projection
static uint8 *g_group_map;       // sprite group + 1 per pixel, 0 = none

// stats for the log
static uint64 g_stat_ticks;
static int g_stat_frames, g_stat_groups, g_stat_max_groups;
static int g_stat_outside, g_stat_missing;

void Hd3d_Log(const char *fmt, ...) {
  if (!g_log)
    return;
  fprintf(g_log, "[%8.3f] ", SDL_GetTicks() / 1000.0);
  va_list va;
  va_start(va, fmt);
  vfprintf(g_log, fmt, va);
  va_end(va);
  fputc('\n', g_log);
  fflush(g_log);
}

static float RadiusInv(int curve) {
  float r = kCurveRadius[(unsigned)curve % kHd3d_NumCurves];
  return r > 0 ? 1.0f / r : 0.0f;
}

void Hd3d_Init(void) {
  if (!g_log)
    g_log = fopen("zelda3_3d.log", "w");
  g_view3d = g_config.view3d % kHd3d_NumLevels;
  g_view3d_curve = g_config.view3d_curve % kHd3d_NumCurves;
  g_view3d_dist = g_config.view3d_dist % kHd3d_NumDists;
  g_angle = kLevelDegrees[g_view3d];
  g_radius_inv = RadiusInv(g_view3d_curve);
  Hd3d_Log("3D view init: View3D=%d (%s deg), View3DCurve=%d (%s), View3DDrawDistance=%d (%s), NewRenderer=%d, "
           "OutputMethod=%d, ExtendedAspectRatio=%d, ContinuousWorld=%d, shader='%s'",
           g_view3d, kLevelNames[g_view3d], g_view3d_curve, kCurveNames[g_view3d_curve],
           g_view3d_dist, kDistNames[g_view3d_dist],
           g_config.new_renderer, g_config.output_method, g_config.extended_aspect_ratio,
           (g_config.features0 & kFeatures0_ContinuousWorld) != 0, g_config.shader ? g_config.shader : "");
  if (!g_config.new_renderer)
    Hd3d_Log("WARNING: NewRenderer is off, the 3D view needs it (zelda3.ini [Graphics] NewRenderer = 1)");
  if (!(g_config.features0 & kFeatures0_ContinuousWorld))
    Hd3d_Log("NOTE: ContinuousWorld is off: outdoors the ground past the screen comes from video memory and may show stale tiles");
}

const char *Hd3d_LevelName(int level) {
  return kLevelNames[(unsigned)level % kHd3d_NumLevels];
}

const char *Hd3d_CurveName(int curve) {
  return kCurveNames[(unsigned)curve % kHd3d_NumCurves];
}

void Hd3d_SetLevel(int level, const char *why) {
  level = (unsigned)level % kHd3d_NumLevels;
  if (level != g_view3d)
    Hd3d_Log("tilt -> %s deg (%s)", kLevelNames[level], why);
  g_view3d = level;
  g_config.view3d = (uint8)level;
}

void Hd3d_CycleLevel(void) {
  Hd3d_SetLevel(g_view3d + 1, "hotkey");
}

void Hd3d_SetCurve(int curve, const char *why) {
  curve = (unsigned)curve % kHd3d_NumCurves;
  if (curve != g_view3d_curve)
    Hd3d_Log("curve -> %s (%s)", kCurveNames[curve], why);
  g_view3d_curve = curve;
  g_config.view3d_curve = (uint8)curve;
}

void Hd3d_CycleCurve(void) {
  Hd3d_SetCurve(g_view3d_curve + 1, "hotkey");
}

const char *Hd3d_DrawDistanceName(int dist) {
  return kDistNames[(unsigned)dist % kHd3d_NumDists];
}

void Hd3d_SetDrawDistance(int dist, const char *why) {
  dist = (unsigned)dist % kHd3d_NumDists;
  if (dist != g_view3d_dist)
    Hd3d_Log("sprite draw distance -> %s (%s)", kDistNames[dist], why);
  g_view3d_dist = dist;
  g_config.view3d_dist = (uint8)dist;
}

void Hd3d_CycleDrawDistance(void) {
  Hd3d_SetDrawDistance(g_view3d_dist + 1, "hotkey");
}

bool Hd3d_FarSprites(int *margin_x, int *margin_top, int *margin_bottom) {
  // the 3D view was drawn last frame; outdoors only with the continuous world
  // (it keeps every enemy of the area alive), indoors in any room
  if (!g_active_last || g_view3d_dist == 0 || !(HdWorld_Active() || player_is_indoors))
    return false;
  *margin_x = 256 * g_view3d_dist;
  *margin_top = 256 * g_view3d_dist;   // far away = up the screen
  *margin_bottom = 16;
  return true;
}

// ---------------------------------------------------------------------------

enum {
  kReason_On,
  kReason_Off,
  kReason_NotGameplay,
  kReason_Mode7,
  kReason_ForcedBlank,
  kReason_OldRenderer,
};
static const char *const kReasonNames[] = {
  "3D on", "tilt off", "not gameplay", "mode 7 (map/special)", "forced blank", "old renderer",
};

static bool IsGameplayModule(void) {
  int mod = main_module_index;
  // 7 dungeon/indoors, 9 overworld, 15/16 iris transitions, 17 falling into a hole
  if (mod == 7 || mod == 9 || mod == 15 || mod == 16 || mod == 17)
    return true;
  // 14 = menus and dialogue over the game
  if (mod == 14)
    return saved_module_for_menu == 7 || saved_module_for_menu == 9;
  return false;
}

static bool EnsureBuffers(int w, int h) {
  if (w == g_w && h == g_h && g_side)
    return true;
  free(g_side), free(g_owner), free(g_floor), free(g_group_map);
  g_side = (uint32 *)malloc(sizeof(uint32) * w * h);
  g_owner = (uint8 *)malloc(w * h);
  g_floor = (uint32 *)malloc(sizeof(uint32) * w * h);
  g_group_map = (uint8 *)malloc(w * h);
  if (!g_side || !g_owner || !g_floor || !g_group_map) {
    Hd3d_Log("ERROR: out of memory for %dx%d buffers", w, h);
    g_w = g_h = 0;
    return false;
  }
  g_w = w, g_h = h;
  Hd3d_Log("buffers %dx%d", w, h);
  return true;
}

bool Hd3d_BeginFrame(Ppu *ppu, int width, int height) {
  float target = kLevelDegrees[g_view3d];
  g_angle += (target - g_angle) * 0.15f;
  if (fabsf(target - g_angle) < 0.05f)
    g_angle = target;
  float rtarget = RadiusInv(g_view3d_curve);
  g_radius_inv += (rtarget - g_radius_inv) * 0.15f;
  if (fabsf(rtarget - g_radius_inv) < 1e-6f)
    g_radius_inv = rtarget;

  int reason;
  if (g_angle < 0.05f)
    reason = kReason_Off;
  else if (!(ppu->renderFlags & kPpuRenderFlags_NewRenderer))
    reason = kReason_OldRenderer;
  else if (ppu->mode == 7)
    reason = kReason_Mode7;
  else if (ppu->forcedBlank)
    reason = kReason_ForcedBlank;
  else if (!IsGameplayModule())
    reason = kReason_NotGameplay;
  else
    reason = kReason_On;

  if (reason != g_last_reason && reason != kReason_ForcedBlank) {
    Hd3d_Log("%s (module=%d sub=%d saved=%d mode=%d tilt=%.1f)", kReasonNames[reason],
             main_module_index, submodule_index, saved_module_for_menu, ppu->mode, g_angle);
    g_last_reason = reason;
  }
  g_fx_flat_weather = reason == kReason_On;
  if (reason != kReason_On || !EnsureBuffers(width, height)) {
    g_active_last = false;
    g_fx_flat_weather = false;
    g_weather = false;
    HdWorld_SuppressOverlay(false);
    return false;
  }
  memset(g_owner, 0xff, width * height);
  // Outdoors the weather (rain sheet, storm darkness, Lost Woods fog) is BG1
  // added on the subscreen: keep it out of the ground and the sprites, and
  // add it flat over the whole view at the end
  g_weather = HdWorld_OverlayPresent();
  if (g_weather != g_weather_last) {
    Hd3d_Log("weather layer %s (area=0x%02x)", g_weather ? "kept flat over the view" : "none",
             BYTE(overworld_screen_index));
    g_weather_last = g_weather;
  }
  HdWorld_SuppressOverlay(g_weather);
  ppu->skipLayers = 4 | 16 | (g_weather ? 1 : 0);   // ground only: no BG3, no sprites, no weather
  g_active_last = true;
  return true;
}

void Hd3d_CaptureLine(Ppu *ppu, int line) {
  if (line < 1 || line > g_h)
    return;
  // ppu_redrawLine writes row (line - 1) of the buffer itself: pass the start
  uint32 *row = g_side + (line - 1) * g_w;
  ppu_redrawLine(ppu, line, (uint8 *)g_side, g_w * 4, g_weather ? 1 : 0);
  // pixel column c <-> PPU line buffer index (see PpuDrawWholeLine)
  uint8 *own = g_owner + (line - 1) * g_w;
  int base = kPpuExtraLeftRight - ppu->extraLeftRight;
  for (int c = 0; c < g_w; c++) {
    if ((row[c] >> 24 & 0xf) == 4) {
      int i = c + base;
      own[c] = (i >= 0 && i < kPpuXPixels) ? ppu->objOwnerLine[i] : 0xff;
    }
  }
}

void Hd3d_AfterLines(Ppu *ppu) {
  ppu->skipLayers = 0;
}

// ---------------------------------------------------------------------------
// Ground beyond the SNES screen

enum {
  kSource_None,    // sky / black
  kSource_World,   // continuous world area pictures (outdoors)
  kSource_Vram,    // tiles in video memory (indoors, or outdoors without continuous world)
};
static const char *const kSourceNames[] = { "none", "world pictures", "video memory" };

typedef struct Ground {
  int source;
  int org_x, org_y;   // kSource_World: world pixel of frame pixel (0, 0)
  Ppu *ppu;
} Ground;

// One BG layer pixel from video memory: returns the cgram index (0 = transparent)
static int VramLayerPixel(Ppu *ppu, int layer, int x, int y, int *prio) {
  BgLayer *bg = &ppu->bgLayer[layer];
  x += bg->hScroll + BG_BIAS_X(ppu, layer);
  y += bg->vScroll + BG_BIAS_Y(ppu, layer);
  int sc = bg->tilemapAdr + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && bg->tilemapHigher)
    sc += bg->tilemapWider ? 0x800 : 0x400;
  if ((x & 0x100) && bg->tilemapWider)
    sc += 0x400;
  uint16 e = ppu->vram[(sc + ((x >> 3) & 0x1f)) & 0x7fff];
  int row = y & 7, col = x & 7;
  if (e & 0x8000) row = 7 - row;
  if (e & 0x4000) col = 7 - col;
  const uint16 *t = &ppu->vram[(bg->tileAdr + (e & 0x3ff) * 16 + row) & 0x7fff];
  int bit = 7 - col;
  int v = (t[0] >> bit & 1) | (t[0] >> (8 + bit) & 1) << 1 | (t[8] >> bit & 1) << 2 | (t[8] >> (8 + bit) & 1) << 3;
  *prio = e >> 13 & 1;
  return v ? ((e >> 10 & 7) * 16 + v) : 0;
}

static uint32 CgramToPixel(Ppu *ppu, int index, uint32 layer) {
  uint16 c = ppu->cgram[index];
  return layer << 24 | ppu->brightnessMult[c & 0x1f] << 16 |
         ppu->brightnessMult[(c >> 5) & 0x1f] << 8 | ppu->brightnessMult[(c >> 10) & 0x1f];
}

// BG1 and BG2 combined by priority, as the PPU does (BG3 and sprites excluded)
static uint32 SampleVram(Ppu *ppu, int fx, int fy) {
  int x = fx - ppu->extraLeftRight, y = fy + 1;   // frame row 0 = PPU line 1
  int idx[2] = { 0, 0 }, pr[2] = { 0, 0 };
  for (int l = 0; l < 2; l++)
    if (ppu->screenEnabled[0] & (1 << l))
      idx[l] = VramLayerPixel(ppu, l, x, y, &pr[l]);
  // order: BG1 high, BG2 high, BG1 low, BG2 low
  if (idx[0] && pr[0]) return CgramToPixel(ppu, idx[0], 0);
  if (idx[1] && pr[1]) return CgramToPixel(ppu, idx[1], 1);
  if (idx[0]) return CgramToPixel(ppu, idx[0], 0);
  if (idx[1]) return CgramToPixel(ppu, idx[1], 1);
  return CgramToPixel(ppu, 0, 5);
}

static void Ground_Setup(Ground *g, Ppu *ppu, int width) {
  g->ppu = ppu;
  if (HdWorld_Active()) {
    g->source = kSource_World;
    HdWorld_ViewOrigin(width, &g->org_x, &g->org_y);
  } else {
    g->source = kSource_Vram;
  }
  if (g->source != g_last_source) {
    Hd3d_Log("ground past the screen: %s (module=%d area=0x%02x indoors=%d)", kSourceNames[g->source],
             main_module_index, BYTE(overworld_screen_index), player_is_indoors);
    g_last_source = g->source;
  }
}

// ground pixel at frame coordinates outside the SNES screen; false = nothing there
static bool Ground_Sample(const Ground *g, int fx, int fy, uint32 *out) {
  switch (g->source) {
  case kSource_World:
    return HdWorld_SampleGround(g->org_x + fx, g->org_y + fy, out);
  case kSource_Vram:
    *out = SampleVram(g->ppu, fx, fy);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Camera
//
// Ground coordinates (u, v) are pixels of the 2D frame, z points up. The
// camera looks at the ground point (cx, p) from distance D, tilted by th
// toward the bottom of the screen, with focal length D (so with no tilt the
// picture is exactly the 2D one). p is chosen so the bottom edge of the
// ground sits on the bottom of the screen.
//
// With the curve on, the ground is a sphere of radius R touching the plane at
// (cx, p). A ray's hit point is mapped back to (u, v) by arc length, so the
// map is wrapped onto the sphere without stretching.

typedef struct Cam {
  float D, s, c, cx, cy, p;
  float R;                      // 0 = flat
  float ox, oy, oz;             // camera position
  float sy_oy, sy_oz;           // O = C - sphere center (x = 0)
} Cam;

static void Cam_Setup(Cam *cm, int w, int h) {
  float th = g_angle * (3.14159265f / 180.0f);
  cm->D = kCamDist, cm->s = sinf(th), cm->c = cosf(th);
  cm->cx = w * 0.5f, cm->cy = h * 0.5f;
  float dy = (h - 0.5f) - cm->cy;
  float t = cm->D * cm->c / (cm->D * cm->c + dy * cm->s);
  cm->p = h - (cm->D * cm->s + t * (dy * cm->c - cm->D * cm->s));
  cm->R = g_radius_inv > 1e-6f ? 1.0f / g_radius_inv : 0.0f;
  cm->ox = cm->cx, cm->oy = cm->p + cm->D * cm->s, cm->oz = cm->D * cm->c;
  cm->sy_oy = cm->D * cm->s, cm->sy_oz = cm->D * cm->c + cm->R;
}

// screen pixel center -> ground (u, v) and camera depth; false = sky
static bool Cam_Ray(const Cam *cm, float sx, float sy, float *u, float *v, float *depth) {
  float dx = sx - cm->cx, dy = sy - cm->cy;
  float rx = dx, ry = -cm->D * cm->s + dy * cm->c, rz = -cm->D * cm->c - dy * cm->s;
  float lam;
  if (cm->R == 0.0f) {
    if (rz >= -1e-4f)
      return false;
    lam = -cm->oz / rz;
    *u = cm->ox + lam * rx;
    *v = cm->oy + lam * ry;
  } else {
    float a = rx * rx + ry * ry + rz * rz;
    float b = cm->sy_oy * ry + cm->sy_oz * rz;
    float cc = cm->sy_oy * cm->sy_oy + cm->sy_oz * cm->sy_oz - cm->R * cm->R;
    float disc = b * b - a * cc;
    if (disc < 0)
      return false;
    lam = (-b - sqrtf(disc)) / a;
    if (lam <= 0)
      return false;
    float hx = lam * rx, hy = cm->oy + lam * ry - cm->p;   // relative to the touch point
    float r = sqrtf(hx * hx + hy * hy);
    float k = 1.0f;
    if (r > 1e-3f) {
      float q = r / cm->R;
      k = cm->R * asinf(q > 1.0f ? 1.0f : q) / r;
    }
    *u = cm->cx + hx * k;
    *v = cm->p + hy * k;
  }
  *depth = lam * cm->D;
  return true;
}

// ground (u, v) -> screen point and scale (screen pixels per ground pixel)
static bool Cam_ToScreen(const Cam *cm, float u, float v, float *sx, float *sy, float *scale) {
  float x = u, y = v, z = 0.0f;
  if (cm->R != 0.0f) {
    float hx = u - cm->cx, hy = v - cm->p;
    float r = sqrtf(hx * hx + hy * hy);
    if (r > 1e-3f) {
      float phi = r / cm->R;
      if (phi > 1.5f)
        return false;
      float k = cm->R * sinf(phi) / r;
      x = cm->cx + hx * k, y = cm->p + hy * k;
      z = cm->R * cosf(phi) - cm->R;
    }
  }
  float rx = x - cm->ox, ry = y - cm->oy, rz = z - cm->oz;
  float zc = -cm->s * ry - cm->c * rz;          // along the view direction
  if (zc < 1.0f)
    return false;
  float up = -cm->c * ry + cm->s * rz;
  *sx = cm->cx + cm->D * rx / zc;
  *sy = cm->cy - cm->D * up / zc;
  *scale = cm->D / zc;
  return true;
}

// ---------------------------------------------------------------------------
// Drawing

static inline uint32 Lerp(uint32 a, uint32 b, int f /* 0-256 */) {
  uint32 rb = ((a & 0xff00ff) * (256 - f) + (b & 0xff00ff) * f) >> 8 & 0xff00ff;
  uint32 g = ((a & 0x00ff00) * (256 - f) + (b & 0x00ff00) * f) >> 8 & 0x00ff00;
  return rb | g;
}

// ---------------------------------------------------------------------------
// Sky
//
// A gradient from the horizon to the zenith, and above it a layer of pixel-art
// clouds on a high flat ceiling seen in perspective (so they shrink toward the
// horizon). The ceiling is anchored to the world: walking moves you under the
// clouds, and they drift slowly with time. Grey in storms, dusk colors in the
// Dark World. Tagged as layer 3 (backdrop the shader leaves alone).

typedef struct Sky {
  bool outdoors;
  uint32 zenith, horizon, cloud_lit, cloud_shade;
  float off_x, off_y;   // world anchor + drift of the cloud ceiling
  float cover;          // how cloudy: 0-1
} Sky;

static const float kCloudHeight = 700.0f;   // cloud ceiling above the ground, pixels
static const float kCloudScale = 1.0f / 180.0f;

static inline uint32 HashXY(int x, int y) {
  uint32 h = (uint32)x * 0x8da6b343u ^ (uint32)y * 0xd8163841u;
  h ^= h >> 13, h *= 0x85ebca6bu, h ^= h >> 16;
  return h;
}

static float ValueNoise(float x, float y) {
  int xi = (int)floorf(x), yi = (int)floorf(y);
  float fx = x - xi, fy = y - yi;
  fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
  float a = (HashXY(xi, yi) & 0xffff) * (1.0f / 65535), b = (HashXY(xi + 1, yi) & 0xffff) * (1.0f / 65535);
  float c = (HashXY(xi, yi + 1) & 0xffff) * (1.0f / 65535), d = (HashXY(xi + 1, yi + 1) & 0xffff) * (1.0f / 65535);
  return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static void Sky_Setup(Sky *sk, bool outdoors, int width) {
  sk->outdoors = outdoors;
  bool dark = (overworld_screen_index & 0x40) != 0;
  bool storm = HdFx_IsRainingOutdoors();
  if (storm) {
    sk->zenith = 0x4c5866, sk->horizon = 0x8a96a2, sk->cloud_lit = 0x9aa4ae, sk->cloud_shade = 0x6c7682;
    sk->cover = 0.85f;
  } else if (dark) {
    sk->zenith = 0x3a2a5c, sk->horizon = 0xc0889a, sk->cloud_lit = 0xd8a0b0, sk->cloud_shade = 0x8a6488;
    sk->cover = 0.45f;
  } else {
    sk->zenith = 0x3c78d8, sk->horizon = 0xbcdcf4, sk->cloud_lit = 0xf8f8f8, sk->cloud_shade = 0xc4d4ec;
    sk->cover = 0.42f;
  }
  int ox = 0, oy = 0;
  if (outdoors)
    HdWorld_ViewOrigin(width, &ox, &oy);
  float t = SDL_GetTicks() * 0.004f;   // slow drift, 4 pixels a second
  sk->off_x = ox + t, sk->off_y = oy + t * 0.35f;
}

// sky color for the ray through screen pixel (sx, sy)
static uint32 Sky_Color(const Sky *sk, const Cam *cm, float sx, float sy, int px, int py) {
  if (!sk->outdoors)
    return 5u << 24;
  float dx = sx - cm->cx, dy = sy - cm->cy;
  float rx = dx, ry = -cm->D * cm->s + dy * cm->c, rz = -cm->D * cm->c - dy * cm->s;
  float len = sqrtf(rx * rx + ry * ry + rz * rz);
  float e = rz / len;                       // sin(elevation): 0 horizon, 1 straight up
  if (e <= 0)
    return 3u << 24 | sk->horizon;
  float f = e * 1.6f;
  uint32 c = Lerp(sk->horizon, sk->zenith, f >= 1 ? 256 : (int)(f * 256));
  if (e > 0.015f) {
    float lam = (kCloudHeight - cm->oz) / rz;
    float u = (cm->ox + lam * rx + sk->off_x) * kCloudScale;
    float v = (cm->oy + lam * ry + sk->off_y) * kCloudScale;
    float n = ValueNoise(u, v) * 0.65f + ValueNoise(u * 2.3f + 17.1f, v * 2.3f + 5.3f) * 0.35f;
    // pixel-art: two cloud shades, dithered edge (2x2 ordered)
    static const float kBayer[4] = { 0.0f, 0.5f, 0.75f, 0.25f };
    float dither = (kBayer[(py & 1) * 2 + (px & 1)] - 0.5f) * 0.06f;
    float edge = 1.0f - sk->cover;
    float fade = e < 0.12f ? e / 0.12f : 1.0f;     // clouds thin out at the horizon
    if (n + dither > edge) {
      uint32 cloud = (n + dither > edge + 0.12f) ? sk->cloud_lit : sk->cloud_shade;
      c = Lerp(c, cloud, (int)(fade * 230));
    }
  }
  return 3u << 24 | c;
}

static void DrawGround(uint32 *out, int pitch_px, const Cam *cm, const Ground *gr, const Sky *sk) {
  int w = g_w, h = g_h;
  // far ground fades into the horizon color (black indoors)
  uint32 fog = sk->outdoors ? 3u << 24 | sk->horizon : 5u << 24;
  for (int y = 0; y < h; y++) {
    uint32 *dst = out + y * pitch_px;
    for (int x = 0; x < w; x++) {
      float u, v, depth;
      if (!Cam_Ray(cm, x + 0.5f, y + 0.5f, &u, &v, &depth) || depth >= kFogEnd) {
        dst[x] = Sky_Color(sk, cm, x + 0.5f, y + 0.5f, x, y);
        continue;
      }
      int ui = (int)floorf(u), vi = (int)floorf(v);
      uint32 px;
      if (ui >= 0 && ui < w && vi >= 0 && vi < h) {
        px = g_floor[vi * w + ui];
      } else {
        g_stat_outside++;
        if (!Ground_Sample(gr, ui, vi, &px)) {
          g_stat_missing++;
          dst[x] = fog;
          continue;
        }
      }
      if (depth > kFogStart) {
        int f = (int)((depth - kFogStart) * (256.0f / (kFogEnd - kFogStart)));
        px = (px & 0xff000000) | Lerp(px, fog, f > 256 ? 256 : f);
      }
      dst[x] = px;
    }
  }
}

typedef struct Group {
  int key;
  int x0, y0, x1, y1;
  bool far;            // drawn from the OAM table (pieces outside the SNES screen)
  int n_ents;
  uint8 ents[24];      // OAM entries of a far group, in OAM order (first wins)
} Group;

enum { kMaxGroups = 64 };

static const uint8 kObjSizes[8][2] = {   // same as kSpriteSizes in snes/ppu.c
  {8, 16}, {8, 32}, {8, 64}, {16, 32},
  {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

// Group of each OAM entry this frame (OamGroups)
static int g_key_of[128];

static int GroupKey(int oam) {
  return g_key_of[oam];
}

static int UfFind(int *parent, int i) {
  while (parent[i] != i)
    i = parent[i] = parent[parent[i]];
  return i;
}

// Link's pieces are one group, each enemy/NPC slot one group. Pieces with no
// owner (Zelda and the other followers, items, effects) are joined when they
// touch, so a follower's head and body stand up together as one cutout.
static void OamGroups(const bool *vis, const int *fx, const int *fy, const int *sz) {
  int parent[128];
  for (int i = 0; i < 128; i++)
    parent[i] = i;
  for (int i = 0; i < 128; i++) {
    if (!vis[i] || g_link_oam_mask[i] || g_sprite_oam_owner[i] < 16)
      continue;
    for (int j = i + 1; j < 128; j++) {
      if (!vis[j] || g_link_oam_mask[j] || g_sprite_oam_owner[j] < 16)
        continue;
      // rectangles overlap or touch (1 pixel gap allowed)
      if (fx[i] <= fx[j] + sz[j] && fx[j] <= fx[i] + sz[i] &&
          fy[i] <= fy[j] + sz[j] && fy[j] <= fy[i] + sz[i]) {
        int a = UfFind(parent, i), b = UfFind(parent, j);
        if (a != b)
          parent[a > b ? a : b] = a < b ? a : b;
      }
    }
  }
  for (int i = 0; i < 128; i++) {
    if (g_link_oam_mask[i])
      g_key_of[i] = 0;                               // Link (body, shield, sword, shadow)
    else if (g_sprite_oam_owner[i] < 16)
      g_key_of[i] = 1 + g_sprite_oam_owner[i];       // an enemy / NPC slot
    else
      g_key_of[i] = 32 + UfFind(parent, i);          // touching unowned pieces
  }
}

static int CompareGroups(const void *a, const void *b) {
  const Group *ga = (const Group *)a, *gb = (const Group *)b;
  return ga->y1 - gb->y1;   // smaller y1 = farther away, drawn first
}

// OAM entry: visible, frame position and size, the way the PPU places it
// (true positions when ResolveSpritePositions found them, else the raw OAM)
static bool OamEntry(Ppu *ppu, int i, int *fx, int *fy, int *size) {
  int yy = ppu->oam[i * 2] >> 8;
  if (yy == 0xf0 && !(ppu->objYValid && ppu->objShowF0[i]))
    return false;
  int high = ppu->oam[0x100 + (i >> 3)] >> ((i & 7) * 2);
  *size = kObjSizes[ppu->objSize][(high >> 1) & 1];
  int x, y;
  if (ppu->objXValid) {
    x = ppu->objX[i];
  } else {
    x = (ppu->oam[i * 2] & 0xff) + (high & 1) * 256;
    if (x >= 256 + ppu->extraLeftRight)
      x -= 512;
  }
  if (ppu->objYValid)
    y = ppu->objY[i];
  else
    y = yy >= g_h ? yy - 256 : yy;
  *fx = x - ppu->objBiasX + ppu->extraLeftRight;
  *fy = y - ppu->objBiasY;
  return true;
}

// one pixel of an OAM entry (local coordinates), as the PPU fetches it
static bool OamPixel(Ppu *ppu, int i, int col, int row, int size, uint32 *out) {
  int oam1 = ppu->oam[i * 2 + 1];
  if (oam1 & 0x8000) row = size - 1 - row;
  if (oam1 & 0x4000) col = size - 1 - col;
  int obj_adr = (oam1 & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
  int tile = ((((oam1 & 0xff) >> 4) + (row >> 3)) << 4) | (((oam1 & 0xf) + (col >> 3)) & 0xf);
  const uint16 *a = &ppu->vram[(obj_adr + tile * 16 + (row & 7)) & 0x7fff];
  int bit = 7 - (col & 7);
  int v = (a[0] >> bit & 1) | (a[0] >> (8 + bit) & 1) << 1 | (a[8] >> bit & 1) << 2 | (a[8] >> (8 + bit) & 1) << 3;
  if (!v)
    return false;
  *out = CgramToPixel(ppu, 0x80 + 16 * ((oam1 >> 9) & 7) + v, 4);
  return true;
}

static int g_stat_far_groups;

static int DrawSprites(uint32 *out, int pitch_px, const Cam *cm, Ppu *ppu) {
  int w = g_w, h = g_h;
  Group groups[kMaxGroups];
  int16 key_to_group[32 + 128];
  memset(key_to_group, 0xff, sizeof(key_to_group));
  int n = 0;

  // Groups with a piece outside the SNES screen are drawn entirely from the
  // OAM table (the PPU only drew their on-screen part)
  bool far_key[32 + 128];
  memset(far_key, 0, sizeof(far_key));
  // far pieces need their true positions (ResolveSpritePositions runs while
  // the draw distance is on)
  bool far_ok = ppu->objXValid && ppu->objYValid && g_view3d_dist > 0;
  int efx[128], efy[128], esz[128];
  bool evis[128];
  for (int i = 0; i < 128; i++)
    evis[i] = OamEntry(ppu, i, &efx[i], &efy[i], &esz[i]);
  OamGroups(evis, efx, efy, esz);
  for (int i = 0; i < 128; i++) {
    if (far_ok && evis[i] && (efx[i] < 0 || efy[i] < 0 || efx[i] + esz[i] > w || efy[i] + esz[i] > h))
      far_key[GroupKey(i)] = true;
  }

  memset(g_group_map, 0, w * h);
  for (int i = 0; i < w * h; i++) {
    uint8 o = g_owner[i];
    if (o >= 128)
      continue;
    int key = GroupKey(o);
    if (far_key[key])
      continue;
    int gi = key_to_group[key];
    if (gi < 0) {
      if (n == kMaxGroups)
        continue;
      gi = key_to_group[key] = n++;
      groups[gi].key = key, groups[gi].far = false, groups[gi].n_ents = 0;
      groups[gi].x0 = groups[gi].y0 = 1 << 20;
      groups[gi].x1 = groups[gi].y1 = -(1 << 20);
    }
    Group *g = &groups[gi];
    int x = i % w, y = i / w;
    if (x < g->x0) g->x0 = x;
    if (x > g->x1) g->x1 = x;
    if (y < g->y0) g->y0 = y;
    if (y > g->y1) g->y1 = y;
    g_group_map[i] = (uint8)(gi + 1);
  }
  int far_groups = 0;
  for (int i = 0; i < 128; i++) {
    if (!evis[i] || !far_key[GroupKey(i)])
      continue;
    int key = GroupKey(i);
    int gi = key_to_group[key];
    if (gi < 0) {
      if (n == kMaxGroups)
        continue;
      gi = key_to_group[key] = n++;
      groups[gi].key = key, groups[gi].far = true, groups[gi].n_ents = 0;
      groups[gi].x0 = groups[gi].y0 = 1 << 20;
      groups[gi].x1 = groups[gi].y1 = -(1 << 20);
      far_groups++;
    }
    Group *g = &groups[gi];
    if (g->n_ents == (int)sizeof(g->ents))
      continue;
    g->ents[g->n_ents++] = (uint8)i;
    if (efx[i] < g->x0) g->x0 = efx[i];
    if (efx[i] + esz[i] - 1 > g->x1) g->x1 = efx[i] + esz[i] - 1;
    if (efy[i] < g->y0) g->y0 = efy[i];
    if (efy[i] + esz[i] - 1 > g->y1) g->y1 = efy[i] + esz[i] - 1;
  }
  g_stat_far_groups += far_groups;
  if (n == 0)
    return 0;
  // sort far to near, remembering each group's id in g_group_map
  for (int i = 0; i < n; i++)
    groups[i].key = groups[i].key << 8 | i;
  qsort(groups, n, sizeof(Group), CompareGroups);

  for (int gi = 0; gi < n; gi++) {
    Group *g = &groups[gi];
    int id = (g->key & 0xff) + 1;
    float ax = (g->x0 + g->x1 + 1) * 0.5f, ay = (float)(g->y1 + 1);   // feet
    float sx, sy, sc;
    if (!Cam_ToScreen(cm, ax, ay, &sx, &sy, &sc))
      continue;
    // far away: same fog as the ground, gone at the fog end
    float depth = cm->D / sc;
    if (depth >= kFogEnd)
      continue;
    float inv = 1.0f / sc;
    int dx0 = (int)floorf(sx + (g->x0 - ax) * sc), dx1 = (int)ceilf(sx + (g->x1 + 1 - ax) * sc);
    int dy0 = (int)floorf(sy + (g->y0 - ay) * sc), dy1 = (int)ceilf(sy);
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > w) dx1 = w;
    if (dy1 > h) dy1 = h;
    for (int y = dy0; y < dy1; y++) {
      int vy = (int)floorf(ay + (y + 0.5f - sy) * inv);
      if (vy < g->y0 || vy > g->y1)
        continue;
      uint32 *dst = out + y * pitch_px;
      if (!g->far) {
        const uint8 *gm = g_group_map + vy * w;
        const uint32 *src = g_side + vy * w;
        for (int x = dx0; x < dx1; x++) {
          int vx = (int)floorf(ax + (x + 0.5f - sx) * inv);
          if (vx >= g->x0 && vx <= g->x1 && gm[vx] == id)
            dst[x] = src[vx];
        }
      } else {
        for (int x = dx0; x < dx1; x++) {
          int vx = (int)floorf(ax + (x + 0.5f - sx) * inv);
          if (vx < g->x0 || vx > g->x1)
            continue;
          for (int e = 0; e < g->n_ents; e++) {
            int i = g->ents[e], lx = vx - efx[i], ly = vy - efy[i];
            uint32 c;
            if ((unsigned)lx < (unsigned)esz[i] && (unsigned)ly < (unsigned)esz[i] &&
                OamPixel(ppu, i, lx, ly, esz[i], &c)) {
              dst[x] = c;
              break;
            }
          }
        }
      }
    }
  }
  return n;
}

static void DrawOverlay(uint32 *out, int pitch_px) {
  // BG3 (HUD, dialogue boxes, menus) stays flat where the 2D game put it
  for (int y = 0; y < g_h; y++) {
    const uint32 *src = g_side + y * g_w;
    uint32 *dst = out + y * pitch_px;
    for (int x = 0; x < g_w; x++)
      if ((src[x] >> 24 & 0xf) == 2)
        dst[x] = src[x];
  }
}

void Hd3d_EndFrame(uint8 *pixels, int pitch, int width, int height) {
  if (!g_active_last || width != g_w || height != g_h)
    return;
  uint64 t0 = SDL_GetPerformanceCounter();
  int pitch_px = pitch / 4;
  for (int y = 0; y < height; y++)
    memcpy(g_floor + y * width, pixels + y * pitch, width * 4);

  Cam cm;
  Cam_Setup(&cm, width, height);
  Ground gr;
  Ground_Setup(&gr, g_zenv.ppu, width);
  bool outdoors = main_module_index == 9 || (main_module_index == 14 && saved_module_for_menu == 9);
  Sky sk;
  Sky_Setup(&sk, outdoors, width);
  uint32 *out = (uint32 *)pixels;
  DrawGround(out, pitch_px, &cm, &gr, &sk);
  int groups = DrawSprites(out, pitch_px, &cm, g_zenv.ppu);
  if (g_weather) {
    // the game's weather layer, flat over ground and sky (not sprites). With
    // the HD rain on it only darkens (its drops would be a second rain)
    HdWorld_SuppressOverlay(false);
    HdWorld_OverlayFlat(pixels, pitch, width, height, g_fx_rain && HdFx_IsRainingOutdoors());
  }
  HdFx_FlatWeather(pixels, pitch, width, height);   // HD rain drops and lightning, flat
  DrawOverlay(out, pitch_px);

  g_stat_ticks += SDL_GetPerformanceCounter() - t0;
  g_stat_groups += groups;
  if (groups > g_stat_max_groups)
    g_stat_max_groups = groups;
  if (++g_stat_frames == 300) {
    double ms = g_stat_ticks * 1000.0 / SDL_GetPerformanceFrequency() / g_stat_frames;
    Hd3d_Log("stats: %d frames, 3D compose %.3f ms/frame, sprite groups avg %.1f max %d, tilt %.1f, curve R=%.0f, "
             "past-screen px/frame %d (missing %d), source %s, far sprites avg %.1f (draw distance %s, %s), "
             "frame %dx%d, module=%d area=0x%02x",
             g_stat_frames, ms, (double)g_stat_groups / g_stat_frames, g_stat_max_groups, g_angle, cm.R,
             g_stat_outside / g_stat_frames, g_stat_missing / g_stat_frames, kSourceNames[gr.source],
             (double)g_stat_far_groups / g_stat_frames, kDistNames[g_view3d_dist],
             HdWorld_Active() ? "active" : "needs continuous world outdoors",
             width, height, main_module_index, BYTE(overworld_screen_index));
    g_stat_ticks = 0, g_stat_frames = 0, g_stat_groups = 0, g_stat_max_groups = 0;
    g_stat_outside = 0, g_stat_missing = 0, g_stat_far_groups = 0;
  }
}
