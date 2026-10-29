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
#include "dungeon.h"
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

int Hd3d_ViewDir(int dir) {
  // sprite directions -> Link's facing and back
  static const uint8 kDirToFacing[4] = { 6, 4, 2, 0 };
  int f = Hd3d_ViewFacing(kDirToFacing[dir & 3]);
  static const uint8 kFacingToDir[4] = { 3, 2, 1, 0 };   // up, down, left, right
  return kFacingToDir[(f >> 1) & 3];
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
  // every sprite pixel on top of BG1/BG2 in this copy: the 2D game hides
  // Link under a bridge or behind a wall top with the BG priority, and then
  // he was missing from the cutout; in 3D the depth buffer hides him
  // (objBuffer holds the priority in its top 4 bits, see snes/ppu.c)
  static PpuZbufType saved[kPpuXPixels];
  memcpy(saved, ppu->objBuffer.data, sizeof(saved));
  for (int i = 0; i < kPpuXPixels; i++)
    if (ppu->objBuffer.data[i] & 0xff)
      ppu->objBuffer.data[i] = (PpuZbufType)((ppu->objBuffer.data[i] & 0x0fff) | (3 * 4 + 2) << 12);
  ppu_redrawLine(ppu, line, (uint8 *)g_side, g_w * 4, g_weather ? 1 : 0);
  memcpy(ppu->objBuffer.data, saved, sizeof(saved));
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

// pixel (col, row) of the tile `e` (tilemap word) of a BG layer: the cgram
// index, 0 = transparent
static int TileWordPixel(Ppu *ppu, int layer, uint16 e, int col, int row) {
  if (e & 0x8000) row = 7 - row;
  if (e & 0x4000) col = 7 - col;
  const uint16 *t = &ppu->vram[(ppu->bgLayer[layer].tileAdr + (e & 0x3ff) * 16 + row) & 0x7fff];
  int bit = 7 - col;
  int v = (t[0] >> bit & 1) | (t[0] >> (8 + bit) & 1) << 1 | (t[8] >> bit & 1) << 2 | (t[8] >> (8 + bit) & 1) << 3;
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
static void Cam_SetupThird(Cam *cm, int w, int h, float lx, float ly, float lz) {
  float th = g_pitch3 * (3.14159265f / 180.0f);
  float hx = sinf(g_yaw), hy = -cosf(g_yaw);
  cm->F = kCamDist;
  cm->cx = w * 0.5f, cm->cy = h * 0.5f;
  Cam_Orient(cm, th, hx, hy);
  cm->R = g_radius_inv > 1e-6f ? 1.0f / g_radius_inv : 0.0f;
  cm->tx = lx, cm->ty = ly;
  // aim a little ahead of Link and at his chest, so he stands in the lower
  // half of the screen with the way ahead open
  float ax = lx + hx * kThirdAhead, ay = ly + hy * kThirdAhead, az = kThirdAimZ + lz;
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
// Drawing casts one ray per screen pixel through the room's cells (see
// CastWalls), so tops, faces and sides are each found and painted by their
// own rule. A depth buffer keeps characters behind walls hidden.

enum { kWallMax = 48 };          // tallest wall, pixels
static uint8 g_hmap[64 * 64];    // height per 8x8 tile of the room
static uint8 g_hfold[64 * 64];   // 1 = the surface shows the art z pixels north (fold-up)
static uint8 g_hbase[64 * 64];   // fold-up: tile row just south of the run (where its face stands)
static uint8 g_hlayer[64 * 64];  // layer that gave the cell its class (0 = BG2, 1 = BG1)
static uint8 g_partial[64 * 64]; // 1 = an object cell: only the pixels in g_praise stand up
static uint8 g_praise[512 * 512];// object cells: height of this pixel, 0 = floor drawn in the tile
static uint8 g_pdown[512 * 512]; // object cells: rows from this pixel down to its column's front (face base)
static uint8 g_pcol[512 * 512];  // object cells: cgram index shown (scratch for BuildPixelMasks)
// object boxes (objects with solid collision): per pixel of the footprint,
// the box it belongs to (index + 1, 0 = not a box pixel)
typedef struct ObjBox {
  int16 x0, w;           // room pixel column of the west edge, width
  int16 depth;           // pixels from the front to the back
  uint8 seat_h, seat_d;  // chairs: seat height and depth (0 = a plain box)
  float k, kt;           // drawing rows per pixel of surface, weight of a top (see ShadeWall)
} ObjBox;
enum { kMaxBoxes = 64 * 32 };
static ObjBox g_boxes[kMaxBoxes];
static uint16 g_pbox[512 * 512];
// floor drawn over the rest of a box object's drawing: the floor this many
// 16 pixel steps to the side (0 = none)
static int8 g_pcover[512 * 512];
static uint32 g_mask_hash;       // tiles the masks were built for
// Two floor levels (see BuildHeightMap): the floor height of each cell, the
// room's upper level height and the tallest surface (where rays start)
static uint8 g_hfloor[64 * 64];
static uint8 g_lower[64 * 64];   // 1 = BG2 has a hole there: the cell shows BG1
static uint8 g_upper[64 * 64];   // 1 = on the upper floor
static uint8 g_hbot[64 * 64];    // bridges: height of the deck's underside (open below), 0 = solid to the floor
static bool g_two_levels;
static bool g_raised;            // some floor is above 0 (two levels or a chest platform)
enum { kPlatformH = 8 };         // chest platforms (the throne room's dais) above their floor
static int g_level_h, g_hmax = kWallMax;
static int g_org_x, g_org_y;     // ground origin of the last 3D frame (room pixel of frame pixel 0, 0)
static float g_aim_floor;        // third person: floor height under Link, eased
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
  kCls_Pot,        // big pots (4x4)
  kCls_Chair,      // chairs and thrones: a low seat in front, a tall back behind
  kCls_Count,
};
static const char kClsLetter[kCls_Count] = { '.', 'W', 'C', 'D', 'r', 'f', 'o', 'b', 'T', 't', 'P', 'h' };
static const char *const kClsName[kCls_Count] = {
  "floor", "wall", "ceiling", "door", "rail", "furniture", "small", "block", "tall", "torch", "bigpot", "chair",
};
static const uint8 kClsHeight[kCls_Count] = { 0, kWallMax, kWallMax, kWallMax, 8, 12, 10, 16, 28, 14, 20, 28 };

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
    if (id == 0x22 || id == 0x2f || id == 0x30 || id == 0x5f || id == 0x69 || id == 0x6c || id == 0x6d)
      return kCls_Rail;
    if (id == 0x88 || id == 0x8a)
      return kCls_Block;   // large / long vertical rails: long low barriers
    if (id == 0x3c)
      return kCls_Chair;   // double chair
    if (id == 0xdd)
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
    if (id == 0x24 || id == 0x25)
      return kCls_Chair;   // thrones (Hyrule Castle), drawn like the 4x4 wall corners
    if (id == 0x1c)
      return kCls_Pot;   // drawn like the 4x4 wall corners, collision 0x27
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

// --- class of each tile number ---
//
// The game gives every 8x8 tile number (0-0x3ff) its collision type through
// one table (attributes_for_tile), and every object draws its tiles from one
// fixed table: a tile number means the same thing in every dungeon, only the
// art changes. So the class (wall, ceiling, furniture...) is kept per tile
// number too, not per object: a wall object also draws the ceiling above it
// and the trim at its base, and those got the wall's class.
//
// The classes are seeded once per launch: every room is built (on a copy of
// the game RAM, put back after) and each tile votes for the class of the
// object that drew it. The result goes to hd3d_tiles_seed.txt; lines copied
// to hd3d_tiles.txt ("1a3 wall") override single tiles; "1a3 tall full"
// also stands the whole tile up (see BuildPixelMasks). Key 8 (room dump)
// reloads that file.
enum { kNumTileNums = 0x400, kNumRooms = 0x128 };
static uint8 g_tile_cls[kNumTileNums];        // class + 1, 0 = never seen
static uint8 g_tile_override[kNumTileNums];   // class + 1, 0 = none
static uint8 g_tile_full[kNumTileNums];       // 1 = stands up whole, never cut by the floor mask
static uint32 (*g_votes)[kCls_Count];         // filled while seeding, kept for the room dump
static bool g_seeding, g_tiles_ready;

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

static void VoteRoomTiles(void) {
  for (int l = 0; l < 2; l++) {
    const uint16 *bg = l ? dung_bg1 : dung_bg2;
    for (int i = 0; i < 64 * 64; i++) {
      int o = g_tile_obj[l][i];
      g_votes[bg[i] & 0x3ff][o ? g_room_objs[o - 1].cls : kCls_Floor]++;
    }
  }
}

void Hd3d_RoomBuildEnd(void) {
  g_room_building = false;
  if (g_seeding) {
    VoteRoomTiles();
    return;
  }
  int counts[kCls_Count] = { 0 };
  for (int i = 0; i < g_n_room_objs; i++)
    counts[g_room_objs[i].cls]++;
  Hd3d_Log("room %d built: %d objects (walls %d, ceiling %d, doors %d, rails %d, furniture %d, small %d, blocks %d, "
           "tall %d, torches %d)", g_room_id, g_n_room_objs, counts[kCls_Wall], counts[kCls_Ceiling],
           counts[kCls_Door], counts[kCls_Rail], counts[kCls_Furniture], counts[kCls_Small], counts[kCls_Block],
           counts[kCls_Tall], counts[kCls_Torch]);
}

static int ClassByName(const char *name) {
  for (int c = 0; c < kCls_Count; c++)
    if (!strcmp(name, kClsName[c]))
      return c;
  return -1;
}

// hd3d_tiles.txt: "<tile number in hex> <class> [full]" per line, # starts a
// comment
static void LoadTileOverrides(void) {
  memset(g_tile_override, 0, sizeof(g_tile_override));
  memset(g_tile_full, 0, sizeof(g_tile_full));
  g_mask_hash = 0;
  FILE *f = fopen("hd3d_tiles.txt", "r");
  if (!f)
    return;
  char line[256], name[32], opt[32];
  int n = 0, bad = 0;
  unsigned tile;
  while (fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    int got = sscanf(line, "%x %31s %31s", &tile, name, opt);
    if (got < 2)
      continue;
    int c = ClassByName(name);
    if (tile >= kNumTileNums || c < 0) {
      bad++;
      continue;
    }
    g_tile_override[tile] = (uint8)(c + 1), n++;
    g_tile_full[tile] = got == 3 && !strcmp(opt, "full");
  }
  fclose(f);
  Hd3d_Log("hd3d_tiles.txt: %d tile classes%s", n, bad ? " (some lines not understood)" : "");
}

static void WriteTileSeed(void) {
  FILE *f = fopen("hd3d_tiles_seed.txt", "w");
  if (!f)
    return;
  fprintf(f, "# Class of each dungeon tile number, voted by the room objects that draw it\n"
             "# (every room built once). Not read back: copy lines to hd3d_tiles.txt and\n"
             "# change the class there. Classes:");
  for (int c = 0; c < kCls_Count; c++)
    fprintf(f, " %s", kClsName[c]);
  fprintf(f, "\n# ? = less than 80%% of the votes agree\n");
  for (int t = 0; t < kNumTileNums; t++) {
    if (!g_tile_cls[t])
      continue;
    uint32 total = 0;
    for (int c = 0; c < kCls_Count; c++)
      total += g_votes[t][c];
    int best = g_tile_cls[t] - 1;
    int pct = (int)(g_votes[t][best] * 100 / total);
    fprintf(f, "%03x %-9s # %s attr %02x, %d%%:", t, kClsName[best], pct < 80 ? "?" : " ", attributes_for_tile[t], pct);
    for (int c = 0; c < kCls_Count; c++)
      if (g_votes[t][c])
        fprintf(f, " %s %u", kClsName[c], g_votes[t][c]);
    fprintf(f, "\n");
  }
  fclose(f);
}

// Builds every room on a copy of the game RAM and puts it all back
static void SeedTileClasses(void) {
  g_tiles_ready = true;
  uint8 *ram = malloc(sizeof(g_ram));
  RoomObj *objs = malloc(sizeof(g_room_objs));
  uint16 *tile_obj = malloc(sizeof(g_tile_obj));
  g_votes = calloc(kNumTileNums, sizeof(*g_votes));
  if (!ram || !objs || !tile_obj || !g_votes) {
    free(ram), free(objs), free(tile_obj), free(g_votes), g_votes = NULL;
    return;
  }
  uint64 t0 = SDL_GetPerformanceCounter();
  memcpy(ram, g_ram, sizeof(g_ram));
  memcpy(objs, g_room_objs, sizeof(g_room_objs));
  memcpy(tile_obj, g_tile_obj, sizeof(g_tile_obj));
  int n_objs = g_n_room_objs, room_id = g_room_id;

  g_seeding = true;
  for (int room = 0; room < kNumRooms; room++) {
    dungeon_room_index = (uint16)room;
    Dungeon_LoadRoom();
  }
  g_seeding = false;

  memcpy(g_ram, ram, sizeof(g_ram));
  memcpy(g_room_objs, objs, sizeof(g_room_objs));
  memcpy(g_tile_obj, tile_obj, sizeof(g_tile_obj));
  g_n_room_objs = n_objs, g_room_id = room_id;

  int seen = 0, unsure = 0;
  for (int t = 0; t < kNumTileNums; t++) {
    uint32 total = 0, top = 0;
    int best = 0;
    for (int c = 0; c < kCls_Count; c++) {
      total += g_votes[t][c];
      if (g_votes[t][c] > top)
        top = g_votes[t][c], best = c;
    }
    if (!total)
      continue;
    g_tile_cls[t] = (uint8)(best + 1);
    seen++;
    unsure += top * 5 < total * 4;
  }
  WriteTileSeed();
  free(ram), free(objs), free(tile_obj);   // the votes stay, for the room dump
  double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
  Hd3d_Log("tile classes seeded from %d rooms in %.0f ms: %d tile numbers, %d unsure (hd3d_tiles_seed.txt)",
           kNumRooms, ms, seen, unsure);
  LoadTileOverrides();
}

// The room on screen was not built since launch (a saved game started in
// it): build it again on a copy of the game RAM, only to learn its objects
static void CaptureRoomObjects(void) {
  static int tried = -1;
  if (tried == dungeon_room_index)
    return;
  tried = dungeon_room_index;
  uint8 *ram = malloc(sizeof(g_ram));
  if (!ram)
    return;
  memcpy(ram, g_ram, sizeof(g_ram));
  Dungeon_LoadRoom();
  memcpy(g_ram, ram, sizeof(g_ram));
  free(ram);
  Hd3d_Log("room %d: objects read again (it was not built since launch)", g_room_id);
}

void Hd3d_RequestRoomDump(void) {
  g_room_dump_request = true;
  Hd3d_Log("room dump requested (key 8)");
  if (g_tiles_ready)
    LoadTileOverrides();
}

// Tallest class of the cell's two layers (or of layer `only`: 0 = BG2,
// 1 = BG1, -1 = both): by tile number (hd3d_tiles.txt, then the seed), else
// by the object that drew it (objects = that info is for this room)
static int CellClass(int cell, bool objects, uint8 *layer, int only) {
  int best = kCls_Floor;
  *layer = only > 0 ? 1 : 0;
  for (int l = 0; l < 2; l++) {
    if (only >= 0 && l != only)
      continue;
    int t = (l ? dung_bg1 : dung_bg2)[cell] & 0x3ff;
    int k = g_tile_override[t] ? g_tile_override[t] : g_tile_cls[t];
    int o = g_tile_obj[l][cell];
    int c = k ? k - 1 : objects && o ? g_room_objs[o - 1].cls : kCls_Floor;
    if (kClsHeight[c] > kClsHeight[best])
      best = c, *layer = (uint8)l;
  }
  return best;
}

static bool IsWallClass(int c) {
  return c == kCls_Wall || c == kCls_Ceiling || c == kCls_Door;
}

// class letter of a tile number as the 3D view uses it, and the share of the
// seed votes that agree (-1 = from hd3d_tiles.txt, 0 = never seen)
static char TileClassLetter(int t, int *pct) {
  if (g_tile_override[t]) {
    *pct = -1;
    return kClsLetter[g_tile_override[t] - 1];
  }
  *pct = 0;
  if (!g_tile_cls[t])
    return '?';
  if (g_votes) {
    uint32 total = 0;
    for (int c = 0; c < kCls_Count; c++)
      total += g_votes[t][c];
    *pct = total ? (int)(g_votes[t][g_tile_cls[t] - 1] * 100 / total) : 0;
  }
  return kClsLetter[g_tile_cls[t] - 1];
}

// Close-up of the cells around Link, to find a tile that stands up wrong:
// both layers' tile numbers, the class, the height, both collision tables
// (the game keeps one per layer: bridges, stairs and the floor under them
// differ there) and what the object mask did
static bool Bg2Only(Ppu *ppu);

static void RoomDumpAroundLink(const uint8 *cls) {
  int lx = ((link_x_coord + 8) >> 3) & 63, ly = ((link_y_coord + 20) >> 3) & 63;
  Ppu *ppu = g_zenv.ppu;
  Hd3d_Log("  --- around Link: cell (%d,%d), link (%d,%d) z %d, on %s level, room header bg2 properties %d, "
           "main screen %02x sub screen %02x color math %02x %s", lx, ly, link_x_coord, link_y_coord, link_z_coord,
           link_is_on_lower_level ? "lower" : "upper", dung_hdr_bg2_properties, ppu->screenEnabled[0],
           ppu->screenEnabled[1], ppu->mathEnabled, ppu->subtractColor ? "subtract" : "add");
  int holes = 0, upper = 0;
  for (int i = 0; i < 64 * 64; i++)
    holes += g_lower[i], upper += g_upper[i];
  int bridge = 0;
  for (int i = 0; i < 64 * 64; i++)
    bridge += g_hbot[i] != 0;
  Hd3d_Log("  bridge cells (open below): %d%s", bridge, g_hbot[ly * 64 + lx] ? ", Link's cell is one" : "");
  Hd3d_Log("  two floor levels: %s (%s; %d upper cells, upper floor at %d px; Link's floor %d px), %s",
           g_two_levels ? "yes" : "no", holes ? "BG1 under BG2's holes" : "one layer, ledges", upper, g_level_h,
           g_hfloor[ly * 64 + lx], Bg2Only(ppu) ? "BG1 only through BG2's holes" : "BG1 on the main screen");
  Hd3d_Log("  each cell: BG2 tile/BG1 tile, class letter (capital = from BG1), height in px, collision BG2/BG1, "
           "mask (- whole cell, m object with raised pixels, F object flattened by the floor flood); [ ] = Link");
  for (int y = ly - 7; y <= ly + 7; y++) {
    char row[13 * 30 + 8];
    int n = 0;
    for (int x = lx - 6; x <= lx + 6; x++) {
      int c = (y & 63) * 64 + (x & 63);
      int t2 = dung_bg2[c] & 0x3ff, t1 = dung_bg1[c] & 0x3ff;
      char k = kClsLetter[cls[c]];
      if (g_hlayer[c] && k >= 'a' && k <= 'z') k = (char)(k - 32);
      char m = '-';
      if (g_partial[c]) {
        int raised = 0;
        for (int p = 0; p < 64; p++)
          raised += g_praise[((c >> 6) * 8 + (p >> 3)) * 512 + (c & 63) * 8 + (p & 7)] != 0;
        m = raised ? 'm' : 'F';
      }
      bool me = x == lx && y == ly;
      n += snprintf(row + n, sizeof(row) - n, "%c%03x/%03x %c%2d %02x/%02x %c%c", me ? '[' : ' ', t2, t1, k, g_hmap[c],
                    dung_bg2_attr_table[c], dung_bg1_attr_table[c], m, me ? ']' : ' ');
    }
    Hd3d_Log("  %2d %s", y & 63, row);
  }
}

// Objects that stand flat although they should not: their tiles' classes
// (an unknown object id is "floor", and so are the tiles only it draws)
static void RoomDumpObjects(void) {
  for (int i = 0; i < g_n_room_objs; i++) {
    const RoomObj *o = &g_room_objs[i];
    const char *kind = (o->code & 0xf00) == kHd3d_ObjSubtype1 ? "T1" : (o->code & 0xf00) == kHd3d_ObjSubtype2 ? "T2" :
                       (o->code & 0xf00) == kHd3d_ObjSubtype3 ? "T3" : (o->code & 0xf00) == kHd3d_ObjDoor ? "door" :
                       o->code == kHd3d_ObjPushBlock ? "pushblock" : "torch";
    // the tile numbers it drew that are still on screen, with their classes
    uint16 seen[12];
    int ns = 0, flat = 0, owned = 0;
    char tiles[12 * 12 + 1];
    int n = 0;
    tiles[0] = 0;
    for (int l = 0; l < 2; l++) {
      const uint16 *bg = l ? dung_bg1 : dung_bg2;
      for (int c = 0; c < 64 * 64; c++) {
        if (g_tile_obj[l][c] != i + 1)
          continue;
        owned++;
        int t = bg[c] & 0x3ff, pct;
        char k = TileClassLetter(t, &pct);
        flat += k == '.';
        bool dup = false;
        for (int j = 0; j < ns; j++)
          dup |= seen[j] == t;
        if (dup || ns == 12)
          continue;
        seen[ns++] = (uint16)t;
        if (pct < 0)
          n += snprintf(tiles + n, sizeof(tiles) - n, " %03x%c!", t, k);
        else
          n += snprintf(tiles + n, sizeof(tiles) - n, " %03x%c%d", t, k, pct);
      }
    }
    Hd3d_Log("  obj %3d pass %d %s 0x%02x at (%d,%d) -> %s, %d tiles (%d flat):%s%s", i, o->pass, kind,
             o->code & 0xff, o->x, o->y, kClsName[o->cls], owned, flat, tiles,
             o->cls == kCls_Floor && owned ? "   <- object id not classified" : "");
  }
}

static void RoomDump(const uint8 *attr, const uint8 *cls) {
  Hd3d_Log("=== room dump: room %d (built for %d), layer %s, %d objects", dungeon_room_index, g_room_id,
           link_is_on_lower_level ? "lower" : "upper", g_n_room_objs);
  RoomDumpAroundLink(cls);
  Hd3d_Log("  objects (tiles: number, class letter, %% of seed votes agreeing; ! = from hd3d_tiles.txt):");
  RoomDumpObjects();
  // object tiles that the floor flood flattened completely
  int flat_cells = 0;
  for (int c = 0; c < 64 * 64; c++) {
    if (!g_partial[c])
      continue;
    int raised = 0, covered = 0;
    for (int p = 0; p < 64; p++) {
      int i = ((c >> 6) * 8 + (p >> 3)) * 512 + (c & 63) * 8 + (p & 7);
      raised += g_praise[i] != 0, covered += g_pcover[i] != 0;
    }
    if (!raised && !covered) {   // (covered: the drawing above a box, on purpose)
      if (flat_cells++ < 40)
        Hd3d_Log("  flattened by the floor flood: cell (%d,%d) tile %03x (%s) -> add \"%03x %s full\" to hd3d_tiles.txt "
                 "if it should stand", c & 63, c >> 6, (g_hlayer[c] ? dung_bg1 : dung_bg2)[c] & 0x3ff, kClsName[cls[c]],
                 (g_hlayer[c] ? dung_bg1 : dung_bg2)[c] & 0x3ff, kClsName[cls[c]]);
    }
  }
  Hd3d_Log("  %d object cells flattened by the floor flood", flat_cells);
  Hd3d_Log("  class map, then heights (64x64 tiles of 8 px; . floor W wall C ceiling D door r rail f furniture "
           "o small b block T tall t torch P big pot h chair; w = wall tile that is walkable for the game; heights in 8 px):");
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
  // to fix a tile: copy its number to hd3d_tiles.txt with the right class
  for (int l = 0; l < 2; l++) {
    const uint16 *bg = l ? dung_bg1 : dung_bg2;
    Hd3d_Log("  %s tile numbers, each followed by its class letter (from hd3d_tiles.txt or the seed, ? = never seen):",
             l ? "BG1" : "BG2");
    for (int y = 0; y < 64; y++) {
      char row[64 * 4 + 1];
      for (int x = 0; x < 64; x++) {
        int t = bg[y * 64 + x] & 0x3ff;
        int k = g_tile_override[t] ? g_tile_override[t] : g_tile_cls[t];
        snprintf(row + x * 4, 5, "%03x%c", t, k ? kClsLetter[k - 1] : '?');
      }
      Hd3d_Log("  %2d %s", y, row);
    }
  }
}

// cgram index of room pixel (wx, wy) as shown: BG1 over BG2, or in rooms
// with BG1 on the sub screen only, BG1 through BG2's see-through pixels
static int RoomPixel(Ppu *ppu, int wx, int wy) {
  int c = ((wy >> 3) & 63) * 64 + ((wx >> 3) & 63);
  int col = wx & 7, row = wy & 7;
  if (!(ppu->screenEnabled[0] & 1)) {
    int p = TileWordPixel(ppu, 1, dung_bg2[c], col, row);
    return p || !(ppu->screenEnabled[1] & 1) ? p : TileWordPixel(ppu, 0, dung_bg1[c], col, row);
  }
  int p = TileWordPixel(ppu, 0, dung_bg1[c], col, row);
  return p ? p : TileWordPixel(ppu, 1, dung_bg2[c], col, row);
}

static bool IsMaskedClass(int c) {
  return c >= kCls_Rail;   // rails, furniture, pots, blocks, statues, torches
}

// Object tiles are drawn with a bit of floor around the object (behind a
// pillar, the corners of a pot), and the whole 8x8 tile stood up with it.
// As when reading a sprite's silhouette, the floor's colors are flooded from
// the real floor into the object tiles: what the flood reaches stays flat,
// the object (its outline, and everything the outline encloses) stands up.
//
// Then each pixel column of the object is split the 3/4 way: its bottom
// `h` rows are the face, painted standing up, and the rows above them are
// the top seen from above. Only the footprint (the column minus the face
// rows) is raised, so the top shows the object's top and not the floor
// north of it; a column too short for both keeps a 4 pixel deep footprint
// and a lower face.
// collision of a solid object (pots, columns, statues): anything but the
// floor, water, holes, stairs and ledges
static bool IsFootAttr(uint8 a) {
  return a != 0x00 && a != 0x08 && a != 0x09 && !(a >= 0x1c && a <= 0x1f) && !(a >= 0x28 && a <= 0x2b);
}

static void BuildPixelMasks(Ppu *ppu, const uint8 *cls, const uint8 *attr) {
  uint32 hash = 2166136261u;
  for (int i = 0; i < 64 * 64; i++)
    hash = (hash ^ dung_bg2[i] ^ (uint32)dung_bg1[i] << 16 ^ (uint32)g_hmap[i] << 8 ^ (uint32)g_hfloor[i] << 24) * 16777619u;
  hash ^= ppu->screenEnabled[0] & 1;
  // built when the room's tiles change, and again half a second later: the
  // room's graphics may still be loading on the first frames (video memory
  // itself is not hashed, animated tiles change it all the time)
  static int again;
  if (hash != g_mask_hash)
    g_mask_hash = hash, again = 30;
  else if (!again || --again)
    return;

  memset(g_partial, 0, sizeof(g_partial));
  memset(g_pcover, 0, sizeof(g_pcover));
  int partial = 0;
  for (int c = 0; c < 64 * 64; c++) {
    int t = (g_hlayer[c] ? dung_bg1 : dung_bg2)[c] & 0x3ff;
    if (g_hmap[c] > g_hfloor[c] && IsMaskedClass(cls[c]) && !g_tile_full[t])
      g_partial[c] = 1, partial++;
  }
  if (!partial)
    return;
  // floor colors: at least 0.5% of the floor's pixels. Only the floor
  // patterns count (tiles repeated over at least 12 flat cells), not the
  // flat things that are drawn once or twice (unknown objects, stairs,
  // doors): their colors made the flood eat the columns
  static uint16 uses[0x400];
  memset(uses, 0, sizeof(uses));
  for (int c = 0; c < 64 * 64; c++)
    if (g_hmap[c] == g_hfloor[c])
      uses[(g_hlayer[c] ? dung_bg1 : dung_bg2)[c] & 0x3ff]++;
  static uint32 count[128];
  memset(count, 0, sizeof(count));
  uint32 total = 0;
  for (int c = 0; c < 64 * 64; c++) {
    if (g_hmap[c] != g_hfloor[c] || uses[(g_hlayer[c] ? dung_bg1 : dung_bg2)[c] & 0x3ff] < 12)
      continue;
    int wx = (c & 63) * 8, wy = (c >> 6) * 8;
    for (int k = 0; k < 64; k++)
      count[RoomPixel(ppu, wx + (k & 7), wy + (k >> 3)) & 127]++, total++;
  }
  bool floor_color[128];
  for (int i = 0; i < 128; i++)
    floor_color[i] = total && count[i] * 200 >= total;
  floor_color[0] = true;   // see-through: the backdrop

  static uint32 queue[512 * 512];
  int qn = 0;
  for (int c = 0; c < 64 * 64; c++) {
    if (!g_partial[c])
      continue;
    int wx = (c & 63) * 8, wy = (c >> 6) * 8;
    for (int k = 0; k < 64; k++) {
      int x = wx + (k & 7), y = wy + (k >> 3), i = y * 512 + x;
      g_praise[i] = (uint8)(g_hmap[c] - g_hfloor[c]);   // above its floor
      g_pbox[i] = 0;
      g_pcol[i] = (uint8)RoomPixel(ppu, x, y);
    }
  }
  // seeds: floor-colored object pixels next to the flat floor
  static const int kDx[4] = { 1, -1, 0, 0 }, kDy[4] = { 0, 0, 1, -1 };
  for (int c = 0; c < 64 * 64; c++) {
    if (!g_partial[c])
      continue;
    int wx = (c & 63) * 8, wy = (c >> 6) * 8;
    for (int k = 0; k < 64; k++) {
      int x = wx + (k & 7), y = wy + (k >> 3), i = y * 512 + x;
      if (!floor_color[g_pcol[i] & 127])
        continue;
      for (int d = 0; d < 4; d++) {
        int nx = (x + kDx[d]) & 511, ny = (y + kDy[d]) & 511;
        int nc = (ny >> 3) * 64 + (nx >> 3);
        if (!g_partial[nc] && g_hmap[nc] == g_hfloor[nc]) {
          g_praise[i] = 0, queue[qn++] = (uint32)i;
          break;
        }
      }
    }
  }
  for (int q = 0; q < qn; q++) {
    int x = queue[q] & 511, y = queue[q] >> 9;
    for (int d = 0; d < 4; d++) {
      int nx = (x + kDx[d]) & 511, ny = (y + kDy[d]) & 511, ni = ny * 512 + nx;
      if (g_partial[(ny >> 3) * 64 + (nx >> 3)] && g_praise[ni] && floor_color[g_pcol[ni] & 127])
        g_praise[ni] = 0, queue[qn++] = (uint32)ni;
    }
  }
  // Objects with solid collision stand up as a box at the front of their
  // collision tiles, as deep as the object is wide (a pot 32x32, a column
  // 16x16): the collision runs further north than the object is deep. The
  // drawing above the box's front is its art: the lower part stands up as
  // the face, the rest lies on the top (see ShadeWall). The rest of the
  // drawing (above the box, a shadow below it) would lie flat on the floor
  // as a copy: it is covered with the floor beside the object (g_pcover).
  typedef struct BoxRun { uint8 cx, y0, y1, f0, f1, k; } BoxRun;   // cells y0..y1-1, collision f0..f1
  static BoxRun runs[kMaxBoxes];
  int nruns = 0;
  for (int cx = 0; cx < 64; cx++) {
    int cy = 0;
    while (cy < 64) {
      int c = cy * 64 + cx;
      if (!g_partial[c]) {
        cy++;
        continue;
      }
      int y0 = cy, k = cls[c];
      while (cy < 64 && g_partial[cy * 64 + cx] && cls[cy * 64 + cx] == k)
        cy++;
      int f0 = -1, f1 = -1;
      for (int y = y0; y < cy; y++)
        if (IsFootAttr(attr[y * 64 + cx])) {
          if (f0 < 0) f0 = y;
          f1 = y;
        }
      if (f1 >= 0 && nruns < kMaxBoxes)
        runs[nruns++] = (BoxRun){ (uint8)cx, (uint8)y0, (uint8)cy, (uint8)f0, (uint8)f1, (uint8)k };
    }
  }
  for (int n = 0; n < nruns; n++) {
    const BoxRun *b = &runs[n];
    // width: the neighbouring box columns with the same front and class
    int cx0 = b->cx, cx1 = b->cx;
    for (int dir = -1; dir <= 1; dir += 2) {
      for (int cx = b->cx + dir; cx >= 0 && cx < 64; cx += dir) {
        bool found = false;
        for (int m = 0; m < nruns && !found; m++)
          found = runs[m].cx == cx && runs[m].f1 == b->f1 && runs[m].k == b->k;
        if (!found) break;
        if (dir < 0) cx0 = cx; else cx1 = cx;
      }
    }
    int w = cx1 - cx0 + 1;
    // depth: what the drawing leaves after the face (a long barrier is long),
    // at least the width (a column is as deep as it is wide), at most the
    // collision (a pot's drawing is taller than it is deep)
    int front = (b->f1 + 1) * 8;
    int hmax = g_hmap[b->f1 * 64 + b->cx] - g_hfloor[b->f1 * 64 + b->cx];
    int depth = front - b->y0 * 8 - hmax;
    if (depth < w * 8) depth = w * 8;
    if (depth > front - b->f0 * 8) depth = front - b->f0 * 8;
    int foot_top = front - depth;
    if (foot_top < b->y0 * 8) foot_top = b->y0 * 8, depth = front - foot_top;
    // The drawing is every row of the run's cells. Not from its first object
    // pixel: the flood eats the top of a column that shares the floor's
    // colors, and the few rows left were stretched over the whole height.
    // A chair: the front 55% of its depth is the seat, at 45% of the height;
    // the back behind it stands full height. Its tops weigh less: a throne's
    // drawing is mostly the seat front and the back's face (see ShadeWall)
    ObjBox *bx = &g_boxes[n];
    bool chair = b->k == kCls_Chair;
    bx->x0 = (int16)(cx0 * 8), bx->w = (int16)(w * 8), bx->depth = (int16)depth;
    bx->seat_d = chair ? (uint8)(depth * 55 / 100) : 0;
    bx->seat_h = chair ? (uint8)(hmax * 45 / 100) : 0;
    bx->kt = chair ? 0.35f : 1.0f;
    bx->k = hmax > 0 ? (front - b->y0 * 8) / (hmax + bx->kt * depth) : 1.0f;
    for (int px = b->cx * 8; px < b->cx * 8 + 8; px++) {
      for (int r = b->y0 * 8; r < b->y1 * 8; r++) {
        int i = r * 512 + px;
        bool in = r >= foot_top && r < front;
        int hgt = front - 1 - r < bx->seat_d ? bx->seat_h : hmax;
        g_praise[i] = in ? (uint8)hgt : 0;
        g_pdown[i] = in ? (uint8)(front - 1 - r) : 0;
        g_pbox[i] = in ? (uint16)(n + 1) : 0;
        if (in)
          continue;
        // cover: the floor 16, 32... pixels to the side (floor patterns
        // repeat every 16), on whichever side reaches plain floor first
        for (int k = 1; k <= 6 && !g_pcover[i]; k++) {
          for (int dir = -1; dir <= 1 && !g_pcover[i]; dir += 2) {
            int sx = (px + dir * 16 * k) & 511, sc = (r >> 3) * 64 + (sx >> 3);
            if (!g_partial[sc] && g_hmap[sc] == g_hfloor[sc])
              g_pcover[i] = (int8)(dir * k);
          }
        }
      }
    }
  }
  int boxes = nruns;
  // the others: 3/4 split of each column of object pixels
  int raised = 0;
  for (int x = 0; x < 512; x++) {
    int y = 0;
    while (y < 512) {
      int i = y * 512 + x;
      if (!g_partial[(y >> 3) * 64 + (x >> 3)] || !g_praise[i] || g_pbox[i]) {
        y++;
        continue;
      }
      int y0 = y, h = 0;
      while (y < 512 && g_partial[(y >> 3) * 64 + (x >> 3)] && g_praise[y * 512 + x] && !g_pbox[y * 512 + x]) {
        if (g_praise[y * 512 + x] > h) h = g_praise[y * 512 + x];
        y++;
      }
      int len = y - y0, depth = len - h;
      if (depth < 4) {
        depth = len < 4 ? len : 4;
        h = len - depth;
      }
      for (int r = y0; r < y; r++) {
        int ri = r * 512 + x;
        g_praise[ri] = r >= y - depth && h > 0 ? (uint8)h : 0;
        g_pdown[ri] = (uint8)(y - 1 - r);
        raised += g_praise[ri] != 0;
      }
    }
  }
  int colors = 0;
  for (int i = 1; i < 128; i++)
    colors += floor_color[i];
  Hd3d_Log("object masks (room %d): %d cells, %d box columns, %d raised pixels (split), %d floor colors",
           dungeon_room_index, partial, boxes, raised, colors);
}

// Two floor levels, drawn two ways:
// - on two layers: the upper floor on BG2, the only layer on the main
//   screen, the lower one on BG1, seen through BG2's see-through tiles
//   (those cells have collision 0x1c in the BG2 table, their own collision
//   is in BG1's). A cell is lower where BG2 has a hole;
// - on one layer (the castle lobby): the upper floor ends in a ledge
//   (collision 0x28, the lip you jump down from) above a cliff drawn as a
//   north wall. The upper floor is what is reached from the cells north of
//   the ledges without crossing a wall, a ledge or stairs.
// Either way the cliffs (lower walls just south of the upper floor) set the
// height: the upper floor is raised by the commonest cliff height and the
// cliffs are made exactly that tall. Stairs between the two are a ramp.
//
// When BG1 is not on the main screen it only shows through BG2's holes, so
// elsewhere only BG2 counts (BG1 there is hidden: walls under the floor).
static bool Bg2Only(Ppu *ppu) {
  return !(ppu->screenEnabled[0] & 1);
}

static void FindHoles(Ppu *ppu) {
  memset(g_lower, 0, sizeof(g_lower));
  if (!Bg2Only(ppu) || !(ppu->screenEnabled[1] & 1))
    return;
  for (int i = 0; i < 64 * 64; i++)
    g_lower[i] = dung_bg2_attr_table[i] == 0x1c;
}

static bool IsStairsAttr(uint8 a) {
  return a >= 0x1d && a <= 0x1f;
}

static bool IsLedgeAttr(uint8 a) {
  return a >= 0x28 && a <= 0x2b;
}

static void FindUpper(const uint8 *attr, const uint8 *cls) {
  memset(g_upper, 0, sizeof(g_upper));
  int holes = 0;
  for (int i = 0; i < 64 * 64; i++)
    holes += g_lower[i];
  if (holes) {
    for (int i = 0; i < 64 * 64; i++)
      g_upper[i] = !g_lower[i];
    g_two_levels = true;
    return;
  }
  // one layer: flood from the cells north of the ledges
  static uint16 queue[64 * 64];
  int qn = 0, walkable = 0;
  #define BARRIER(i) (IsWallClass(cls[i]) || IsWallAttr(attr[i]) || IsLedgeAttr(attr[i]) || IsStairsAttr(attr[i]))
  for (int i = 0; i < 64 * 64; i++) {
    walkable += !BARRIER(i);
    if (attr[i] == 0x28 && i >= 64 && !BARRIER(i - 64) && !g_upper[i - 64])
      g_upper[i - 64] = 1, queue[qn++] = (uint16)(i - 64);
  }
  for (int q = 0; q < qn; q++) {
    int x = queue[q] & 63, y = queue[q] >> 6;
    static const int kDx[4] = { 1, -1, 0, 0 }, kDy[4] = { 0, 0, 1, -1 };
    for (int d = 0; d < 4; d++) {
      int nx = x + kDx[d], ny = y + kDy[d];
      if (nx < 0 || nx > 63 || ny < 0 || ny > 63)
        continue;
      int n = ny * 64 + nx;
      if (!g_upper[n] && !BARRIER(n))
        g_upper[n] = 1, queue[qn++] = (uint16)n;
    }
  }
  #undef BARRIER
  // the flood leaked around the ledges (the "upper" floor is most of the
  // room) or found nothing worth raising: one level
  if (qn < 8 || qn * 10 > walkable * 7) {
    memset(g_upper, 0, sizeof(g_upper));
    qn = 0;
  }
  g_two_levels = qn > 0;
}

static int LevelHeight(const uint8 *cls) {
  int hist[9] = { 0 };
  for (int cx = 0; cx < 64; cx++) {
    for (int cy = 1; cy < 64; cy++) {
      int c = cy * 64 + cx;
      if (g_upper[c] || !g_upper[c - 64] || !IsWallClass(cls[c]) || cls[c] == kCls_Ceiling)
        continue;
      int len = 0;
      while (cy < 64 && !g_upper[cy * 64 + cx] && IsWallClass(cls[cy * 64 + cx]))
        len++, cy++;
      hist[len > 8 ? 8 : len]++;
    }
  }
  int best = 3;
  for (int k = 2; k <= 6; k++)
    if (hist[k] > hist[best]) best = k;
  return best * 8;
}

static void BuildHeightMap(void) {
  // the table of the layer Link is on, as the game's tile detection reads it
  const uint8 *link_attr = dung_bg2_attr_table + (link_is_on_lower_level ? 0x1000 : 0);
  if (!g_tiles_ready)
    SeedTileClasses();
  if (g_room_id != dungeon_room_index && main_module_index == 7 && submodule_index == 0)
    CaptureRoomObjects();
  bool objects = g_room_id == dungeon_room_index && g_n_room_objs > 0;
  Ppu *ppu = g_zenv.ppu;
  FindHoles(ppu);
  bool bg2_only = Bg2Only(ppu);
  // each cell's collision and class: of the layer it shows when BG1 is
  // only seen through BG2's holes, else of Link's layer and both layers
  uint8 attr[64 * 64];
  for (int i = 0; i < 64 * 64; i++)
    attr[i] = !bg2_only ? link_attr[i] : g_lower[i] ? dung_bg1_attr_table[i] : dung_bg2_attr_table[i];
  uint8 cls[64 * 64];
  int cells = 0;
  for (int i = 0; i < 64 * 64; i++) {
    cls[i] = (uint8)CellClass(i, objects, &g_hlayer[i], !bg2_only ? -1 : g_lower[i] ? 1 : 0);
    // a solid tile the seed calls floor, drawn here by furniture: its own
    // class (the thrones' tiles are floor in most other rooms, and the wall
    // rule below made them 32 pixel walls)
    if (cls[i] == kCls_Floor && objects && IsFootAttr(attr[i])) {
      for (int l = 0; l < 2; l++) {
        int o = g_tile_obj[l][i], t = (l ? dung_bg1 : dung_bg2)[i] & 0x3ff;
        if ((bg2_only && l != (g_lower[i] ? 1 : 0)) || !o || g_tile_override[t] ||
            !IsMaskedClass(g_room_objs[o - 1].cls))
          continue;
        cls[i] = g_room_objs[o - 1].cls, g_hlayer[i] = (uint8)l;
        break;
      }
    }
    // decorations drawn on a wall (shelves, the pot rack, bars...) are wall
    if (cls[i] == kCls_Floor && IsWallAttr(attr[i]))
      cls[i] = kCls_Wall;
    // door openings (collision 0x80-0x8f, 0xf0 and up): walked through, so
    // flat; the wall around them still stands
    if (IsWallClass(cls[i]) && (attr[i] >= 0x80 && attr[i] <= 0x8f || attr[i] >= 0xf0))
      cls[i] = kCls_Floor;
  }
  // Chest platforms (T1 0xdc, also the throne room's dais): the rectangle of
  // the tiles it drew is raised kPlatformH. Its rim and corner trims are its
  // edge, flat on top; its front row stays a wall (as tall as the raise, the
  // art standing up as its face); the stairs in it are a ramp (below)
  uint8 plat[64 * 64];
  memset(plat, 0, sizeof(plat));
  for (int o = 0; objects && o < g_n_room_objs; o++) {
    if (g_room_objs[o].code != (kHd3d_ObjSubtype1 | 0xdc))
      continue;
    int x0 = 64, y0 = 64, x1 = -1, y1 = -1;
    for (int i = 0; i < 64 * 64; i++) {
      if (g_tile_obj[0][i] != o + 1 && g_tile_obj[1][i] != o + 1)
        continue;
      int x = i & 63, y = i >> 6;
      x0 = IntMin(x0, x), x1 = IntMax(x1, x), y0 = IntMin(y0, y), y1 = IntMax(y1, y);
      if (cls[i] == kCls_Small)
        cls[i] = kCls_Floor;   // the rim, the trims and the steps
    }
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++)
        plat[y * 64 + x] = 1;
  }
  FindUpper(attr, cls);
  bool two = g_two_levels;
  g_level_h = two ? LevelHeight(cls) : 0;
  bool holes = false;
  for (int i = 0; i < 64 * 64 && !holes; i++)
    holes = g_lower[i];
  // Upper floor edges over a hole are drawn as walls (the ledge lips, 0x28-
  // 0x2b, and the solid corners between them): they are the upper floor's
  // rim, flat at its height, and the drop below them is its side
  if (holes) {
    uint8 rim[64 * 64];
    for (int i = 0; i < 64 * 64; i++) {
      rim[i] = 0;
      if (!g_upper[i] || !IsWallClass(cls[i]) || cls[i] == kCls_Ceiling)
        continue;
      int x = i & 63, y = i >> 6;
      bool ledge_n = false, lower_n = false;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int nx = x + dx, ny = y + dy;
          if ((dx | dy) == 0 || nx < 0 || nx > 63 || ny < 0 || ny > 63)
            continue;
          int n = ny * 64 + nx;
          lower_n |= g_lower[n];
          ledge_n |= (dx == 0 || dy == 0) && IsLedgeAttr(attr[n]);
        }
      rim[i] = IsLedgeAttr(attr[i]) || (IsWallAttr(attr[i]) && ledge_n && lower_n);
    }
    for (int i = 0; i < 64 * 64; i++)
      if (rim[i])
        cls[i] = kCls_Floor;
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
  memset(g_hbase, 0, sizeof(g_hbase));
  for (int cx = 0; cx < 64; cx++) {
    int cy = 0;
    while (cy < 64) {
      int c = cy * 64 + cx;
      if (wall[c]) {
        int y0 = cy;
        bool any_solid = false, touches_upper = false;
        while (cy < 64 && wall[cy * 64 + cx] && g_upper[cy * 64 + cx] == g_upper[c]) {
          any_solid |= IsWallAttr(attr[cy * 64 + cx]);
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
              int nx = cx + dx, ny = cy + dy;
              if (nx >= 0 && nx < 64 && ny >= 0 && ny < 64)
                touches_upper |= g_upper[ny * 64 + nx];
            }
          cy++;
        }
        if (!any_solid)
          continue;   // a doorway: walkable all the way
        bool floor_n = y0 > 0 && !solid[(y0 - 1) * 64 + cx];
        bool floor_s = cy < 64 && !solid[cy * 64 + cx];
        int hgt = (cy - y0) * 8;
        bool fold = true;
        if (two && !g_upper[c] && y0 > 0 && g_upper[(y0 - 1) * 64 + cx]) {
          hgt = g_level_h;                 // the cliff up to the upper floor
        } else if (holes && !g_upper[c] && touches_upper) {
          hgt = g_level_h, fold = false;   // the pit's other walls: up to the rim
        } else if (cy - y0 > 6) {
          hgt = 32, fold = false;          // east and west walls: long runs, seen from above
        } else if (floor_n && !floor_s) {
          hgt = 16, fold = false;          // south wall: we look at its top
        } else if (!floor_s) {
          if (hgt > 32) hgt = 32;          // side walls (run to the next wall)
        }
        if (hgt > kWallMax) hgt = kWallMax;
        for (int y = y0; y < cy; y++)
          g_hmap[y * 64 + cx] = (uint8)hgt, g_hfold[y * 64 + cx] = fold, g_hbase[y * 64 + cx] = (uint8)cy;
        cells += cy - y0;
        continue;
      }
      int k = cls[c];
      if (k != kCls_Floor && !IsWallClass(k)) {
        // an object: as tall as its class (or its depth, if smaller)
        int y0 = cy;
        while (cy < 64 && cls[cy * 64 + cx] == k && g_upper[cy * 64 + cx] == g_upper[c])
          cy++;
        int hgt = (cy - y0) * 8;
        if (hgt > kClsHeight[k]) hgt = kClsHeight[k];
        for (int y = y0; y < cy; y++)
          g_hmap[y * 64 + cx] = (uint8)hgt, g_hfold[y * 64 + cx] = 1, g_hbase[y * 64 + cx] = (uint8)cy;
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
  // the upper floor and everything on it; stairs down from it are a ramp
  uint8 floor_h[64 * 64];
  for (int i = 0; i < 64 * 64; i++)
    floor_h[i] = two && g_upper[i] ? (uint8)g_level_h : 0;
  if (two) {
    for (int cx = 0; cx < 64; cx++) {
      for (int cy = 1; cy < 64; cy++) {
        int c = cy * 64 + cx;
        if (g_upper[c] || !g_upper[c - 64] || !IsStairsAttr(attr[c]))
          continue;
        int y0 = cy, n = 0;
        while (cy < 64 && IsStairsAttr(attr[cy * 64 + cx]))
          n++, cy++;
        for (int k = 0; k < n; k++) {
          int r = (y0 + k) * 64 + cx;
          floor_h[r] = (uint8)(g_level_h * (n - k) / (n + 1));
          g_hmap[r] = 0;   // a step, not a wall
        }
      }
    }
  }
  // the platforms' raise; their stairs (collision 0x22) climb it as a ramp
  g_raised = two;
  for (int cx = 0; cx < 64; cx++) {
    for (int cy = 0; cy < 64; cy++) {
      int c = cy * 64 + cx;
      if (!plat[c] || wall[c])
        continue;   // the front row: its wall already reaches the top
      g_raised = true;
      if (attr[c] != 0x22) {
        floor_h[c] = (uint8)(floor_h[c] + kPlatformH);
        continue;
      }
      int y0 = cy, n = 0;
      while (cy < 64 && plat[cy * 64 + cx] && attr[cy * 64 + cx] == 0x22)
        n++, cy++;
      for (int k = 0; k < n; k++) {
        int r = (y0 + k) * 64 + cx;
        floor_h[r] = (uint8)(floor_h[r] + kPlatformH * (n - k) / (n + 1));
        g_hmap[r] = 0;
      }
      cy--;
    }
  }
  // Bridges: a narrow strip of upper floor (10 tiles at most) with holes at
  // both ends, across or along, and the lower floor walkable under it (BG1's
  // collision): an 8 pixel deck, open below, so the lower level passes
  // under it. Cells with walls under them stay solid: the supports.
  memset(g_hbot, 0, sizeof(g_hbot));

  if (holes && g_level_h > 8) {
    for (int i = 0; i < 64 * 64; i++) {
      uint8 below = dung_bg1_attr_table[i];
      if (!g_upper[i] || !(below == 0x00 || below == 0x09))
        continue;
      int x = i & 63, y = i >> 6;
      bool bridge = false;
      for (int axis = 0; axis < 2 && !bridge; axis++) {
        int dx = axis ? 1 : 0, dy = axis ? 0 : 1, len = 1;
        bool ends = true;
        for (int dir = -1; dir <= 1; dir += 2) {
          int nx = x + dir * dx, ny = y + dir * dy;
          while (nx >= 0 && nx < 64 && ny >= 0 && ny < 64 && g_upper[ny * 64 + nx])
            nx += dir * dx, ny += dir * dy, len++;
          ends &= nx >= 0 && nx < 64 && ny >= 0 && ny < 64 && g_lower[ny * 64 + nx];
        }
        bridge = ends && len <= 10;
      }
      if (bridge)
        g_hbot[i] = (uint8)(g_level_h - 8);
    }
  }
  g_hmax = 0;
  for (int i = 0; i < 64 * 64; i++) {
    int f = floor_h[i];
    g_hfloor[i] = (uint8)f;
    if (f) {
      if (!g_hmap[i])
        g_hfold[i] = 0;   // flat upper floor: its art is under it
      g_hmap[i] = (uint8)(g_hmap[i] + f > 255 ? 255 : g_hmap[i] + f);
    }
    if (g_hmap[i] > g_hmax) g_hmax = g_hmap[i];
  }
  g_stat_wall_cells += cells;
  BuildPixelMasks(ppu, cls, attr);
  if (g_room_dump_request) {
    g_room_dump_request = false;
    if (!objects)
      Hd3d_Log("room dump: no object info for room %d (built for %d)", dungeon_room_index, g_room_id);
    RoomDump(attr, cls);
  }
}

// ground color at frame coordinates (u, v): the PPU frame inside the SNES
// screen, past it the room tiles
static bool SampleSurface(const Ground *gr, float u, float v, uint32 *out) {
  int ui = (int)floorf(u), vi = (int)floorf(v);
  if (ui >= 0 && ui < g_w && vi >= 0 && vi < g_h) {
    *out = g_floor[vi * g_w + ui];
    return true;
  }
  return Ground_Sample(gr, ui, vi, out);   // (DrawTerrain counts these)
}

// --- ray casting the raised room ---
//
// Every screen pixel's ray walks the room's 8x8 cells (a grid traversal, like
// a voxel renderer) from where it comes down to the tallest wall, until it
// hits a raised surface or reaches the ground. Object cells are walked again
// pixel by pixel (g_praise). The face that was hit decides the art:
//   top     fold-up surfaces show the art `height` rows north (the painted
//           top), flat tops (south walls, ceilings) the art under them;
//   south   the painted face standing up: art z rows north of its base;
//   sides   the same face rows, darkened, the face's edge tile spread
//   north   across the depth (never the floor smeared along the ray).
// With the world curve on, heights are measured from the sphere under the
// ray, so walls stand on the curved floor.

enum { kFace_Top, kFace_South, kFace_North, kFace_East, kFace_West };

typedef struct WallHit {
  float t, z;          // ray parameter, height above the floor
  float hx, hy;        // room pixel coordinates of the hit
  int face, cell, pix; // pix: object pixel index, -1 = whole cell
} WallHit;

// height of the (curved) floor below frame point (x, y)
static inline float FloorZ(const Cam *cm, float x, float y) {
  if (cm->R == 0.0f)
    return 0.0f;
  float dx = x - cm->tx, dy = y - cm->ty, r2 = dx * dx + dy * dy, R2 = cm->R * cm->R;
  return r2 >= R2 ? -1e9f : sqrtf(R2 - r2) - cm->R;
}

static inline float RayZ(const Cam *cm, const float *d, float t) {
  return cm->oz + t * d[2] - FloorZ(cm, cm->ox + t * d[0], cm->oy + t * d[1]);
}

// one surface of height H between t0 and t1, entered through face `entered`:
// false = the ray passes over it
// B: underside of a bridge deck (rays passing below it go on), 0 = solid
// The ray's height at the last t asked: each cell starts where the one
// before it ended, so a walk takes one RayZ (a square root with the curve
// on) per cell instead of two
typedef struct RayZCache { float t, z; } RayZCache;

static inline float RayZAt(const Cam *cm, const float *d, float t, RayZCache *zc) {
  if (t != zc->t)
    zc->t = t, zc->z = RayZ(cm, d, t);
  return zc->z;
}

static inline bool HitBox(const Cam *cm, const float *d, float t0, float t1, float H, float B, int entered,
                          RayZCache *zc, WallHit *hit) {
  float z0 = RayZAt(cm, d, t0, zc);
  if (z0 < B)
    return false;   // under the deck (rays only go down)
  if (z0 < H) {
    hit->t = t0, hit->z = z0, hit->face = entered;
    return true;
  }
  float z1 = RayZAt(cm, d, t1, zc);
  if (z1 >= H)
    return false;
  hit->t = t0 + (z0 - H) / (z0 - z1) * (t1 - t0);
  hit->z = H, hit->face = kFace_Top;
  return true;
}

// the object pixels of cell (cx, cy) between t0 and t1
static bool CastPixels(const Cam *cm, const float *d, float px0, float py0, float t0, float t1, int cx, int cy,
                       int entered, RayZCache *zc, WallHit *hit) {
  int floor_h = g_hfloor[(cy & 63) * 64 + (cx & 63)];
  float x = px0 + t0 * d[0], y = py0 + t0 * d[1];
  int x0 = cx * 8, y0 = cy * 8;
  int ix = (int)floorf(x), iy = (int)floorf(y);
  ix = ix < x0 ? x0 : ix > x0 + 7 ? x0 + 7 : ix;
  iy = iy < y0 ? y0 : iy > y0 + 7 ? y0 + 7 : iy;
  int sx = d[0] > 0 ? 1 : -1, sy = d[1] > 0 ? 1 : -1;
  float adx = fabsf(d[0]), ady = fabsf(d[1]);
  float tdx = adx > 1e-9f ? 1.0f / adx : 1e30f, tdy = ady > 1e-9f ? 1.0f / ady : 1e30f;
  float nx = adx > 1e-9f ? t0 + (sx > 0 ? ix + 1 - x : x - ix) / adx : 1e30f;
  float ny = ady > 1e-9f ? t0 + (sy > 0 ? iy + 1 - y : y - iy) / ady : 1e30f;
  float t = t0;
  for (;;) {
    float te = nx < ny ? nx : ny;
    if (te > t1) te = t1;
    int i = (iy & 511) * 512 + (ix & 511);
    int H = floor_h + g_praise[i];
    if (H && HitBox(cm, d, t, te, (float)H, g_hbot[(cy & 63) * 64 + (cx & 63)], entered, zc, hit)) {
      hit->pix = i;
      return true;
    }
    if (te >= t1)
      return false;
    if (nx < ny) {
      ix += sx, t = nx, nx += tdx;
      entered = sx > 0 ? kFace_West : kFace_East;
      if (ix < x0 || ix > x0 + 7) return false;
    } else {
      iy += sy, t = ny, ny += tdy;
      entered = sy > 0 ? kFace_North : kFace_South;
      if (iy < y0 || iy > y0 + 7) return false;
    }
  }
}

static bool CastWalls(const Cam *cm, const Ground *gr, const float *d, float t_end, WallHit *hit) {
  if (d[2] >= 0)
    return false;
  float t = cm->oz > g_hmax ? (g_hmax - cm->oz) / d[2] : 0.0f;
  if (t >= t_end)
    return false;
  // room pixel coordinates along the ray: (px0, py0) + t * d
  float px0 = gr->org_x + cm->ox, py0 = gr->org_y + cm->oy;
  float x = px0 + t * d[0], y = py0 + t * d[1];
  int cx = (int)floorf(x * 0.125f), cy = (int)floorf(y * 0.125f);
  int sx = d[0] > 0 ? 1 : -1, sy = d[1] > 0 ? 1 : -1;
  float adx = fabsf(d[0]), ady = fabsf(d[1]);
  float tdx = adx > 1e-9f ? 8.0f / adx : 1e30f, tdy = ady > 1e-9f ? 8.0f / ady : 1e30f;
  float nx = adx > 1e-9f ? t + (sx > 0 ? (cx + 1) * 8 - x : x - cx * 8) / adx : 1e30f;
  float ny = ady > 1e-9f ? t + (sy > 0 ? (cy + 1) * 8 - y : y - cy * 8) / ady : 1e30f;
  int entered = kFace_Top;   // starting inside a cell: as if from above
  RayZCache zc = { -1.0f, 0.0f };
  for (;;) {
    float te = nx < ny ? nx : ny;
    if (te > t_end) te = t_end;
    int c = (cy & 63) * 64 + (cx & 63);
    if (g_hmap[c]) {
      bool got;
      if (g_partial[c]) {
        got = CastPixels(cm, d, px0, py0, t, te, cx, cy, entered, &zc, hit);
      } else {
        hit->pix = -1;
        got = HitBox(cm, d, t, te, g_hmap[c], g_hbot[c], entered, &zc, hit);
      }
      if (got) {
        hit->cell = c;
        hit->hx = px0 + hit->t * d[0], hit->hy = py0 + hit->t * d[1];
        return true;
      }
    }
    if (te >= t_end)
      return false;
    if (nx < ny) {
      cx += sx, t = nx, nx += tdx;
      entered = sx > 0 ? kFace_West : kFace_East;
    } else {
      cy += sy, t = ny, ny += tdy;
      entered = sy > 0 ? kFace_North : kFace_South;
    }
  }
}

// art for a hit, at frame coordinates (*fu, *fv); false = nothing there
static bool ShadeWall(const Ground *gr, const WallHit *h, uint32 *out, float *fu, float *fv) {
  // a point just inside the surface that was hit
  float ix = h->hx, iy = h->hy;
  switch (h->face) {
  case kFace_East: ix -= 0.5f; break;
  case kFace_West: ix += 0.5f; break;
  case kFace_South: iy -= 0.5f; break;
  case kFace_North: iy += 0.5f; break;
  }
  int c = h->cell;
  // heights above the cell's own floor (the upper level is raised whole)
  float f = g_hfloor[c];
  bool floor_pix = h->pix >= 0 && !g_praise[h->pix];
  float H = floor_pix ? 0 : h->pix >= 0 ? g_praise[h->pix] : g_hmap[c] - f;
  float z = h->z - f;
  z = z < 0 ? 0 : z > H ? H : z;
  float au = ix, av = iy;
  int shade = 256;
  if (floor_pix || H <= 0) {
    // the raised floor itself: its art under it, the edge rows down its sides
    switch (h->face) {
    case kFace_Top: break;
    case kFace_South: av = iy - fmodf(f - h->z > 0 ? f - h->z : 0, 8.0f), shade = 200; break;
    default: shade = 160; break;
    }
  } else if (h->pix >= 0 && g_pbox[h->pix]) {
    // object box (or chair): seen in 3/4 view, a surface point `d` pixels
    // behind the front and `z` up is drawn z + kt * d rows above the front,
    // scaled by k so the drawing's rows cover the whole box: the face is the
    // lower rows, the top the rows above; a chair's seat front, seat, back
    // and back top in turn
    const ObjBox *bx = &g_boxes[g_pbox[h->pix] - 1];
    float base = floorf(iy) + g_pdown[h->pix] + 1.0f;
    float d = g_pdown[h->pix] + 0.5f;
    // the sides and the back wear the front art at the same height (a
    // chair's back above its seat), wrapped around the box: each side and
    // the back show the whole front once, so the corners meet
    float p = z > bx->seat_h ? z + bx->kt * bx->seat_d : z;
    float face_v = base - 0.5f - p * bx->k;
    float lx = (float)(((h->pix & 511) - bx->x0) & 511) + (ix - floorf(ix));   // across the front
    float across = d * bx->w / bx->depth;
    switch (h->face) {
    case kFace_Top: av = base - (H + bx->kt * d) * bx->k; break;
    case kFace_South: av = base - 0.5f - (z + bx->kt * g_pdown[h->pix]) * bx->k; break;
    case kFace_East: au = ix - lx + bx->w - across, av = face_v, shade = 212; break;
    case kFace_West: au = ix - lx + across, av = face_v, shade = 196; break;
    case kFace_North: au = ix - lx + bx->w - lx, av = face_v, shade = 180; break;
    }
  } else if (h->pix >= 0) {
    // object pixel: its column's face stands at its front, g_pdown rows south
    float base = floorf(iy) + g_pdown[h->pix] + 1.0f;
    switch (h->face) {
    case kFace_Top: av = iy - H; break;
    case kFace_South: av = iy - z; break;
    case kFace_East: av = base - 0.5f - z, shade = 196; break;
    case kFace_West: av = base - 0.5f - z, shade = 176; break;
    case kFace_North: av = base - 0.5f - z, shade = 150; break;
    }
  } else if (g_hfold[c]) {
    int row = (int)floorf(iy * 0.125f);
    float base = (float)((row - (row & 63) + g_hbase[c]) * 8);
    float dd = base - iy;
    float k = dd > 0 ? fmodf(dd, 8.0f) : 0.0f;   // depth into the run, tiled per 8 pixels
    switch (h->face) {
    case kFace_Top: av = iy - H; break;
    case kFace_South: av = iy - z; break;
    case kFace_East: au = ix - k, av = base - 0.5f - z, shade = 196; break;
    case kFace_West: au = ix + k, av = base - 0.5f - z, shade = 176; break;
    case kFace_North: av = base - 0.5f - z, shade = 150; break;
    }
  } else {
    // flat top (south wall, ceiling): its south rows repeat down the face
    switch (h->face) {
    case kFace_Top: break;
    case kFace_South: av = iy - fmodf(f + H - h->z > 0 ? f + H - h->z : 0, 8.0f), shade = 200; break;
    default: shade = 160; break;
    }
  }
  *fu = au - gr->org_x, *fv = av - gr->org_y;
  if (!SampleSurface(gr, *fu, *fv, out))
    return false;
  if (shade < 256)
    *out = (*out & 0xff000000) | Lerp(*out, 0, 256 - shade);
  return true;
}

// Worker threads for the ray caster: every pixel is its own ray and only
// reads the room, so the rows are shared out (an atomic row counter) between
// this thread and one worker per extra core
typedef void RowFn(void *ctx, int y);
static struct {
  int n;               // workers, -1 = not started yet
  SDL_sem *go, *done;
  SDL_atomic_t next;
  int rows;
  RowFn *fn;
  void *ctx;
} g_pool = { -1 };

static void Pool_RunRows(void) {
  int y;
  while ((y = SDL_AtomicAdd(&g_pool.next, 1)) < g_pool.rows)
    g_pool.fn(g_pool.ctx, y);
}

static int SDLCALL Pool_Worker(void *arg) {
  (void)arg;
  for (;;) {
    SDL_SemWait(g_pool.go);
    Pool_RunRows();
    SDL_SemPost(g_pool.done);
  }
  return 0;
}

static void ParallelRows(int rows, RowFn *fn, void *ctx) {
  if (g_pool.n < 0) {
    g_pool.n = 0;
    int want = SDL_GetCPUCount() - 1;
    want = want < 0 ? 0 : want > 7 ? 7 : want;
    g_pool.go = SDL_CreateSemaphore(0), g_pool.done = SDL_CreateSemaphore(0);
    for (int i = 0; i < want && g_pool.go && g_pool.done; i++) {
      SDL_Thread *t = SDL_CreateThread(Pool_Worker, "hd3d_rays", NULL);
      if (!t)
        break;
      SDL_DetachThread(t);
      g_pool.n++;
    }
    Hd3d_Log("ray caster: %d worker threads", g_pool.n);
  }
  g_pool.rows = rows, g_pool.fn = fn, g_pool.ctx = ctx;
  SDL_AtomicSet(&g_pool.next, 0);
  for (int i = 0; i < g_pool.n; i++)
    SDL_SemPost(g_pool.go);
  Pool_RunRows();
  for (int i = 0; i < g_pool.n; i++)
    SDL_SemWait(g_pool.done);
}

typedef struct TerrainCtx {
  uint32 *out;
  int pitch_px;
  const Cam *cm;
  const Ground *gr;
  const Sky *sk;
  uint32 fog;
  SDL_atomic_t outside, missing;
} TerrainCtx;

static void DrawTerrainRow(void *ctx, int y) {
  TerrainCtx *tc = (TerrainCtx *)ctx;
  const Cam *cm = tc->cm;
  const Ground *gr = tc->gr;
  const Sky *sk = tc->sk;
  int w = g_w, h = g_h, outside = 0, missing = 0;
  uint32 fog = tc->fog, *out = tc->out;
  int pitch_px = tc->pitch_px;
  uint32 *dst = out + y * pitch_px;
  for (int x = 0; x < w; x++) {
    int i = y * w + x;
    float d[3], u, v, depth;
    Cam_Dir(cm, x + 0.5f, y + 0.5f, &d[0], &d[1], &d[2]);
    bool ground = Cam_Ray(cm, x + 0.5f, y + 0.5f, &u, &v, &depth) && depth < kFogEnd;
    WallHit hit;
    uint32 px;
    bool ok;
    if (CastWalls(cm, gr, d, (ground ? depth : kFogEnd) / cm->F, &hit)) {
      depth = hit.t * cm->F;
      ok = ShadeWall(gr, &hit, &px, &u, &v);
    } else if (ground) {
      int wx = gr->org_x + (int)floorf(u), wy = gr->org_y + (int)floorf(v);
      int pi = (wy & 511) * 512 + (wx & 511);
      if (g_partial[((wy >> 3) & 63) * 64 + ((wx >> 3) & 63)] && g_pcover[pi])
        u += g_pcover[pi] * 16;
      ok = SampleSurface(gr, u, v, &px);
    } else {
      dst[x] = Sky_Color(sk, cm, x + 0.5f, y + 0.5f, x, y);
      g_depth[i] = 1e9f, g_uv[i] = kUvNone, g_kind[i] = kKind_Sky;
      continue;
    }
    int su = (int)floorf(u), sv = (int)floorf(v);
    bool in_frame = su >= 0 && su < w && sv >= 0 && sv < h;
    outside += !in_frame;
    g_depth[i] = depth;
    g_uv[i] = PackUv(su, sv);
    if (!ok) {
      missing++;
      dst[x] = fog, g_kind[i] = kKind_Sky;
      continue;
    }
    g_kind[i] = in_frame ? kKind_Frame : kKind_Outside;
    if (depth > kFogStart) {
      int f = (int)((depth - kFogStart) * (256.0f / (kFogEnd - kFogStart)));
      px = (px & 0xff000000) | Lerp(px, fog, f > 256 ? 256 : f);
    }
    dst[x] = px;
  }
  SDL_AtomicAdd(&tc->outside, outside);
  SDL_AtomicAdd(&tc->missing, missing);
}

static void DrawTerrain(uint32 *out, int pitch_px, const Cam *cm, const Ground *gr, const Sky *sk) {
  TerrainCtx tc = { out, pitch_px, cm, gr, sk, sk->outdoors ? 3u << 24 | sk->horizon : 5u << 24 };
  SDL_AtomicSet(&tc.outside, 0);
  SDL_AtomicSet(&tc.missing, 0);
  ParallelRows(g_h, DrawTerrainRow, &tc);
  g_stat_outside += SDL_AtomicGet(&tc.outside), g_stat_missing += SDL_AtomicGet(&tc.missing);
}

// floor height under frame point (u, v) in the last 3D frame with walls
// (lower = on the lower level: under a bridge, not on it)
static float FloorUnder(float u, float v, bool lower) {
  if (!g_depth_valid || !g_raised)
    return 0.0f;
  int wx = g_org_x + (int)floorf(u), wy = g_org_y + (int)floorf(v);
  int c = ((wy >> 3) & 63) * 64 + ((wx >> 3) & 63);
  return g_hbot[c] && lower ? 0.0f : g_hfloor[c];
}

// whether a sprite group is on the lower level: Link by his layer, enemies
// by theirs (sprite_floor)
static bool GroupLower(int key) {
  if (key == 0) return link_is_on_lower_level != 0;
  if (key >= 1 && key <= 16) return sprite_floor[key - 1] != 0;
  if (key >= 160 && key < 176) return sprite_floor[key - 160] != 0;
  return false;
}

typedef struct Group {
  int key;
  float depth;         // of the feet from the camera, -1 = not drawn
  float ax, ay;        // feet: where the cutout stands on the ground (GroupAnchor)
  int x0, y0, x1, y1;
  bool far;            // drawn from the OAM table (pieces outside the SNES screen)
  uint8 solid_depth;   // furniture sprites: a box this deep (DrawSolidSprite), 0 = a cutout
  uint8 solid_top;     // and the drawing rows on its top
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

// Where a group stands. The bottom of its pixel box moved whenever a weapon
// or an attack pose stuck out below the body (a guard turning to Link and
// thrusting its sword down jumped toward the camera and back), so Link and
// each enemy slot stand on their game position instead: 16 pixels below
// the top of the body for enemies, 24 for Link (z left out: a jump lifts
// the picture off its feet). Kept within the box: never below its bottom
// (no rows cut off), and the box is used when the position is far from it.
static void GroupAnchor(Group *g, Ppu *ppu) {
  g->ax = (g->x0 + g->x1 + 1) * 0.5f, g->ay = (float)(g->y1 + 1);
  int key = g->key, wx, wy;
  if (key == 0) {
    wx = link_x_coord + 8, wy = link_y_coord + 24;
  } else {
    int slot = key >= 1 && key <= 16 ? key - 1 : key >= 160 && key < 176 ? key - 160 : -1;
    if (slot < 0 || !sprite_state[slot])
      return;
    wx = (sprite_x_hi[slot] << 8 | sprite_x_lo[slot]) + 8;
    wy = (sprite_y_hi[slot] << 8 | sprite_y_lo[slot]) + 16;
  }
  float fx = (int16)(wx - BG2HOFS_copy2) - ppu->objBiasX + ppu->extraLeftRight;
  float fy = (int16)(wy - BG2VOFS_copy2) - ppu->objBiasY;
  if (fy > g->y1 + 1) fy = (float)(g->y1 + 1);
  if (fy < g->y1 + 1 - 32 || fx < g->x0 - 16 || fx > g->x1 + 17)
    return;
  g->ax = fx, g->ay = fy;
}

// Furniture sprites (the throne room's mantle, statues, blocks): solid boxes
// fixed in the room, not cutouts turned to the camera. depth = pixels from
// the front to the back, top = drawing rows shown on the top (the rest stand
// up on the front, at one row per pixel)
static const struct { uint8 type, depth, top; } kSolidSprites[] = {
  { 0xee, 8, 4 },     // movable mantle (Hyrule Castle throne room)
  { 0x1c, 12, 6 },    // movable statue
  { 0x38, 12, 6 },    // eye statue
  { 0x57, 12, 6 },    // desert palace statue
  { 0x8a, 12, 6 },    // spike block
  { 0xf2, 12, 6 },    // medallion tablet
};

static int SolidSprite(int key) {
  int slot = key >= 1 && key <= 16 ? key - 1 : key >= 160 && key < 176 ? key - 160 : -1;
  if (slot < 0 || !sprite_state[slot])
    return -1;
  for (int i = 0; i < (int)(sizeof(kSolidSprites) / sizeof(kSolidSprites[0])); i++)
    if (kSolidSprites[i].type == sprite_type[slot])
      return i;
  return -1;
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

// a furniture group's pixel at frame (x, y), from its OAM entries
static bool SolidPixel(Ppu *ppu, const Group *g, const int *efx, const int *efy, const int *esz, const int *ecf,
                       int x, int y, uint32 *out) {
  for (int e = 0; e < g->n_ents; e++) {
    int i = g->ents[e], lx = x - efx[i], ly = y - efy[i];
    if ((unsigned)lx < (unsigned)esz[i] && (unsigned)ly < (unsigned)esz[i] && OamPixel(ppu, ecf[i], lx, ly, esz[i], out))
      return true;
  }
  return false;
}

// A furniture sprite as a box standing on the floor: its front on the
// drawing's bottom row, `depth` pixels deep, as tall as the drawing minus
// the rows on its top. The front shows the drawing upright, the top its top
// rows, the back the front mirrored, the sides the drawing's edge columns.
// Drawn by splatting points of the faces that look at the camera (a box's
// visible faces never cover each other), behind the walls by their depth.
static void DrawSolidSprite(uint32 *out, int pitch_px, const Cam *cm, Ppu *ppu, const Group *g, const int *efx,
                            const int *efy, const int *esz, const int *ecf) {
  int w = g_w, h = g_h;
  float X0 = (float)g->x0, X1 = (float)(g->x1 + 1), Yf = (float)(g->y1 + 1), D = g->solid_depth, Yb = Yf - D;
  int span = g->y1 - g->y0 + 1, top = g->solid_top < span ? g->solid_top : span - 1;
  float H = (float)(span - top);
  float fl = FloorUnder((X0 + X1) * 0.5f, Yf, GroupLower(g->key >> 8));
  // samples per drawing pixel: two per screen pixel at the nearest corner
  float scmax = 0.0f;
  for (int k = 0; k < 8; k++) {
    float sx, sy, sc;
    if (Cam_ToScreenH(cm, k & 1 ? X1 : X0, k & 2 ? Yb : Yf, fl + (k & 4 ? H : 0.0f), &sx, &sy, &sc) && sc > scmax)
      scmax = sc;
  }
  if (scmax <= 0.0f)
    return;
  float step = 0.5f / scmax;
  if (step > 1.0f) step = 1.0f;
  if (step < 0.125f) step = 0.125f;
  // the camera's side of each face (the curve bends the far ground away, but
  // a sprite is near Link, where the world is flat)
  bool south = cm->oy > Yf, north = cm->oy < Yb, west = cm->ox < X0, east = cm->ox > X1;
  // the drawing's edge columns, per row, for the sides
  int8 edge_l[256], edge_r[256];
  for (int r = 0; r < span && r < 256; r++) {
    uint32 c;
    edge_l[r] = edge_r[r] = -1;
    for (int k = 0; k < 4 && edge_l[r] < 0; k++)
      if (SolidPixel(ppu, g, efx, efy, esz, ecf, g->x0 + k, g->y0 + r, &c)) edge_l[r] = (int8)k;
    for (int k = 0; k < 4 && edge_r[r] < 0; k++)
      if (SolidPixel(ppu, g, efx, efy, esz, ecf, g->x1 - k, g->y0 + r, &c)) edge_r[r] = (int8)k;
  }
  for (int f = 0; f < 5; f++) {
    // face f: 0 top, 1 front, 2 back, 3 west, 4 east; (a, b) across and up
    // (the top: across and back)
    if ((f == 1 && !south) || (f == 2 && !north) || (f == 3 && !west) || (f == 4 && !east))
      continue;
    if (f == 0 && cm->oz <= fl + H)
      continue;
    float amax = f <= 2 ? X1 - X0 : D, bmax = f == 0 ? D : H;
    for (float b = step * 0.5f; b < bmax; b += step) {
      for (float a = step * 0.5f; a < amax; a += step) {
        float u, v, z;
        int col, row;
        if (f == 0) {
          u = X0 + a, v = Yf - b, z = H;
          col = g->x0 + (int)a, row = g->y0 + top - 1 - (int)(b * top / D);
        } else {
          z = b, row = g->y1 - (int)b;
          int r = row - g->y0;
          if (f == 1) u = X0 + a, v = Yf, col = g->x0 + (int)a;
          else if (f == 2) u = X1 - a, v = Yb, col = g->x0 + (int)a;   // seen from behind: mirrored
          else if (f == 3) u = X0, v = Yf - a, col = edge_l[r] < 0 ? -1 : g->x0 + edge_l[r];
          else u = X1, v = Yf - a, col = edge_r[r] < 0 ? -1 : g->x1 - edge_r[r];
          if (col < 0)
            continue;
        }
        float sx, sy, sc;
        if (!Cam_ToScreenH(cm, u, v, fl + z, &sx, &sy, &sc))
          continue;
        int px = (int)floorf(sx), py = (int)floorf(sy);
        if ((unsigned)px >= (unsigned)w || (unsigned)py >= (unsigned)h)
          continue;
        int p = py * w + px;
        if (g_depth_valid && g_depth[p] < cm->F / sc - 2.0f)
          continue;   // a wall in front
        uint32 c;
        if (!SolidPixel(ppu, g, efx, efy, esz, ecf, col, row, &c))
          continue;
        if (f >= 3)
          c = (c & 0xff000000) | Lerp(c, 0, 256 - (f == 3 ? 176 : 196));
        else if (f == 2)
          c = (c & 0xff000000) | Lerp(c, 0, 256 - 150);
        out[py * pitch_px + px] = c, g_uv[p] = PackUv(col, row), g_kind[p] = kKind_Sprite;
      }
    }
  }
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
    GroupAnchor(g, ppu);
    // furniture: its box from its OAM entries (pixels Link covers in the 2D
    // frame are still there), standing at the front, the drawing's bottom
    int s = SolidSprite(g->key);
    g->solid_depth = 0;
    if (s >= 0) {
      if (!g->far) {
        g->n_ents = 0;
        for (int e = 0; e < 128 && g->n_ents < (int)(sizeof(g->ents) / sizeof(g->ents[0])); e++)
          if (evis[e] && GroupKey(e) == g->key)
            g->ents[g->n_ents++] = (uint16)e;
      }
      if (g->n_ents) {
        g->x0 = g->y0 = 1 << 20, g->x1 = g->y1 = -(1 << 20);
        for (int e = 0; e < g->n_ents; e++) {
          int k = g->ents[e];
          g->x0 = IntMin(g->x0, efx[k]), g->x1 = IntMax(g->x1, efx[k] + esz[k] - 1);
          g->y0 = IntMin(g->y0, efy[k]), g->y1 = IntMax(g->y1, efy[k] + esz[k] - 1);
        }
        g->solid_depth = kSolidSprites[s].depth, g->solid_top = kSolidSprites[s].top;
        g->ax = (g->x0 + g->x1 + 1) * 0.5f, g->ay = (float)(g->y1 + 1);
        if (dump)
          Hd3d_Log("  group key=%d: furniture sprite 0x%02x, a box %dx%d, %d deep", g->key, kSolidSprites[s].type,
                   g->x1 - g->x0 + 1, g->y1 - g->y0 + 1 - g->solid_top, g->solid_depth);
      }
    }
    g->key = g->key << 8 | i;
    float sx, sy, sc;
    g->depth = Cam_ToScreenH(cm, g->ax, g->ay, FloorUnder(g->ax, g->ay, GroupLower(g->key >> 8)), &sx, &sy, &sc)
                   ? cm->F / sc : -1.0f;
  }
  qsort(groups, n, sizeof(Group), CompareGroups);

  for (int gi = 0; gi < n; gi++) {
    Group *g = &groups[gi];
    int id = (g->key & 0xff) + 1;
    float ax = g->ax, ay = g->ay;   // feet
    float sx, sy, sc;
    if (!Cam_ToScreenH(cm, ax, ay, FloorUnder(ax, ay, GroupLower(g->key >> 8)), &sx, &sy, &sc)) {
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
    if (g->solid_depth) {
      DrawSolidSprite(out, pitch_px, cm, ppu, g, efx, efy, esz, ecf);
      continue;
    }
    // behind a wall: the wall's pixels are nearer than the feet
    float zlimit = depth - 4.0f;
    float inv = 1.0f / sc;
    int dx0 = (int)floorf(sx + (g->x0 - ax) * sc), dx1 = (int)ceilf(sx + (g->x1 + 1 - ax) * sc);
    // rows below the feet (a weapon held down, the shadow) lie in front
    int dy0 = (int)floorf(sy + (g->y0 - ay) * sc), dy1 = (int)ceilf(sy + (g->y1 + 1 - ay) * sc);
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
  g_org_x = gr.org_x, g_org_y = gr.org_y;
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
    // aim at the floor he stands on (eased when he changes level)
    g_aim_floor += (FloorUnder(lx, ly + 4.0f, link_is_on_lower_level != 0) - g_aim_floor) * 0.15f;
    Cam_SetupThird(&cm, width, height, lx, ly, g_aim_floor);
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
