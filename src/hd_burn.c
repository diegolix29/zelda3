// Incendio di cespugli ed erba nel mondo esterno: la Lanterna e la Bacchetta del
// Fuoco danno fuoco a cespugli ed erba alti. Ogni tile resta in fiamme per circa
// un secondo e intanto il fuoco passa, con un po' di ritardo casuale, a quelle
// vicine (anche in diagonale), cosi' un prato brucia in qualche secondo come in
// Four Swords Adventures. Quando si spegne il cespuglio si comporta come quello
// tagliato: sparisce, lascia i suoi resti e scopre cio' che nascondeva; il
// terreno resta annerito anche lasciando l'area e tornandoci, finche' il gioco
// non fa ricrescere il cespuglio. Le fiamme, il fumo e l'annerimento sono
// disegnati da hd_fx.c. Le fiamme feriscono Link e i nemici che le toccano.
#include <stdlib.h>
#include <string.h>
#include "hd_burn.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "features.h"
#include "overworld.h"
#include "sprite.h"
#include "misc.h"
#include "ancilla.h"
#include "hd_fx.h"

enum {
  kMaxPending = 128,
  kMaxBurning = 96,
  kMaxScorched = 2048,   // tile annerite ricordate (8 byte l'una)
  kSpreadAt = 20,        // frame in fiamme prima di passare il fuoco ai vicini...
  kSpreadAtRand = 14,    // ...piu' fino a questi, a caso
  kDelaySide = 2,        // ritardo per i vicini di lato (piu' fino a kDelayRand)
  kDelayDiagonal = 14,   // e per quelli in diagonale, che sono piu' lontani
  kDelayRand = 10,
  kHarmlessAt = kHdBurnTime - 16,  // nelle ultime fiammelle il fuoco non fa piu' male
  // Danno a Link: quello degli sprite di fuoco del gioco (fuoco di Kodongo,
  // barre di fuoco: classe 4 di kPlayerDamages), un cuore con ogni armatura
  kLinkDamage = 8,
  // Danno ai nemici: quello della Bacchetta del Fuoco (classe 11 di
  // kEnemyDamages), cosi' ogni nemico reagisce come al fuoco della bacchetta:
  // immune, ferito o in fiamme
  kFireRodAncilla = 2,
};

typedef struct Pending {
  uint16 x, y;
  uint8 timer;
} Pending;

static Pending g_pending[kMaxPending];
static int g_num_pending;
static HdBurnTile g_burning[kMaxBurning];
static uint8 g_spread_at[kMaxBurning];  // 0xff: non passa il fuoco ai vicini
static int g_num_burning;
static HdBurnTile g_scorched[kMaxScorched];
static int g_num_scorched;
static uint16 g_burn_area = 0xffff;
static uint8 g_sfx_timer;
static uint32 g_burn_rng = 0x2545f491;

// Numeri casuali propri: non toccano quelli del gioco
static int Rand(int n) {
  g_burn_rng = g_burn_rng * 1664525 + 1013904223;
  return (g_burn_rng >> 16) % n;
}

// Cespuglio o erba alta: mappa16 che resta dopo e tipo dei resti
static bool Burnable(uint16 map16, uint16 *repl, uint8 *what) {
  if (map16 == 0x36) {
    *repl = 0xdc7, *what = 2;  // cespuglio
  } else if (map16 == 0x72a) {
    *repl = 0xdc8, *what = 4;  // cespuglio del Mondo delle Tenebre
  } else if (map16 == 0x37e) {
    *repl = 0xdc5, *what = 3;  // erba alta
  } else {
    return false;
  }
  return true;
}

// Posizione della tile nella mappa dell'area, -1 se e' fuori dall'area
static int TilePos(uint16 x, uint16 y) {
  uint16 dx = x - overworld_offset_base_x * 8, dy = y - overworld_offset_base_y;
  if (dx > (overworld_offset_mask_x * 8 | 0xf) || dy > (overworld_offset_mask_y | 0xf))
    return -1;
  return (dy & overworld_offset_mask_y) * 8 + ((x >> 3) - overworld_offset_base_x & overworld_offset_mask_x);
}

static bool IsBurnable(uint16 x, uint16 y) {
  int pos = TilePos(x, y);
  uint16 repl;
  uint8 what;
  return pos >= 0 && Burnable(overworld_tileattr[pos >> 1], &repl, &what);
}

// Spegne l'incendio in corso (le tile annerite restano)
static void ClearAll(void) {
  g_num_pending = g_num_burning = 0;
}

static bool IsBurning(uint16 x, uint16 y) {
  for (int i = 0; i < g_num_burning; i++)
    if (g_burning[i].x == x && g_burning[i].y == y)
      return true;
  return false;
}

static void Queue(uint16 x, uint16 y, uint8 timer) {
  if (IsBurning(x, y))
    return;
  for (int i = 0; i < g_num_pending; i++)
    if (g_pending[i].x == x && g_pending[i].y == y)
      return;
  if (g_num_pending < kMaxPending)
    g_pending[g_num_pending++] = (Pending){ x, y, timer };
}

// La tile (gia' allineata a 16) prende fuoco
static bool StartBurning(uint16 x, uint16 y, bool spread) {
  if (!IsBurnable(x, y) || IsBurning(x, y) || g_num_burning >= kMaxBurning)
    return false;
  if (g_burn_area != overworld_area_index) {
    g_burn_area = overworld_area_index;
    ClearAll();
  }
  int i = g_num_burning++;
  g_burning[i] = (HdBurnTile){ x, y, 0, (uint8)Rand(256), (uint8)overworld_area_index };
  g_spread_at[i] = spread ? kSpreadAt + Rand(kSpreadAtRand) : 0xff;
  if (g_sfx_timer == 0) {
    sound_effect_1 = CalculateSfxPan(x) | 0x2a;
    g_sfx_timer = 12;
  }
  return true;
}

static void Spread(uint16 x, uint16 y) {
  for (int dy = -16; dy <= 16; dy += 16) {
    for (int dx = -16; dx <= 16; dx += 16) {
      if ((dx | dy) && IsBurnable(x + dx, y + dy))
        Queue(x + dx, y + dy, ((dx && dy) ? kDelayDiagonal : kDelaySide) + Rand(kDelayRand));
    }
  }
}

// Riquadro delle fiamme della tile (come quello di uno sprite)
static void FlameHitBox(SpriteHitBox *hb, uint16 x, uint16 y) {
  x += 2, y += 2;
  hb->r4_spr_xlo = x, hb->r10_spr_xhi = x >> 8;
  hb->r5_spr_ylo = y, hb->r11_spr_yhi = y >> 8;
  hb->r6_spr_xsize = hb->r7_spr_ysize = 12;
}

// Link tocca le fiamme: si ferisce e rimbalza indietro come toccando uno
// sprite di fuoco (Sprite_AttemptDamageToLinkPlusRecoil)
static void HurtLink(uint16 x, uint16 y) {
  if (countdown_for_blink | link_disable_sprite_damage)
    return;
  SpriteHitBox hb;
  Link_SetupHitBox(&hb);
  FlameHitBox(&hb, x, y);
  if (!CheckIfHitBoxesOverlap(&hb))
    return;
  int dx = link_x_coord + 8 - (x + 8), dy = link_y_coord + 12 - (y + 8);
  int m = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
  if (m == 0)
    dy = m = 1;
  link_incapacitated_timer = 19;
  link_actual_vel_x = dx * 24 / m;
  link_actual_vel_y = dy * 24 / m;
  g_ram[0xc7] = link_actual_vel_z = 12;
  link_z_coord = 0;
  link_auxiliary_state = 1;
  link_give_damage = kLinkDamage;
}

// I nemici nelle fiamme vengono colpiti come dal fuoco della Bacchetta
static void HurtSprites(uint16 x, uint16 y) {
  for (int j = 15; j >= 0; j--) {
    if (sprite_state[j] < 9 || sprite_ignore_projectile[j] || sprite_pause[j] || sprite_hit_timer[j])
      continue;
    SpriteHitBox hb;
    hb.r0_xlo = x + 2, hb.r8_xhi = (x + 2) >> 8;
    hb.r1_ylo = y + 2, hb.r9_yhi = (y + 2) >> 8;
    hb.r2 = hb.r3 = 12;
    Sprite_SetupHitBox(j, &hb);
    if (CheckIfHitBoxesOverlap(&hb))
      Ancilla_CheckDamageToSprite(j, kFireRodAncilla);
  }
}

// Il fuoco si spegne: resta il cespuglio bruciato
static void BurnOut(uint16 x, uint16 y) {
  int pos = TilePos(x, y);
  uint16 repl;
  uint8 what;
  if (pos < 0 || !Burnable(overworld_tileattr[pos >> 1], &repl, &what))
    return;
  uint16 secret = Overworld_RevealSecret(pos);
  if (secret != 0)
    repl = secret;
  overworld_tileattr[pos >> 1] = repl;
  Overworld_Memorize_Map16_Change(pos, repl);
  Overworld_DrawMap16(pos, repl);
  nmi_load_bg_from_vram = 1;
  Sprite_SpawnImmediatelySmashedTerrain(what, x, y);
  if (g_num_scorched == kMaxScorched)
    memmove(g_scorched, g_scorched + 1, --g_num_scorched * sizeof(g_scorched[0]));
  g_scorched[g_num_scorched++] = (HdBurnTile){ x, y, 0, (uint8)Rand(256), (uint8)overworld_area_index };
}

bool HdBurn_Ignite(uint16 x, uint16 y, bool spread) {
  // sotto la pioggia il fuoco non attecchisce
  if (player_is_indoors || !(enhanced_features0 & kFeatures0_SwordAndItems) || HdFx_IsRainingOutdoors())
    return false;
  return StartBurning(x & ~0xf, y & ~0xf, spread);
}

void HdBurn_Reset(void) {
  ClearAll();
  g_num_scorched = 0;
}

void HdBurn_Update(void) {
  if (player_is_indoors) {
    HdBurn_Reset();
    return;
  }
  // Il fuoco si ferma durante menu e transizioni
  if (main_module_index != 9 || submodule_index != 0)
    return;
  // Terreno annerito dell'area: si fa definitivo e sparisce solo se il
  // cespuglio e' ricresciuto (il gioco li fa ricrescere rientrando nell'area)
  int n = 0;
  for (int i = 0; i < g_num_scorched; i++) {
    HdBurnTile s = g_scorched[i];
    if (s.area == (uint8)overworld_area_index) {
      if (IsBurnable(s.x, s.y))
        continue;
      if (s.age < kHdScorchTime)
        s.age++;
    }
    g_scorched[n++] = s;
  }
  g_num_scorched = n;
  if (g_num_pending + g_num_burning == 0)
    return;
  // Il fuoco resta nell'area in cui e' nato
  if (g_burn_area != overworld_area_index) {
    ClearAll();
    return;
  }
  if (g_sfx_timer)
    g_sfx_timer--;

  // Tile in attesa: prendono fuoco quando scade il loro ritardo
  n = 0;
  Pending ready[kMaxPending];
  int num_ready = 0;
  for (int i = 0; i < g_num_pending; i++) {
    Pending p = g_pending[i];
    if (p.timer > 1) {
      p.timer--;
      g_pending[n++] = p;
    } else {
      ready[num_ready++] = p;
    }
  }
  g_num_pending = n;
  for (int i = 0; i < num_ready; i++)
    StartBurning(ready[i].x, ready[i].y, true);

  // Tile in fiamme: passano il fuoco ai vicini, poi si spengono
  n = 0;
  int num_out = 0;
  HdBurnTile out[kMaxBurning];
  int num_spread = 0;
  HdBurnTile spread[kMaxBurning];
  for (int i = 0; i < g_num_burning; i++) {
    HdBurnTile b = g_burning[i];
    uint8 spread_at = g_spread_at[i];
    if (!IsBurnable(b.x, b.y))
      continue;  // tagliato o sollevato mentre bruciava: le fiamme spariscono
    b.age++;
    if (b.age < kHarmlessAt) {
      HurtLink(b.x, b.y);
      HurtSprites(b.x, b.y);
    }
    if (b.age == spread_at)
      spread[num_spread++] = b;
    if (b.age >= kHdBurnTime) {
      out[num_out++] = b;
      continue;
    }
    g_spread_at[n] = spread_at;
    g_burning[n++] = b;
  }
  g_num_burning = n;
  for (int i = 0; i < num_spread; i++)
    Spread(spread[i].x, spread[i].y);
  for (int i = 0; i < num_out; i++)
    BurnOut(out[i].x, out[i].y);
}

int HdBurn_GetBurning(const HdBurnTile **tiles) {
  *tiles = g_burning;
  return (player_is_indoors || g_burn_area != overworld_area_index) ? 0 : g_num_burning;
}

int HdBurn_GetScorched(const HdBurnTile **tiles) {
  *tiles = g_scorched;
  return player_is_indoors ? 0 : g_num_scorched;
}
