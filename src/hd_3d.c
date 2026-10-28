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
#include "hd_light.h"
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
int g_view3d_walls;
int g_view3d_cam;

static const char *const kDistNames[kHd3d_NumDists] = { "Screen", "2x", "3x" };
static const char *const kCamNames[kHd3d_NumCams] = { "Classic", "Third person" };

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

// Third person camera: distance from its aim point, how far ahead of Link
// and how high it aims (pixels), pitch range (degrees from straight down)
// and how fast the right stick turns it
static const float kThirdDist = 150.0f, kThirdAhead = 20.0f, kThirdAimZ = 14.0f;
static const float kThirdPitchMin = 35.0f, kThirdPitchMax = 80.0f, kThirdPitchDefault = 62.0f;
static const float kThirdYawSpeed = 2.6f;      // radians a second at full stick
static const float kThirdPitchSpeed = 55.0f;   // degrees a second at full stick
static const int kStickDeadzone = 8000;
static float g_yaw;              // third person: look direction, 0 = north, + = clockwise
static float g_yaw_target;
static bool g_recenter;          // easing g_yaw toward g_yaw_target (behind Link)
static float g_pitch3 = 62.0f;
static int g_stick_x, g_stick_y; // right stick
static uint32 g_cam_ticks;

// 9 = overworld, 11 = special overworld (Master Sword grove, Zora's
// waterfall, areas 0x80 and up)
static bool IsOverworldModule(int mod) {
  return mod == 9 || mod == 11;
}

// walking around (no menu, no transition): indoors or out
static bool IsWalkingModule(int mod) {
  return mod == 7 || IsOverworldModule(mod);
}

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
static float *g_depth;           // camera depth per screen pixel (indoor walls)
static uint32 *g_uv;             // 2D frame pixel shown at each screen pixel (weather layer), kUvNone = sky
#define kUvNone 0x80008000u
// what each screen pixel shows, for the HD effects that ran on the 2D frame
// (light, forest fog): those only reached the ground inside the SNES screen
enum { kKind_Sky, kKind_Frame, kKind_Outside, kKind_Sprite };
static uint8 *g_kind;
#define PackUv(u, v) ((uint32)(uint16)(int16)(u) | (uint32)(uint16)(int16)(v) << 16)

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
  if (!g_log) {
    // keep the previous run's log: a dump taken before restarting the game
    // is not lost
    remove("zelda3_3d.prev.log");
    rename("zelda3_3d.log", "zelda3_3d.prev.log");
    g_log = fopen("zelda3_3d.log", "w");
  }
  g_view3d = g_config.view3d % kHd3d_NumLevels;
  g_view3d_curve = g_config.view3d_curve % kHd3d_NumCurves;
  g_view3d_dist = g_config.view3d_dist % kHd3d_NumDists;
  g_view3d_walls = g_config.view3d_walls & 1;
  g_view3d_cam = g_config.view3d_cam % kHd3d_NumCams;
  g_angle = kLevelDegrees[g_view3d];
  g_radius_inv = RadiusInv(g_view3d_curve);
  g_pitch3 = kThirdPitchDefault;
  Hd3d_Log("3D view init: View3D=%d (%s deg), View3DCurve=%d (%s), View3DDrawDistance=%d (%s), View3DWalls=%d, "
           "View3DCamera=%d (%s), NewRenderer=%d, OutputMethod=%d, ExtendedAspectRatio=%d, ContinuousWorld=%d, shader='%s'",
           g_view3d, kLevelNames[g_view3d], g_view3d_curve, kCurveNames[g_view3d_curve],
           g_view3d_dist, kDistNames[g_view3d_dist], g_view3d_walls, g_view3d_cam, kCamNames[g_view3d_cam],
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

void Hd3d_SetWalls(int on, const char *why) {
  on &= 1;
  if (on != g_view3d_walls)
    Hd3d_Log("indoor walls -> %s (%s)", on ? "on" : "off", why);
  g_view3d_walls = on;
  g_config.view3d_walls = (uint8)on;
}

void Hd3d_ToggleWalls(void) {
  Hd3d_SetWalls(!g_view3d_walls, "hotkey");
}

const char *Hd3d_CameraName(int cam) {
  return kCamNames[(unsigned)cam % kHd3d_NumCams];
}

// Link's facing (0 up, 2 down, 4 left, 6 right) -> yaw that looks the same way
static float FacingYaw(void) {
  static const float kYaw[4] = { 0.0f, 3.14159265f, -1.5707963f, 1.5707963f };
  return kYaw[(link_direction_facing >> 1) & 3];
}

void Hd3d_SetCamera(int cam, const char *why) {
  cam = (unsigned)cam % kHd3d_NumCams;
  if (cam != g_view3d_cam)
    Hd3d_Log("camera -> %s (%s)", kCamNames[cam], why);
  if (cam == kHd3d_CamThird && g_view3d_cam != kHd3d_CamThird) {
    // start behind Link, at the default pitch
    g_yaw = g_yaw_target = FacingYaw();
    g_recenter = false;
    g_pitch3 = kThirdPitchDefault;
    if (g_view3d == 0)
      Hd3d_SetLevel(3, "third person camera needs the 3D view");
  }
  g_view3d_cam = cam;
  g_config.view3d_cam = (uint8)cam;
}

void Hd3d_ToggleCamera(void) {
  Hd3d_SetCamera(g_view3d_cam + 1, "hotkey");
}

bool Hd3d_ThirdPerson(void) {
  return g_active_last && g_view3d_cam == kHd3d_CamThird;
}

void Hd3d_RightStick(int axis_y, int value) {
  *(axis_y ? &g_stick_y : &g_stick_x) = value;
}

void Hd3d_Recenter(void) {
  if (g_view3d_cam != kHd3d_CamThird)
    return;
  g_yaw_target = FacingYaw();
  g_recenter = true;
}

static float WrapAngle(float a) {
  while (a > 3.14159265f) a -= 6.2831853f;
  while (a < -3.14159265f) a += 6.2831853f;
  return a;
}

// one stick axis past the deadzone -> -1..1
static float StickAmount(int v) {
  if (v > -kStickDeadzone && v < kStickDeadzone)
    return 0.0f;
  float f = (v > 0 ? v - kStickDeadzone : v + kStickDeadzone) / (32767.0f - kStickDeadzone);
  return f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
}

// right stick: X turns the camera around Link, Y raises and lowers it
static void UpdateThirdCamera(void) {
  uint32 now = SDL_GetTicks();
  float dt = g_cam_ticks ? (now - g_cam_ticks) * 0.001f : 0.0f;
  g_cam_ticks = now;
  if (dt > 0.1f) dt = 0.1f;
  float sx = StickAmount(g_stick_x), sy = StickAmount(g_stick_y);
  if (sx != 0.0f) {
    g_yaw = WrapAngle(g_yaw + sx * kThirdYawSpeed * dt);
    g_recenter = false;
  } else if (g_recenter) {
    float d = WrapAngle(g_yaw_target - g_yaw);
    g_yaw = WrapAngle(g_yaw + d * 0.2f);
    if (fabsf(d) < 0.01f)
      g_yaw = g_yaw_target, g_recenter = false;
  }
  // stick up = the camera goes down toward the horizon (looks further ahead)
  g_pitch3 -= sy * kThirdPitchSpeed * dt;
  if (g_pitch3 < kThirdPitchMin) g_pitch3 = kThirdPitchMin;
  if (g_pitch3 > kThirdPitchMax) g_pitch3 = kThirdPitchMax;
}

int Hd3d_ViewFacing(int facing) {
  if (!Hd3d_ThirdPerson() || !IsWalkingModule(main_module_index))
    return facing;
  static const float kYaw[4] = { 0.0f, 3.14159265f, -1.5707963f, 1.5707963f };
  // world direction minus the camera's, in quarter turns: 0 away from the
  // camera, 1 to its right, 2 toward it, 3 to its left
  float rel = WrapAngle(kYaw[(facing >> 1) & 3] - g_yaw);
  static const uint8 kQuarterToFacing[4] = { 0, 6, 2, 4 };
  return kQuarterToFacing[(int)floorf(rel * (2.0f / 3.14159265f) + 0.5f) & 3];
}

int Hd3d_RotateInput(int inputs) {
  // only while walking around: menus and the map keep the plain d-pad
  if (!Hd3d_ThirdPerson() || !IsWalkingModule(main_module_index))
    return inputs;
  int dirs = inputs & 0xf0;
  if (!dirs)
    return inputs;
  // up/down/left/right as seen on the screen -> the world direction under
  // the camera's yaw, snapped to the 8 directions Link can walk
  float side = ((dirs >> 7) & 1) - ((dirs >> 6) & 1);   // right - left
  float fwd = ((dirs >> 4) & 1) - ((dirs >> 5) & 1);    // up - down
  if (side == 0.0f && fwd == 0.0f)
    return inputs;
  float cs = cosf(g_yaw), sn = sinf(g_yaw);
  float wx = cs * side + sn * fwd;     // east
  float wn = -sn * side + cs * fwd;    // north
  static const uint8 kSegmentToButtons[8] = {
    1 << 4, 1 << 4 | 1 << 7, 1 << 7, 1 << 7 | 1 << 5, 1 << 5, 1 << 5 | 1 << 6, 1 << 6, 1 << 6 | 1 << 4,
  };
  int seg = (int)floorf(atan2f(wx, wn) * (4.0f / 3.14159265f) + 0.5f) & 7;
  return (inputs & ~0xf0) | kSegmentToButtons[seg];
}

bool Hd3d_FarSprites(int *margin_x, int *margin_top, int *margin_bottom) {
  // the 3D view was drawn last frame; outdoors only with the continuous world
  // (it keeps every enemy of the area alive), indoors in any room
  if (!g_active_last || g_view3d_dist == 0 || !(HdWorld_Active() || player_is_indoors))
    return false;
  *margin_x = 256 * g_view3d_dist;
  *margin_top = 256 * g_view3d_dist;   // far away = up the screen
  // the third person camera turns: enemies behind Link are seen too
  *margin_bottom = g_view3d_cam == kHd3d_CamThird ? 256 * g_view3d_dist : 16;
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
  // 7 dungeon/indoors, 9/11 overworld, 15/16 iris transitions, 17 falling into a hole
  if (IsWalkingModule(mod) || mod == 15 || mod == 16 || mod == 17)
    return true;
  // 14 = menus and dialogue over the game
  if (mod == 14)
    return IsWalkingModule(saved_module_for_menu);
  return false;
}

static bool EnsureBuffers(int w, int h) {
  if (w == g_w && h == g_h && g_side)
    return true;
  free(g_side), free(g_owner), free(g_floor), free(g_group_map), free(g_depth), free(g_uv), free(g_kind);
  g_side = (uint32 *)malloc(sizeof(uint32) * w * h);
  g_owner = (uint8 *)malloc(w * h);
  g_floor = (uint32 *)malloc(sizeof(uint32) * w * h);
  g_group_map = (uint8 *)malloc(w * h);
  g_depth = (float *)malloc(sizeof(float) * w * h);
  g_uv = (uint32 *)malloc(sizeof(uint32) * w * h);
  g_kind = (uint8 *)malloc(w * h);
  if (!g_side || !g_owner || !g_floor || !g_group_map || !g_depth || !g_uv || !g_kind) {
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
  if (g_view3d_cam == kHd3d_CamThird)
    UpdateThirdCamera();
  else
    g_cam_ticks = 0;

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
    // sprite pixels are layer 4 (palettes 4-7) or 6 (palettes 0-3: boomerang,
    // levers, lamp flame...), see SPRITE_PRIO_TO_PRIO in snes/ppu.c
    uint32 l = row[c] >> 24 & 0xf;
    if (l == 4 || l == 6) {
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

// Front pixel of BG1/BG2 on one screen (bit mask of enabled layers), by
// priority: BG1 high, BG2 high, BG1 low, BG2 low. Returns the layer (0, 1,
// 5 = backdrop) and the cgram index
static int TopLayer(Ppu *ppu, int enabled, int x, int y, int *index) {
  int idx[2] = { 0, 0 }, pr[2] = { 0, 0 };
  for (int l = 0; l < 2; l++)
    if (enabled & (1 << l))
      idx[l] = VramLayerPixel(ppu, l, x, y, &pr[l]);
  static const int kOrder[4][2] = { { 0, 1 }, { 1, 1 }, { 0, 0 }, { 1, 0 } };
  for (int k = 0; k < 4; k++) {
    int l = kOrder[k][0];
    if (idx[l] && pr[l] == kOrder[k][1]) {
      *index = idx[l];
      return l;
    }
  }
  *index = 0;
  return 5;
}

// BG1 and BG2 as the PPU shows them (BG3 and sprites excluded), with the
// color math: rooms that lay a translucent or dark BG1 over the floor (added
// or subtracted on the subscreen) showed that layer's black tiles instead
static uint32 SampleVram(Ppu *ppu, int fx, int fy) {
  int x = fx - ppu->extraLeftRight, y = fy + 1;   // frame row 0 = PPU line 1
  bool hd_dark = HdLight_DarkOverride();   // as rendered: no game darkness
  int mi, ml = TopLayer(ppu, ppu->screenEnabled[0], x, y, &mi);
  uint32 main = CgramToPixel(ppu, mi, (uint32)ml);
  if (!(ppu->mathEnabled & (1 << ml)))
    return main;
  uint32 sub;
  bool half = ppu->halfColor;
  if (ppu->addSubscreen) {
    int sub_enabled = ppu->screenEnabled[1] & (hd_dark ? ~1 : ~0);
    int si, sl = TopLayer(ppu, sub_enabled, x, y, &si);
    if (sl == 5) {
      // empty subscreen: the fixed color, never halved
      half = false;
      sub = hd_dark ? 0 : ppu->brightnessMult[ppu->fixedColorR & 0x1f] << 16 |
                          ppu->brightnessMult[ppu->fixedColorG & 0x1f] << 8 | ppu->brightnessMult[ppu->fixedColorB & 0x1f];
    } else {
      sub = CgramToPixel(ppu, si, 0) & 0xffffff;
    }
  } else {
    half = false;
    sub = hd_dark ? 0 : ppu->brightnessMult[ppu->fixedColorR & 0x1f] << 16 |
                        ppu->brightnessMult[ppu->fixedColorG & 0x1f] << 8 | ppu->brightnessMult[ppu->fixedColorB & 0x1f];
  }
  uint32 out = 0;
  for (int sh = 0; sh <= 16; sh += 8) {
    int a = main >> sh & 0xff, b = sub >> sh & 0xff;
    int c = ppu->subtractColor ? a - b : a + b;
    if (half) c >>= 1;
    c = c < 0 ? 0 : c > 255 ? 255 : c;
    out |= (uint32)c << sh;
  }
  return (main & 0xff000000) | out;
}

static void Ground_Setup(Ground *g, Ppu *ppu, int width) {
  g->ppu = ppu;
  HdWorld_ViewOrigin(width, &g->org_x, &g->org_y);
  if (player_is_indoors) {
    // indoors: the scroll this frame was really drawn with (the game's camera
    // copy can be a frame ahead, and the walls slid against their art)
    BgLayer *bg = &ppu->bgLayer[1];
    g->org_x = bg->hScroll + ppu->bgBiasX - ppu->extraLeftRight;
    g->org_y = bg->vScroll + ppu->bgBiasY + 1;
  }
  if (HdWorld_Active()) {
    g->source = kSource_World;
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

//
// The camera has a position O and three directions: forward f (where it
// looks), right r (flat on the ground) and up (the screen's up). Classic: it
// looks north (-v). Third person: it orbits Link, turned by the yaw (0 =
// looking north, + = clockwise seen from above) and tilted by its own pitch;
// the sphere then touches the ground under Link.

typedef struct Cam {
  float F, cx, cy;              // focal length, screen center
  float tx, ty;                 // touch point of the sphere (center of the world curve)
  float R;                      // 0 = flat
  float ox, oy, oz;             // camera position
  float fx, fy, fz;             // forward
  float rx, ry;                 // right (z = 0)
  float ux, uy, uz;             // screen up
} Cam;

// tilt from straight down (radians) and horizontal look direction (hx, hy)
static void Cam_Orient(Cam *cm, float th, float hx, float hy) {
  float s = sinf(th), c = cosf(th);
  cm->fx = hx * s, cm->fy = hy * s, cm->fz = -c;
  cm->rx = -hy, cm->ry = hx;
  cm->ux = hx * c, cm->uy = hy * c, cm->uz = s;
}

static void Cam_Setup(Cam *cm, int w, int h) {
  float th = g_angle * (3.14159265f / 180.0f);
  float s = sinf(th), c = cosf(th);
  cm->F = kCamDist;
  cm->cx = w * 0.5f, cm->cy = h * 0.5f;
  Cam_Orient(cm, th, 0.0f, -1.0f);
  // p: the ground row the camera looks at, so the bottom edge of the ground
  // sits on the bottom of the screen
  float dy = (h - 0.5f) - cm->cy;
  float t = cm->F * c / (cm->F * c + dy * s);
  float p = h - (cm->F * s + t * (dy * c - cm->F * s));
  cm->R = g_radius_inv > 1e-6f ? 1.0f / g_radius_inv : 0.0f;
  cm->tx = cm->cx, cm->ty = p;
  cm->ox = cm->cx, cm->oy = p + cm->F * s, cm->oz = cm->F * c;
}

// Third person: behind Link at (lx, ly) on the ground (frame coordinates)
static void Cam_SetupThird(Cam *cm, int w, int h, float lx, float ly) {
  float th = g_pitch3 * (3.14159265f / 180.0f);
  float hx = sinf(g_yaw), hy = -cosf(g_yaw);
  cm->F = kCamDist;
  cm->cx = w * 0.5f, cm->cy = h * 0.5f;
  Cam_Orient(cm, th, hx, hy);
  cm->R = g_radius_inv > 1e-6f ? 1.0f / g_radius_inv : 0.0f;
  cm->tx = lx, cm->ty = ly;
  // aim a little ahead of Link and at his chest, so he stands in the lower
  // half of the screen with the way ahead open
  float ax = lx + hx * kThirdAhead, ay = ly + hy * kThirdAhead, az = kThirdAimZ;
  cm->ox = ax - cm->fx * kThirdDist, cm->oy = ay - cm->fy * kThirdDist, cm->oz = az - cm->fz * kThirdDist;
}

// direction of the ray through screen pixel (sx, sy)
static inline void Cam_Dir(const Cam *cm, float sx, float sy, float *rx, float *ry, float *rz) {
  float dx = sx - cm->cx, dy = sy - cm->cy;
  *rx = cm->F * cm->fx + dx * cm->rx - dy * cm->ux;
  *ry = cm->F * cm->fy + dx * cm->ry - dy * cm->uy;
  *rz = cm->F * cm->fz - dy * cm->uz;
}

// screen pixel center -> ground (u, v) and camera depth; false = sky
static bool Cam_Ray(const Cam *cm, float sx, float sy, float *u, float *v, float *depth) {
  float rx, ry, rz;
  Cam_Dir(cm, sx, sy, &rx, &ry, &rz);
  float lam;
  if (cm->R == 0.0f) {
    if (rz >= -1e-4f)
      return false;
    lam = -cm->oz / rz;
    *u = cm->ox + lam * rx;
    *v = cm->oy + lam * ry;
  } else {
    // O - sphere center (the center is R below the touch point)
    float qx = cm->ox - cm->tx, qy = cm->oy - cm->ty, qz = cm->oz + cm->R;
    float a = rx * rx + ry * ry + rz * rz;
    float b = qx * rx + qy * ry + qz * rz;
    float cc = qx * qx + qy * qy + qz * qz - cm->R * cm->R;
    float disc = b * b - a * cc;
    if (disc < 0)
      return false;
    lam = (-b - sqrtf(disc)) / a;
    if (lam <= 0)
      return false;
    float hx = qx + lam * rx, hy = qy + lam * ry;   // relative to the touch point
    float r = sqrtf(hx * hx + hy * hy);
    float k = 1.0f;
    if (r > 1e-3f) {
      float q = r / cm->R;
      k = cm->R * asinf(q > 1.0f ? 1.0f : q) / r;
    }
    *u = cm->tx + hx * k;
    *v = cm->ty + hy * k;
  }
  *depth = lam * cm->F;
  return true;
}

// ground (u, v) -> screen point and scale (screen pixels per ground pixel)
// ground (u, v) lifted by `height` pixels (along the sphere's normal when
// curved) -> screen point and scale
static bool Cam_ToScreenH(const Cam *cm, float u, float v, float height, float *sx, float *sy, float *scale) {
  float x = u, y = v, z = height;
  if (cm->R != 0.0f) {
    float hx = u - cm->tx, hy = v - cm->ty;
    float r = sqrtf(hx * hx + hy * hy);
    if (r > 1e-3f) {
      float phi = r / cm->R;
      if (phi > 1.5f)
        return false;
      float k = (cm->R + height) * sinf(phi) / r;
      x = cm->tx + hx * k, y = cm->ty + hy * k;
      z = (cm->R + height) * cosf(phi) - cm->R;
    }
  }
  float rx = x - cm->ox, ry = y - cm->oy, rz = z - cm->oz;
  float zc = cm->fx * rx + cm->fy * ry + cm->fz * rz;   // along the view direction
  if (zc < 1.0f)
    return false;
  float right = cm->rx * rx + cm->ry * ry;
  float up = cm->ux * rx + cm->uy * ry + cm->uz * rz;
  *sx = cm->cx + cm->F * right / zc;
  *sy = cm->cy - cm->F * up / zc;
  *scale = cm->F / zc;
  return true;
}

static bool Cam_ToScreen(const Cam *cm, float u, float v, float *sx, float *sy, float *scale) {
  return Cam_ToScreenH(cm, u, v, 0.0f, sx, sy, scale);
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
  float rx, ry, rz;
  Cam_Dir(cm, sx, sy, &rx, &ry, &rz);
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
    uint32 *uv = g_uv + y * w;
    uint8 *kind = g_kind + y * w;
    for (int x = 0; x < w; x++) {
      float u, v, depth;
      if (!Cam_Ray(cm, x + 0.5f, y + 0.5f, &u, &v, &depth) || depth >= kFogEnd) {
        dst[x] = Sky_Color(sk, cm, x + 0.5f, y + 0.5f, x, y);
        uv[x] = kUvNone, kind[x] = kKind_Sky;
        continue;
      }
      int ui = (int)floorf(u), vi = (int)floorf(v);
      uv[x] = PackUv(ui, vi);
      uint32 px;
      if (ui >= 0 && ui < w && vi >= 0 && vi < h) {
        px = g_floor[vi * w + ui];
        kind[x] = kKind_Frame;
      } else {
        g_stat_outside++;
        if (!Ground_Sample(gr, ui, vi, &px)) {
          g_stat_missing++;
          dst[x] = fog;
          kind[x] = kKind_Sky;
          continue;
        }
        kind[x] = kKind_Outside;
      }
      if (depth > kFogStart) {
        int f = (int)((depth - kFogStart) * (256.0f / (kFogEnd - kFogStart)));
        px = (px & 0xff000000) | Lerp(px, fog, f > 256 ? 256 : f);
      }
      dst[x] = px;
    }
  }
}

// ---------------------------------------------------------------------------
// Indoor walls
//
// Every 8x8 tile of a room has a collision type in the game (the attribute
// tables, one per floor layer); types 0x01-0x03 are walls. Walls are raised:
// each north-south run of wall tiles gets the height of the run (capped), so
// a room's north wall stands up as tall as it is painted.
//
// The SNES draws walls in 3/4 view: a wall's face is painted on the ground
// just north of its base. So a wall surface point at height z shows the art
// z pixels further north ("fold-up"): the painted face stands upright and the
// floor behind it stays where it is.
//
// Drawing is column by column, front to back (like a heightmap "voxel space"
// renderer): walk up a screen column, find the ground under each row, and
// when that ground is a wall, fill the rows its face and top cover above what
// is already drawn. A depth buffer keeps characters behind walls hidden.

enum { kWallMax = 48 };          // tallest wall, pixels
static uint8 g_hmap[64 * 64];    // height per 8x8 tile of the room
static uint8 g_hfold[64 * 64];   // 1 = the surface shows the art z pixels north (fold-up)
static bool g_depth_valid;
static int g_stat_wall_cells;

static bool IsWallAttr(uint8 a) {
  return a >= 0x01 && a <= 0x03;
}

// --- which room object drew each tile (recorded while the room is built) ---

enum {
  kCls_Floor,
  kCls_Wall,       // walls of the room (raised only where solid)
  kCls_Ceiling,    // the dark void above walls
  kCls_Door,       // door frames (raised only where solid)
  kCls_Rail,       // rails, low walls
  kCls_Furniture,  // beds, tables, chairs, pews, forge
  kCls_Small,      // pots, chests, chest pedestals
  kCls_Block,      // blocks, pegs, big chests
  kCls_Tall,       // statues, columns
  kCls_Torch,
  kCls_Count,
};
static const char kClsLetter[kCls_Count] = { '.', 'W', 'C', 'D', 'r', 'f', 'o', 'b', 'T', 't' };
static const char *const kClsName[kCls_Count] = {
  "floor", "wall", "ceiling", "door", "rail", "furniture", "small", "block", "tall", "torch",
};
static const uint8 kClsHeight[kCls_Count] = { 0, kWallMax, kWallMax, kWallMax, 8, 12, 10, 16, 28, 14 };

// Object ids and names from the comments in dungeon.c (LoadType1ObjectSubtype1/2/3)
static int ObjectClass(int code) {
  int id = code & 0xff;
  switch (code & 0xf00) {
  case kHd3d_ObjSubtype1:
    if (id == 0x00 || id == 0x60 || (id >= 0xa0 && id <= 0xac && id != 0xa4) || id == 0xc0 || id == 0xc2)
      return kCls_Ceiling;
    if ((id >= 0x01 && id <= 0x20) || id == 0x53 || id == 0xb6 || id == 0xb7 || id == 0x36 || id == 0x37 ||
        id == 0x3a || id == 0x3b || id == 0x3e || id == 0x4b || id == 0x51 || id == 0x52 || id == 0x55 ||
        id == 0x56 || id == 0x61 || id == 0x62 || id == 0x90 || id == 0x91 || (id >= 0x64 && id <= 0x68) ||
        id == 0x73 || id == 0x74 || id == 0x76 || id == 0x77 || id == 0x78 || id == 0x7b ||
        (id >= 0x7f && id <= 0x86) || id == 0xcd || id == 0xce)
      return kCls_Wall;
    if (id == 0x22 || id == 0x2f || id == 0x30 || id == 0x5f || id == 0x69 || id == 0x6c || id == 0x6d ||
        id == 0x88 || id == 0x8a)
      return kCls_Rail;
    if (id == 0x3c || id == 0xdd)
      return kCls_Furniture;
    if (id == 0x38 || id == 0x39 || id == 0x3d || id == 0x75 || id == 0x87)
      return kCls_Tall;
    if (id == 0x89 || id == 0xb8 || id == 0xb9 || id == 0x92 || id == 0x93 || id == 0x96 || id == 0xbd || id == 0xde)
      return kCls_Block;
    if (id == 0x95 || id == 0xbc || id == 0xc1 || id == 0xdc)
      return kCls_Small;
    return kCls_Floor;
  case kHd3d_ObjSubtype2:
    if ((id >= 0x04 && id <= 0x1b) || id == 0x2a || id == 0x3c)
      return kCls_Wall;
    if (id == 0x1d || id == 0x21 || id == 0x26)
      return kCls_Tall;
    if (id == 0x22 || id == 0x28 || id == 0x23 || id == 0x3e)
      return kCls_Furniture;
    if (id == 0x20)
      return kCls_Torch;
    return kCls_Floor;
  case kHd3d_ObjSubtype3:
    if (id <= 0x02 || id == 0x0d || id == 0x17 || id == 0x18 || id == 0x69 || id == 0x6a || id == 0x6e || id == 0x6f)
      return kCls_Wall;
    if (id == 0x19 || id == 0x2f || id == 0x5a)
      return kCls_Small;
    if (id == 0x2c || id == 0x31 || id == 0x32 || id == 0x16)
      return kCls_Block;
    if (id == 0x4d || id == 0x5d || id == 0x2d)
      return kCls_Furniture;
    return kCls_Floor;
  case kHd3d_ObjDoor:
    return kCls_Door;
  }
  if (code == kHd3d_ObjPushBlock) return kCls_Block;
  if (code == kHd3d_ObjTorch) return kCls_Torch;
  return kCls_Floor;
}

typedef struct RoomObj {
  uint16 code;
  int8 x, y;
  uint8 pass, cls;
  uint16 tiles;      // tiles it drew (both layers)
} RoomObj;
enum { kMaxRoomObjs = 256 };
static RoomObj g_room_objs[kMaxRoomObjs];
static int g_n_room_objs;
static uint16 g_tile_obj[2][64 * 64];   // per layer (0 = BG2, 1 = BG1): room object index + 1, 0 = none
static uint16 g_snap[2][64 * 64];
static int g_room_id = -1, g_room_pass;
static bool g_room_building, g_room_dump_request;

void Hd3d_RoomBuildBegin(int room) {
  g_room_building = true;
  g_room_id = room;
  g_n_room_objs = 0;
  g_room_pass = 0;
  memset(g_tile_obj, 0, sizeof(g_tile_obj));
}

void Hd3d_RoomPass(int pass) {
  g_room_pass = pass;
}

void Hd3d_ObjBegin(void) {
  if (!g_room_building)
    return;
  memcpy(g_snap[0], dung_bg2, sizeof(g_snap[0]));
  memcpy(g_snap[1], dung_bg1, sizeof(g_snap[1]));
}

void Hd3d_ObjEnd(int code, int x, int y) {
  if (!g_room_building || g_n_room_objs == kMaxRoomObjs)
    return;
  int idx = g_n_room_objs++;
  RoomObj *o = &g_room_objs[idx];
  o->code = (uint16)code, o->x = (int8)x, o->y = (int8)y, o->pass = (uint8)g_room_pass;
  o->cls = (uint8)ObjectClass(code);
  int tiles = 0;
  for (int i = 0; i < 64 * 64; i++) {
    if (dung_bg2[i] != g_snap[0][i])
      g_tile_obj[0][i] = (uint16)(idx + 1), tiles++;
    if (dung_bg1[i] != g_snap[1][i])
      g_tile_obj[1][i] = (uint16)(idx + 1), tiles++;
  }
  o->tiles = (uint16)tiles;
}

void Hd3d_RoomBuildEnd(void) {
  g_room_building = false;
  int counts[kCls_Count] = { 0 };
  for (int i = 0; i < g_n_room_objs; i++)
    counts[g_room_objs[i].cls]++;
  Hd3d_Log("room %d built: %d objects (walls %d, ceiling %d, doors %d, rails %d, furniture %d, small %d, blocks %d, "
           "tall %d, torches %d)", g_room_id, g_n_room_objs, counts[kCls_Wall], counts[kCls_Ceiling],
           counts[kCls_Door], counts[kCls_Rail], counts[kCls_Furniture], counts[kCls_Small], counts[kCls_Block],
           counts[kCls_Tall], counts[kCls_Torch]);
}

void Hd3d_RequestRoomDump(void) {
  g_room_dump_request = true;
  Hd3d_Log("room dump requested (key 8)");
}

static int CellClass(int cell) {
  int best = kCls_Floor;
  for (int l = 0; l < 2; l++) {
    int o = g_tile_obj[l][cell];
    if (o && kClsHeight[g_room_objs[o - 1].cls] > kClsHeight[best])
      best = g_room_objs[o - 1].cls;
  }
  return best;
}

static bool IsWallClass(int c) {
  return c == kCls_Wall || c == kCls_Ceiling || c == kCls_Door;
}

static void RoomDump(const uint8 *attr, const uint8 *cls) {
  Hd3d_Log("=== room dump: room %d (built for %d), layer %s, %d objects", dungeon_room_index, g_room_id,
           link_is_on_lower_level ? "lower" : "upper", g_n_room_objs);
  for (int i = 0; i < g_n_room_objs; i++) {
    const RoomObj *o = &g_room_objs[i];
    const char *kind = (o->code & 0xf00) == kHd3d_ObjSubtype1 ? "T1" : (o->code & 0xf00) == kHd3d_ObjSubtype2 ? "T2" :
                       (o->code & 0xf00) == kHd3d_ObjSubtype3 ? "T3" : (o->code & 0xf00) == kHd3d_ObjDoor ? "door" :
                       o->code == kHd3d_ObjPushBlock ? "pushblock" : "torch";
    Hd3d_Log("  obj %3d pass %d %s 0x%02x at (%d,%d) tiles %d -> %s", i, o->pass, kind, o->code & 0xff, o->x, o->y,
             o->tiles, kClsName[o->cls]);
  }
  Hd3d_Log("  class map, then heights (64x64 tiles of 8 px; . floor W wall C ceiling D door r rail f furniture "
           "o small b block T tall t torch; w = wall tile that is walkable for the game; heights in 8 px):");
  char line[3 * 64 + 8];
  for (int y = 0; y < 64; y++) {
    int n = 0;
    for (int x = 0; x < 64; x++) {
      int c = y * 64 + x;
      char ch = kClsLetter[cls[c]];
      if (IsWallClass(cls[c]) && !IsWallAttr(attr[c]))
        ch = 'w';
      line[n++] = ch;
    }
    line[n++] = ' ', line[n++] = '|', line[n++] = ' ';
    for (int x = 0; x < 64; x++) {
      int hgt = g_hmap[y * 64 + x];
      line[n++] = hgt ? (char)('0' + (hgt / 8 > 9 ? 9 : hgt / 8)) : '.';
    }
    line[n] = 0;
    Hd3d_Log("  %2d %s", y, line);
  }
  Hd3d_Log("  collision types (hex per tile):");
  for (int y = 0; y < 64; y++) {
    char hex[64 * 2 + 1];
    for (int x = 0; x < 64; x++)
      snprintf(hex + x * 2, 3, "%02x", attr[y * 64 + x]);
    Hd3d_Log("  %2d %s", y, hex);
  }
}

static void BuildHeightMap(void) {
  // the table of the layer Link is on, as the game's tile detection reads it
  const uint8 *attr = dung_bg2_attr_table + (link_is_on_lower_level ? 0x1000 : 0);
  bool objects = g_room_id == dungeon_room_index && g_n_room_objs > 0;
  uint8 cls[64 * 64];
  int cells = 0;
  if (!objects) {
    // no object info: every solid tile is a wall (first version)
    for (int i = 0; i < 64 * 64; i++)
      cls[i] = IsWallAttr(attr[i]) ? kCls_Wall : kCls_Floor;
  } else {
    for (int i = 0; i < 64 * 64; i++) {
      cls[i] = (uint8)CellClass(i);
      // decorations drawn on a wall (shelves, the pot rack, bars...) are wall
      if (cls[i] == kCls_Floor && IsWallAttr(attr[i]))
        cls[i] = kCls_Wall;
    }
  }
  // Wall tiles: the wall objects' tiles. Only the top rows of a north wall
  // are solid for the game; its painted face below them is walkable, but it
  // is part of the wall and stands up with it. A run of wall tiles with no
  // solid tile at all (a doorway) stays flat. The ceiling (the dark void
  // above walls) does not count toward a wall's height: it becomes a flat
  // dark top at the height of the wall in front of it.
  uint8 wall[64 * 64], ceil[64 * 64], solid[64 * 64];
  for (int i = 0; i < 64 * 64; i++) {
    ceil[i] = cls[i] == kCls_Ceiling;
    wall[i] = IsWallClass(cls[i]) && !ceil[i];
    solid[i] = wall[i] || ceil[i];
  }

  memset(g_hmap, 0, sizeof(g_hmap));
  memset(g_hfold, 0, sizeof(g_hfold));
  for (int cx = 0; cx < 64; cx++) {
    int cy = 0;
    while (cy < 64) {
      int c = cy * 64 + cx;
      if (wall[c]) {
        int y0 = cy;
        bool any_solid = false;
        while (cy < 64 && wall[cy * 64 + cx])
          any_solid |= IsWallAttr(attr[cy * 64 + cx]), cy++;
        if (!any_solid)
          continue;   // a doorway: walkable all the way
        bool floor_n = y0 > 0 && !solid[(y0 - 1) * 64 + cx];
        bool floor_s = cy < 64 && !solid[cy * 64 + cx];
        int hgt = (cy - y0) * 8;
        bool fold = true;
        if (floor_n && !floor_s) {
          hgt = 16, fold = false;          // south wall: we look at its top
        } else if (!floor_s) {
          if (hgt > 32) hgt = 32;          // side walls (run to the next wall)
        }
        if (hgt > kWallMax) hgt = kWallMax;
        for (int y = y0; y < cy; y++)
          g_hmap[y * 64 + cx] = (uint8)hgt, g_hfold[y * 64 + cx] = fold;
        cells += cy - y0;
        continue;
      }
      int k = cls[c];
      if (k != kCls_Floor && !IsWallClass(k)) {
        // an object: as tall as its class (or its depth, if smaller)
        int y0 = cy;
        while (cy < 64 && cls[cy * 64 + cx] == k)
          cy++;
        int hgt = (cy - y0) * 8;
        if (hgt > kClsHeight[k]) hgt = kClsHeight[k];
        for (int y = y0; y < cy; y++)
          g_hmap[y * 64 + cx] = (uint8)hgt, g_hfold[y * 64 + cx] = 1;
        cells += cy - y0;
        continue;
      }
      cy++;
    }
  }
  // ceilings: flat and dark, as high as the wall next to them (south first)
  for (int cx = 0; cx < 64; cx++) {
    int cy = 0;
    while (cy < 64) {
      if (!ceil[cy * 64 + cx]) {
        cy++;
        continue;
      }
      int y0 = cy;
      while (cy < 64 && ceil[cy * 64 + cx])
        cy++;
      int hgt = 0;
      if (cy < 64 && wall[cy * 64 + cx])
        hgt = g_hmap[cy * 64 + cx];
      else if (y0 > 0 && wall[(y0 - 1) * 64 + cx])
        hgt = g_hmap[(y0 - 1) * 64 + cx];
      for (int y = y0; y < cy; y++)
        g_hmap[y * 64 + cx] = (uint8)hgt, g_hfold[y * 64 + cx] = 0;
      cells += cy - y0;
    }
  }
  g_stat_wall_cells += cells;
  if (g_room_dump_request) {
    g_room_dump_request = false;
    if (!objects)
      Hd3d_Log("room dump: no object info for room %d (built for %d)", dungeon_room_index, g_room_id);
    RoomDump(attr, cls);
  }
}

static inline int HeightAt(const Ground *gr, float u, float v, int *fold) {
  int wx = gr->org_x + (int)floorf(u), wy = gr->org_y + (int)floorf(v);
  int c = ((wy >> 3) & 63) * 64 + ((wx >> 3) & 63);
  *fold = g_hfold[c];
  return g_hmap[c];
}

// ground color at frame coordinates (u, v): the PPU frame inside the SNES
// screen, past it the room tiles
static bool SampleSurface(const Ground *gr, float u, float v, uint32 *out) {
  int ui = (int)floorf(u), vi = (int)floorf(v);
  if (ui >= 0 && ui < g_w && vi >= 0 && vi < g_h) {
    *out = g_floor[vi * g_w + ui];
    return true;
  }
  g_stat_outside++;
  return Ground_Sample(gr, ui, vi, out);
}

static void DrawTerrain(uint32 *out, int pitch_px, const Cam *cm, const Ground *gr, const Sky *sk) {
  int w = g_w, h = g_h;
  uint32 fog = sk->outdoors ? 3u << 24 | sk->horizon : 5u << 24;
  for (int x = 0; x < w; x++) {
    int ybuf = h;   // rows ybuf..h-1 of this column are drawn
    for (int y = h - 1; y >= 0 && ybuf > 0; y--) {
      float u, v, depth;
      if (!Cam_Ray(cm, x + 0.5f, y + 0.5f, &u, &v, &depth) || depth >= kFogEnd) {
        for (int r = 0; r < ybuf; r++) {
          out[r * pitch_px + x] = Sky_Color(sk, cm, x + 0.5f, r + 0.5f, x, r);
          g_depth[r * w + x] = 1e9f;
          g_uv[r * w + x] = kUvNone, g_kind[r * w + x] = kKind_Sky;
        }
        ybuf = 0;
        break;
      }
      int f = depth > kFogStart ? (int)((depth - kFogStart) * (256.0f / (kFogEnd - kFogStart))) : 0;
      if (f > 256) f = 256;
      int fold;
      int hgt = HeightAt(gr, u, v, &fold);
      int top = y;
      float sy_top = y + 0.5f;
      if (hgt) {
        float sx_t, sc_t;
        if (Cam_ToScreenH(cm, u, v, (float)hgt, &sx_t, &sy_top, &sc_t)) {
          top = (int)floorf(sy_top);
          if (top < 0) top = 0;
        }
      }
      if (top >= ybuf)
        continue;   // hidden behind what is already drawn
      float span = (y + 0.5f) - sy_top;
      for (int r = ybuf - 1; r >= top; r--) {
        // height of this row on the face, then fold-up: art z pixels north
        float z = 0.0f;
        if (hgt && fold && span > 0.01f) {
          z = hgt * ((y + 0.5f) - (r + 0.5f)) / span;
          z = z < 0 ? 0 : z > hgt ? (float)hgt : z;
        }
        uint32 px;
        int su = (int)floorf(u), sv = (int)floorf(v - z);
        uint8 k = su >= 0 && su < w && sv >= 0 && sv < h ? kKind_Frame : kKind_Outside;
        if (!SampleSurface(gr, u, v - z, &px)) {
          g_stat_missing++;
          px = fog;
          k = kKind_Sky;
        } else if (f) {
          px = (px & 0xff000000) | Lerp(px, fog, f);
        }
        out[r * pitch_px + x] = px;
        g_uv[r * w + x] = PackUv(su, sv), g_kind[r * w + x] = k;
        g_depth[r * w + x] = depth;
      }
      ybuf = top;
    }
  }
}

typedef struct Group {
  int key;
  float depth;         // of the feet from the camera, -1 = not drawn
  int x0, y0, x1, y1;
  bool far;            // drawn from the OAM table (pieces outside the SNES screen)
  int n_ents;
  uint16 ents[24];     // entries of a far group (0-127 OAM, then far pieces), first wins
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
  // farther away drawn first (with the camera turned, that is not the top)
  if (ga->depth != gb->depth)
    return ga->depth > gb->depth ? -1 : 1;
  return ga->y1 - gb->y1;
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
// Far enemies' pieces, kept out of the real OAM (Sprite_Main in sprite.c)
typedef struct FarPiece {
  int16 x, y;          // relative to the game camera, like the OAM
  uint16 char_flags;   // OAM word 1
  uint8 big, slot;
} FarPiece;
enum { kMaxFarPieces = 256 };
static FarPiece g_far[kMaxFarPieces];
static int g_n_far;

void Hd3d_FarPiecesClear(void) {
  g_n_far = 0;
}

void Hd3d_FarPiece(int slot, int x, int y, uint16 char_flags, int big) {
  if (g_n_far == kMaxFarPieces)
    return;
  FarPiece *f = &g_far[g_n_far++];
  f->x = (int16)x, f->y = (int16)y, f->char_flags = char_flags, f->big = (uint8)big, f->slot = (uint8)slot;
}

static bool OamPixel(Ppu *ppu, int oam1, int col, int row, int size, uint32 *out) {
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
static bool g_dump_request;   // sprite dump to the log on the next 3D frame (key 6)

void Hd3d_RequestDump(void) {
  g_dump_request = true;
  Hd3d_Log("sprite dump requested (key 6)%s", g_active_last ? "" : ": the 3D view is off, it runs on the next 3D frame");
}

static int DrawSprites(uint32 *out, int pitch_px, const Cam *cm, Ppu *ppu) {
  int w = g_w, h = g_h;
  Group groups[kMaxGroups];
  int16 key_to_group[32 + 128 + 16];   // + far enemies (160 + slot)
  memset(key_to_group, 0xff, sizeof(key_to_group));
  int n = 0;

  // Groups with a piece outside the SNES screen are drawn entirely from the
  // OAM table (the PPU only drew their on-screen part)
  bool far_key[32 + 128 + 16];
  memset(far_key, 0, sizeof(far_key));
  // far pieces need their true positions (ResolveSpritePositions runs while
  // the draw distance is on)
  bool far_ok = ppu->objXValid && ppu->objYValid && g_view3d_dist > 0;
  // entries: 0-127 the OAM, then the far enemies' pieces
  static int efx[128 + kMaxFarPieces], efy[128 + kMaxFarPieces], esz[128 + kMaxFarPieces];
  static int ecf[128 + kMaxFarPieces];
  bool evis[128];
  for (int i = 0; i < 128; i++)
    ecf[i] = ppu->oam[i * 2 + 1];
  for (int i = 0; i < 128; i++)
    evis[i] = OamEntry(ppu, i, &efx[i], &efy[i], &esz[i]);
  OamGroups(evis, efx, efy, esz);
  for (int i = 0; i < 128; i++) {
    if (far_ok && evis[i] && (efx[i] < 0 || efy[i] < 0 || efx[i] + esz[i] > w || efy[i] + esz[i] > h))
      far_key[GroupKey(i)] = true;
  }

  bool dump = g_dump_request;
  g_dump_request = false;
  int captured[128];   // pixels the PPU drew for each OAM entry (dump only)
  if (dump)
    memset(captured, 0, sizeof(captured));
  memset(g_group_map, 0, w * h);
  for (int i = 0; i < w * h; i++) {
    uint8 o = g_owner[i];
    if (o >= 128)
      continue;
    if (dump)
      captured[o]++;
    // a pixel credited to an entry far from where that entry is: leave it
    // out, or its group's box (and feet) stretched across the frame and part
    // of the sprite was stood up in the wrong place
    {
      int x = i % w, y = i / w, sz = esz[o];
      int scale = ppu->objScale[o];
      int m = 2 + (scale ? IntMax(((sz * scale >> 8) - sz + 1) >> 1, 0) : 0);
      if (!evis[o] || x < efx[o] - m || x >= efx[o] + sz + m || y < efy[o] - m || y >= efy[o] + sz + m)
        continue;
    }
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
    if (g->n_ents == (int)(sizeof(g->ents) / sizeof(g->ents[0])))
      continue;
    g->ents[g->n_ents++] = (uint16)i;
    if (efx[i] < g->x0) g->x0 = efx[i];
    if (efx[i] + esz[i] - 1 > g->x1) g->x1 = efx[i] + esz[i] - 1;
    if (efy[i] < g->y0) g->y0 = efy[i];
    if (efy[i] + esz[i] - 1 > g->y1) g->y1 = efy[i] + esz[i] - 1;
  }
  // far enemies (kept out of the OAM): one group per enemy slot
  for (int p = 0; p < g_n_far; p++) {
    const FarPiece *f = &g_far[p];
    int e = 128 + p;
    efx[e] = f->x - ppu->objBiasX + ppu->extraLeftRight;
    efy[e] = f->y - ppu->objBiasY;
    esz[e] = kObjSizes[ppu->objSize][f->big & 1];
    ecf[e] = f->char_flags;
    int key = 160 + (f->slot & 15);
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
    if (g->n_ents == (int)(sizeof(g->ents) / sizeof(g->ents[0])))
      continue;
    g->ents[g->n_ents++] = (uint16)e;
    if (efx[e] < g->x0) g->x0 = efx[e];
    if (efx[e] + esz[e] - 1 > g->x1) g->x1 = efx[e] + esz[e] - 1;
    if (efy[e] < g->y0) g->y0 = efy[e];
    if (efy[e] + esz[e] - 1 > g->y1) g->y1 = efy[e] + esz[e] - 1;
  }
  g_stat_far_groups += far_groups;
  if (dump) {
    Hd3d_Log("=== sprite dump: module=%d sub=%d room=%d area=0x%02x indoors=%d tilt=%.1f dist=%s objXValid=%d objYValid=%d "
             "bias=%d,%d objSize=%d far pieces=%d groups=%d",
             main_module_index, submodule_index, dungeon_room_index, BYTE(overworld_screen_index), player_is_indoors,
             g_angle, kDistNames[g_view3d_dist], ppu->objXValid, ppu->objYValid, ppu->objBiasX, ppu->objBiasY,
             ppu->objSize, g_n_far, n);
    for (int i = 0; i < 128; i++) {
      if (!evis[i] && !captured[i])
        continue;
      int cf = ecf[i], o = g_sprite_oam_owner[i];
      Hd3d_Log("  oam %3d: %s frame=(%d,%d) size=%d tile=0x%03x pal=%d prio=%d flip=%c%c owner=%s%d key=%d far=%d "
               "captured_px=%d",
               i, evis[i] ? "vis " : "HID ", efx[i], efy[i], esz[i], cf & 0x1ff, (cf >> 9) & 7, (cf >> 12) & 3,
               (cf & 0x4000) ? 'H' : '-', (cf & 0x8000) ? 'V' : '-',
               g_link_oam_mask[i] ? "link" : o < 16 ? "slot" : "none", o < 16 ? o : -1, GroupKey(i),
               far_key[GroupKey(i)], captured[i]);
    }
    for (int p = 0; p < g_n_far; p++)
      Hd3d_Log("  far piece %d: slot=%d cam-rel=(%d,%d) tile=0x%03x pal=%d big=%d", p, g_far[p].slot, g_far[p].x,
               g_far[p].y, g_far[p].char_flags & 0x1ff, (g_far[p].char_flags >> 9) & 7, g_far[p].big);
  }
  if (n == 0)
    return 0;
  // sort far to near, remembering each group's id in g_group_map
  for (int i = 0; i < n; i++) {
    Group *g = &groups[i];
    g->key = g->key << 8 | i;
    float sx, sy, sc;
    g->depth = Cam_ToScreen(cm, (g->x0 + g->x1 + 1) * 0.5f, (float)(g->y1 + 1), &sx, &sy, &sc) ? cm->F / sc : -1.0f;
  }
  qsort(groups, n, sizeof(Group), CompareGroups);

  for (int gi = 0; gi < n; gi++) {
    Group *g = &groups[gi];
    int id = (g->key & 0xff) + 1;
    float ax = (g->x0 + g->x1 + 1) * 0.5f, ay = (float)(g->y1 + 1);   // feet
    float sx, sy, sc;
    if (!Cam_ToScreen(cm, ax, ay, &sx, &sy, &sc)) {
      if (dump)
        Hd3d_Log("  group key=%d far=%d box=(%d,%d)-(%d,%d): NOT DRAWN (behind the camera)", g->key >> 8, g->far,
                 g->x0, g->y0, g->x1, g->y1);
      continue;
    }
    // far away: same fog as the ground, gone at the fog end
    float depth = cm->F / sc;
    if (dump)
      Hd3d_Log("  group key=%d far=%d box=(%d,%d)-(%d,%d) pieces=%d -> screen feet=(%.0f,%.0f) scale=%.2f%s",
               g->key >> 8, g->far, g->x0, g->y0, g->x1, g->y1, g->n_ents, sx, sy, sc,
               depth >= kFogEnd ? " NOT DRAWN (past the fog)" : "");
    if (depth >= kFogEnd)
      continue;
    // behind a wall: the wall's pixels are nearer than the feet
    float zlimit = depth - 4.0f;
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
          if (vx >= g->x0 && vx <= g->x1 && gm[vx] == id &&
              !(g_depth_valid && g_depth[y * w + x] < zlimit))
            dst[x] = src[vx], g_uv[y * w + x] = PackUv(vx, vy), g_kind[y * w + x] = kKind_Sprite;
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
                OamPixel(ppu, ecf[i], lx, ly, esz[i], &c)) {
              if (!(g_depth_valid && g_depth[y * w + x] < zlimit))
                dst[x] = c, g_uv[y * w + x] = PackUv(vx, vy), g_kind[y * w + x] = kKind_Sprite;
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

  Ground gr;
  Ground_Setup(&gr, g_zenv.ppu, width);
  Cam cm;
  if (g_view3d_cam == kHd3d_CamThird) {
    // Link's feet in frame coordinates, placed the way his sprite is (OAM
    // position relative to the game camera, then the sprite bias, as in
    // OamEntry). Not from the ground origin: indoors that comes from the PPU
    // scroll, which wraps at 1024, while Link's dungeon coordinates do not
    // (the camera looked far away from him in big dungeons: all black)
    Ppu *ppu = g_zenv.ppu;
    float lx = (int16)(link_x_coord - BG2HOFS_copy2) - ppu->objBiasX + ppu->extraLeftRight + 8.0f;
    float ly = (int16)(link_y_coord - BG2VOFS_copy2) - ppu->objBiasY + 20.0f;
    Cam_SetupThird(&cm, width, height, lx, ly);
  } else {
    Cam_Setup(&cm, width, height);
  }
  bool outdoors = IsOverworldModule(main_module_index) ||
                  (main_module_index == 14 && IsOverworldModule(saved_module_for_menu));
  Sky sk;
  Sky_Setup(&sk, outdoors, width);
  uint32 *out = (uint32 *)pixels;
  bool walls = g_view3d_walls && player_is_indoors;
  if (walls) {
    BuildHeightMap();
    DrawTerrain(out, pitch_px, &cm, &gr, &sk);
  } else {
    DrawGround(out, pitch_px, &cm, &gr, &sk);
  }
  g_depth_valid = walls;
  int groups = DrawSprites(out, pitch_px, &cm, g_zenv.ppu);
  // the HD light and forest fog ran on the 2D frame: the ground past it and
  // the sprites (drawn from the side buffer) get them now, at the 2D point
  // they show
  HdLight_Projected(pixels, pitch, width, height, g_uv, g_kind);
  HdFx_FogProjected(pixels, pitch, width, height, g_uv, g_kind);
  HdFx_SoftIrisProjected(pixels, pitch, width, height, g_uv, g_kind);
  if (g_weather) {
    // the game's weather layer, laid on the tilted ground and on the sprites
    // (at the 2D frame pixel each one shows, so it follows the perspective).
    // With the HD rain on it only darkens (its drops would be a second rain)
    HdWorld_SuppressOverlay(false);
    HdWorld_OverlayFlat(pixels, pitch, width, height, g_fx_rain && HdFx_IsRainingOutdoors(), g_uv);
  }
  HdFx_FlatWeather(pixels, pitch, width, height);   // HD rain drops and lightning, flat
  DrawOverlay(out, pitch_px);

  g_stat_ticks += SDL_GetPerformanceCounter() - t0;
  g_stat_groups += groups;
  if (groups > g_stat_max_groups)
    g_stat_max_groups = groups;
  if (++g_stat_frames == 300) {
    double ms = g_stat_ticks * 1000.0 / SDL_GetPerformanceFrequency() / g_stat_frames;
    Hd3d_Log("stats: %d frames, 3D compose %.3f ms/frame, sprite groups avg %.1f max %d, tilt %.1f, camera %s (yaw %.0f pitch %.0f), curve R=%.0f, "
             "past-screen px/frame %d (missing %d), source %s, far sprites avg %.1f (draw distance %s, %s), "
             "walls %s (wall tiles avg %d), "
             "frame %dx%d, module=%d area=0x%02x",
             g_stat_frames, ms, (double)g_stat_groups / g_stat_frames, g_stat_max_groups, g_angle,
             kCamNames[g_view3d_cam], g_yaw * (180.0f / 3.14159265f), g_pitch3, cm.R,
             g_stat_outside / g_stat_frames, g_stat_missing / g_stat_frames, kSourceNames[gr.source],
             (double)g_stat_far_groups / g_stat_frames, kDistNames[g_view3d_dist],
             g_view3d_dist && !(HdWorld_Active() || player_is_indoors) ? "needs continuous world outdoors" : "ok",
             g_view3d_walls ? (player_is_indoors ? "on" : "on (indoors only)") : "off", g_stat_wall_cells / g_stat_frames,
             width, height, main_module_index, BYTE(overworld_screen_index));
    g_stat_ticks = 0, g_stat_frames = 0, g_stat_groups = 0, g_stat_max_groups = 0;
    g_stat_outside = 0, g_stat_missing = 0, g_stat_far_groups = 0, g_stat_wall_cells = 0;
  }
}
