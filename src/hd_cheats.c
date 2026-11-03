// Trucchi scelti nel launcher, vedi hd_cheats.h
#include "hd_cheats.h"
#include "config.h"
#include "variables.h"

static bool g_kill_pending;
// valori a inizio fotogramma e modulo del gioco in quel momento
static uint8 g_magic, g_bombs, g_arrows, g_module;
static uint16 g_rupees;

// Si gioca: mondo esterno, dungeon, menu e dialoghi. Fuori di qui (titolo,
// scelta del file, caricamenti, morte) i valori cambiano davvero e non si
// toccano
static bool InGame(uint8 m) {
  return m == 7 || m == 9 || m == 14;
}

void Cheats_BeforeFrame(void) {
  g_module = main_module_index;
  g_magic = link_magic_power;
  g_bombs = link_item_bombs;
  g_arrows = link_num_arrows;
  g_rupees = link_rupees_goal;
}

static void Restore(uint8 *p, uint8 before) {
  if (*p < before)
    *p = before;
}

void Cheats_AfterFrame(void) {
  if (!InGame(main_module_index))
    g_kill_pending = false;
  // passa-muri spento: non resta acceso da un salvataggio rapido di prima
  if (!g_config.cheats.no_clip && cheatWalkThroughWalls)
    cheatWalkThroughWalls = 0;
  if (!InGame(g_module) || !InGame(main_module_index))
    return;
  if (g_config.cheats.infinite_magic)
    Restore(&link_magic_power, g_magic);
  if (g_config.cheats.infinite_bombs)
    Restore(&link_item_bombs, g_bombs);
  if (g_config.cheats.infinite_arrows)
    Restore(&link_num_arrows, g_arrows);
  if (g_config.cheats.infinite_rupees && link_rupees_goal < g_rupees)
    link_rupees_goal = g_rupees;
}

void Cheats_RequestKill(void) {
  if (g_config.cheats.kill_link && InGame(main_module_index))
    g_kill_pending = true;
}

bool Cheats_TakeKill(void) {
  if (!g_kill_pending || link_health_current == 0)
    return false;
  g_kill_pending = false;
  return true;
}

bool Cheats_InfiniteLife(void) {
  return g_config.cheats.infinite_life;
}

bool Cheats_OneHitKill(void) {
  return g_config.cheats.one_hit_kill;
}
