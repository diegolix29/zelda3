// Vibrazione del controller: quando e quanto (hd_rumble.h)
#include <SDL.h>
#include <stdlib.h>
#include "hd_rumble.h"
#include "hd_menu.h"
#include "config.h"
#include "variables.h"
#include "player.h"
#include "hd_fx.h"

// Forza dei due motori (pesante e leggero, 0-1), durata e pausa minima fra
// due vibrazioni dello stesso tipo (in ms). Forza piena solo per i momenti
// rari e grossi; quelli frequenti brevi e leggeri. La pausa fa si' che tre
// bombe insieme o una raffica di colpi diano una vibrazione sola
typedef struct RumbleKind {
  float low, high;
  int ms, pause_ms;
} RumbleKind;

static const RumbleKind kRumbleKinds[kRumble_Count] = {
  [kRumble_Explosion] = { 1.00f, 1.00f, 170, 250 },
  [kRumble_DashCrash] = { 1.00f, 0.80f, 170, 250 },
  [kRumble_HurtSmall] = { 0.30f, 0.40f,  90, 250 },
  [kRumble_HurtMedium] = { 0.60f, 0.60f, 140, 250 },
  [kRumble_HurtBig] = { 0.90f, 0.90f, 170, 250 },
  [kRumble_Death] = { 1.00f, 0.60f, 600, 1000 },
  [kRumble_Shake] = { 0.45f, 0.25f,  80, 0 },
  [kRumble_Shock] = { 0.70f, 1.00f, 400, 600 },
  [kRumble_Shield] = { 0.25f, 0.35f,  70, 250 },
  [kRumble_SpinReady] = { 0.30f, 0.50f,  80, 250 },
  [kRumble_LedgeLand] = { 0.25f, 0.20f,  80, 250 },
  [kRumble_FallLand] = { 0.70f, 0.50f, 170, 250 },
  [kRumble_Hookshot] = { 0.30f, 0.50f,  70, 250 },
  [kRumble_Hammer] = { 0.30f, 0.45f,  60, 150 },
  [kRumble_HeavyLift] = { 0.60f, 0.40f, 170, 250 },
  [kRumble_MenuConfirm] = { 0.60f, 0.60f, 170, 250 },
  [kRumble_MenuErase] = { 1.00f, 1.00f, 170, 250 },
  [kRumble_MenuError] = { 0.50f, 0.50f, 120, 250 },
};

static uint32 g_last_kind_time[kRumble_Count];
static uint32 g_active_end;   // fine della vibrazione in corso
static float g_active_force;  // e la sua forza

// Solo durante il gioco: non nel titolo, nella scelta del file e nella
// sequenza dimostrativa (tranne i momenti dei menu, kRumble_Menu*)
static bool InGame(void) {
  return HdFx_GameLoaded();
}

static void Request(int kind, float k) {
  if (!g_config.rumble || (unsigned)kind >= kRumble_Count || (kind < kRumble_MenuConfirm && !InGame()))
    return;
  const RumbleKind *r = &kRumbleKinds[kind];
  uint32 now = SDL_GetTicks();
  if (g_last_kind_time[kind] && now - g_last_kind_time[kind] < (uint32)r->pause_ms)
    return;
  float force = (r->low > r->high ? r->low : r->high) * k;
  // una vibrazione forte non viene coperta da una piu' debole
  if ((int32)(g_active_end - now) > 0 && force < g_active_force)
    return;
  g_last_kind_time[kind] = now ? now : 1;
  g_active_end = now + r->ms, g_active_force = force;
  Host_Rumble(r->low * k, r->high * k, r->ms);
}

void HdRumble(int kind) {
  Request(kind, 1.0f);
}

// fotogramma dell'ultima esplosione con la posizione (bombe, super bomba):
// il suono della stessa esplosione non ne aggiunge un'altra
static uint16 g_explosion_frame = 0xffff;

void HdRumble_At(int kind, uint16 x, uint16 y) {
  int dx = abs((int16)(x - (link_x_coord + 8))), dy = abs((int16)(y - (link_y_coord + 12)));
  int d = dx > dy ? dx : dy;
  float k = d <= 48 ? 1.0f : d >= 200 ? 0.35f : 1.0f - 0.65f * (d - 48) / 152.0f;
  if (kind == kRumble_Explosion)
    g_explosion_frame = frame_counter;
  Request(kind, k);
}

void HdRumble_Sound(uint8 sfx1) {
  // 0x0c: esplosione (bombe, super bomba, nemici e boss che esplodono)
  // le altre (nemici, boss): a forza media, senza la posizione
  if ((sfx1 & 0x3f) == 0x0c && g_explosion_frame != frame_counter)
    Request(kRumble_Explosion, 0.6f);
}

void HdRumble_Tick(void) {
  static uint8 last_state, last_module, last_hook;
  static int8 last_shake_x, last_shake_y;
  if (!g_config.rumble) {
    last_state = link_player_handler_state, last_module = main_module_index;
    return;
  }
  uint8 st = link_player_handler_state;
  // lo schermo trema: il gioco sposta BG1 avanti e indietro
  int8 sx = (int8)bg1_x_offset, sy = (int8)bg1_y_offset;
  if ((sx | sy) && (sx != last_shake_x || sy != last_shake_y))
    HdRumble(kRumble_Shake);
  last_shake_x = sx, last_shake_y = sy;
  if (st != last_state) {
    if (st == kPlayerState_Electrocution)
      HdRumble(kRumble_Shock);
    else if (st == 24)  // LinkState_HoldingBigRock
      HdRumble(kRumble_HeavyLift);
    // fine dei salti dai dislivelli (stati 11-16)
    if (last_state >= 11 && last_state <= 16 && (st == kPlayerState_Ground || st == kPlayerState_Swimming))
      HdRumble(kRumble_LedgeLand);
  }
  // caduta in un buco: arrivo al piano di sotto (modulo 17 -> 7)
  if (last_module == 17 && main_module_index == 7)
    HdRumble(kRumble_FallLand);
  if (related_to_hookshot && !last_hook)
    HdRumble(kRumble_Hookshot);
  last_hook = related_to_hookshot;
  last_state = st, last_module = main_module_index;
}
