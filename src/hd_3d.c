// 3D view, step 1: the world tilted in perspective.
//
// How a 3D frame is made (all in software, so it works with every output
// method and shader):
// - the PPU draws the frame WITHOUT sprites and BG3 (skipLayers). That is the
//   ground, and every HD effect (water, light, shadows, continuous world)
//   still runs on it as usual;
// - after each scanline the same line is drawn again with everything into a
//   side buffer (ppu_redrawLine), remembering which OAM entry owns each sprite
//   pixel (ppu->objOwnerLine);
// - at the end of the frame the ground is re-projected onto a tilted plane,
//   the sprites are grouped by owner (Link, each enemy, each item) and stood
//   up as flat cutouts at their feet, scaled with distance, and BG3 (HUD,
//   dialogue boxes) is put back flat on the screen.
//
// Limits of this step: the ground ends where the SNES screen ends (beyond it
// there is sky outdoors, black indoors), and nothing has height yet.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <SDL.h>
#include "hd_3d.h"
#include "snes/ppu.h"
#include "variables.h"
#include "config.h"

extern uint8 g_link_oam_mask[128];      // player_oam.c
extern uint8 g_sprite_oam_owner[128];   // sprite.c

int g_view3d;

static const float kLevelDegrees[kHd3d_NumLevels] = { 0.0f, 20.0f, 35.0f, 50.0f };
static const char *const kLevelNames[kHd3d_NumLevels] = { "Off", "20", "35", "50" };

// Distance of the camera from the ground, in SNES pixels. Also the focal
// length, so with no tilt the picture is exactly the 2D one.
static const float kCamDist = 320.0f;

static FILE *g_log;
static float g_angle;            // current tilt in degrees (eases toward the target)
static bool g_active_last;
static int g_last_reason = -1;

// frame buffers (size of the frame being drawn)
static int g_w, g_h;
static uint32 *g_side;           // full frame, sprites and BG3 included
static uint8 *g_owner;           // OAM index per pixel of g_side, 0xff = no sprite
static uint32 *g_floor;          // copy of the ground before re-projection
static uint8 *g_group_map;       // sprite group + 1 per pixel, 0 = none

// stats for the log
static uint64 g_stat_ticks;
static int g_stat_frames, g_stat_groups, g_stat_max_groups;

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

void Hd3d_Init(void) {
  if (!g_log)
    g_log = fopen("zelda3_3d.log", "w");
  g_view3d = g_config.view3d % kHd3d_NumLevels;
  g_angle = kLevelDegrees[g_view3d];
  Hd3d_Log("3D view init: View3D=%d (%s deg), NewRenderer=%d, OutputMethod=%d, ExtendedAspectRatio extra=%d, shader='%s'",
           g_view3d, kLevelNames[g_view3d], g_config.new_renderer, g_config.output_method,
           g_config.extended_aspect_ratio, g_config.shader ? g_config.shader : "");
  if (!g_config.new_renderer)
    Hd3d_Log("WARNING: NewRenderer is off, the 3D view needs it (zelda3.ini [Graphics] NewRenderer = 1)");
}

const char *Hd3d_LevelName(int level) {
  return kLevelNames[(unsigned)level % kHd3d_NumLevels];
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
  if (reason != kReason_On || !EnsureBuffers(width, height)) {
    g_active_last = false;
    return false;
  }
  memset(g_owner, 0xff, width * height);
  ppu->skipLayers = 4 | 16;   // ground only: no BG3, no sprites
  g_active_last = true;
  return true;
}

void Hd3d_CaptureLine(Ppu *ppu, int line) {
  if (line < 1 || line > g_h)
    return;
  uint32 *row = g_side + (line - 1) * g_w;
  ppu_redrawLine(ppu, line, (uint8 *)row, g_w * 4, 0);
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
// Projection
//
// Ground coordinates (u, v) are pixels of the 2D frame. The camera looks at
// the ground point (cx, p) from distance D, tilted by `th` toward the bottom
// of the screen. For a screen pixel (cx + dx, cy + dy):
//   t = D cos(th) / (D cos(th) + dy sin(th))      (t < 1 = magnified)
//   u = cx + t dx
//   v = p + D sin(th) + t (dy cos(th) - D sin(th))
// p is chosen so the bottom edge of the ground sits on the bottom of the screen.

typedef struct Proj {
  float D, s, c, cx, cy, p;
} Proj;

static void Proj_Setup(Proj *pr, int w, int h) {
  float th = g_angle * (3.14159265f / 180.0f);
  pr->D = kCamDist, pr->s = sinf(th), pr->c = cosf(th);
  pr->cx = w * 0.5f, pr->cy = h * 0.5f;
  float dy = (h - 0.5f) - pr->cy;
  float t = pr->D * pr->c / (pr->D * pr->c + dy * pr->s);
  pr->p = h - (pr->D * pr->s + t * (dy * pr->c - pr->D * pr->s));
}

// ground point -> screen point and scale (screen pixels per ground pixel)
static bool Proj_ToScreen(const Proj *pr, float u, float v, float *sx, float *sy, float *scale) {
  float w = v - pr->p - pr->D * pr->s;
  float den = pr->D * pr->c * pr->c - w * pr->s;
  if (den <= 1e-3f)
    return false;
  float dy = pr->D * pr->c * (pr->D * pr->s + w) / den;
  float tden = pr->D * pr->c + dy * pr->s;
  if (tden <= 1e-3f)
    return false;
  float t = pr->D * pr->c / tden;
  *sx = pr->cx + (u - pr->cx) / t;
  *sy = pr->cy + dy;
  *scale = 1.0f / t;
  return true;
}

static inline uint32 Lerp(uint32 a, uint32 b, int f /* 0-256 */) {
  uint32 rb = ((a & 0xff00ff) * (256 - f) + (b & 0xff00ff) * f) >> 8 & 0xff00ff;
  uint32 g = ((a & 0x00ff00) * (256 - f) + (b & 0x00ff00) * f) >> 8 & 0x00ff00;
  return rb | g;
}

static uint32 SkyColor(int y, int h, bool outdoors) {
  if (!outdoors)
    return 5u << 24;
  // top 0x5a8fd8 -> horizon 0xbcd8f0, tagged as backdrop (layer 5)
  int f = h > 1 ? y * 256 / (h - 1) : 0;
  return 5u << 24 | Lerp(0x5a8fd8, 0xbcd8f0, f);
}

static void DrawGround(uint32 *out, int pitch_px, const Proj *pr, bool outdoors) {
  int w = g_w, h = g_h;
  const int kHaze = 20;  // ground rows that fade into the sky at the far edge
  for (int y = 0; y < h; y++) {
    uint32 *dst = out + y * pitch_px;
    uint32 sky = SkyColor(y, h, outdoors);
    float dy = (y + 0.5f) - pr->cy;
    float tden = pr->D * pr->c + dy * pr->s;
    if (tden <= 1e-3f) {
      for (int x = 0; x < w; x++) dst[x] = sky;
      continue;
    }
    float t = pr->D * pr->c / tden;
    float v = pr->p + pr->D * pr->s + t * (dy * pr->c - pr->D * pr->s);
    int vi = (int)floorf(v);
    if (vi < 0 || vi >= h) {
      for (int x = 0; x < w; x++) dst[x] = sky;
      continue;
    }
    int haze = vi < kHaze ? (kHaze - vi) * 256 / kHaze : 0;
    const uint32 *src = g_floor + vi * w;
    // u = cx + t (x + 0.5 - cx), stepped in 16.16 fixed point
    int32 u = (int32)((pr->cx + t * (0.5f - pr->cx)) * 65536.0f);
    int32 du = (int32)(t * 65536.0f);
    for (int x = 0; x < w; x++, u += du) {
      int ui = u >> 16;
      if (u < 0 || ui >= w) {
        dst[x] = sky;
      } else {
        uint32 px = src[ui];
        dst[x] = haze ? (px & 0xff000000) | Lerp(px, sky, haze) : px;
      }
    }
  }
}

typedef struct Group {
  int key;
  int x0, y0, x1, y1;
  float sx, sy, scale;
} Group;

enum { kMaxGroups = 64 };

static int GroupKey(int oam) {
  if (g_link_oam_mask[oam])
    return 0;                              // Link (body, shield, sword, shadow)
  if (g_sprite_oam_owner[oam] < 16)
    return 1 + g_sprite_oam_owner[oam];    // an enemy / NPC slot
  return 32 + oam;                         // anything else: each OAM entry alone
}

static int CompareGroups(const void *a, const void *b) {
  const Group *ga = (const Group *)a, *gb = (const Group *)b;
  return ga->y1 - gb->y1;   // smaller y1 = farther away, drawn first
}

static int DrawSprites(uint32 *out, int pitch_px, const Proj *pr) {
  int w = g_w, h = g_h;
  Group groups[kMaxGroups];
  int16 key_to_group[32 + 128];
  memset(key_to_group, 0xff, sizeof(key_to_group));
  int n = 0;
  memset(g_group_map, 0, w * h);
  for (int i = 0; i < w * h; i++) {
    uint8 o = g_owner[i];
    if (o >= 128)
      continue;
    int key = GroupKey(o);
    int gi = key_to_group[key];
    if (gi < 0) {
      if (n == kMaxGroups)
        continue;
      gi = key_to_group[key] = n++;
      groups[gi].key = key;
      groups[gi].x0 = groups[gi].y0 = 1 << 20;
      groups[gi].x1 = groups[gi].y1 = -1;
    }
    Group *g = &groups[gi];
    int x = i % w, y = i / w;
    if (x < g->x0) g->x0 = x;
    if (x > g->x1) g->x1 = x;
    if (y < g->y0) g->y0 = y;
    if (y > g->y1) g->y1 = y;
    g_group_map[i] = (uint8)(gi + 1);
  }
  if (n == 0)
    return 0;
  // sort far to near, keeping the group ids in g_group_map valid
  int order_id[kMaxGroups];
  for (int i = 0; i < n; i++)
    order_id[i] = i, groups[i].key = groups[i].key << 8 | i;
  qsort(groups, n, sizeof(Group), CompareGroups);
  for (int i = 0; i < n; i++)
    order_id[i] = groups[i].key & 0xff;

  for (int gi = 0; gi < n; gi++) {
    Group *g = &groups[gi];
    int id = order_id[gi] + 1;
    float ax = (g->x0 + g->x1 + 1) * 0.5f, ay = (float)(g->y1 + 1);   // feet
    float sx, sy, sc;
    if (!Proj_ToScreen(pr, ax, ay, &sx, &sy, &sc))
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
      const uint8 *gm = g_group_map + vy * w;
      const uint32 *src = g_side + vy * w;
      uint32 *dst = out + y * pitch_px;
      for (int x = dx0; x < dx1; x++) {
        int vx = (int)floorf(ax + (x + 0.5f - sx) * inv);
        if (vx >= g->x0 && vx <= g->x1 && gm[vx] == id)
          dst[x] = src[vx];
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

  Proj pr;
  Proj_Setup(&pr, width, height);
  bool outdoors = main_module_index == 9 || (main_module_index == 14 && saved_module_for_menu == 9);
  uint32 *out = (uint32 *)pixels;
  DrawGround(out, pitch_px, &pr, outdoors);
  int groups = DrawSprites(out, pitch_px, &pr);
  DrawOverlay(out, pitch_px);

  g_stat_ticks += SDL_GetPerformanceCounter() - t0;
  g_stat_groups += groups;
  if (groups > g_stat_max_groups)
    g_stat_max_groups = groups;
  if (++g_stat_frames == 300) {
    double ms = g_stat_ticks * 1000.0 / SDL_GetPerformanceFrequency() / g_stat_frames;
    Hd3d_Log("stats: %d frames, 3D compose %.3f ms/frame, sprite groups avg %.1f max %d, tilt %.1f, frame %dx%d, module=%d area=0x%02x",
             g_stat_frames, ms, (double)g_stat_groups / g_stat_frames, g_stat_max_groups, g_angle,
             width, height, main_module_index, BYTE(overworld_screen_index));
    g_stat_ticks = 0, g_stat_frames = 0, g_stat_groups = 0, g_stat_max_groups = 0;
  }
}
