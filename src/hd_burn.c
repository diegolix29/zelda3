// Fuoco e ghiaccio su cespugli ed erba nel mondo esterno ("Interazione
// bruciatura e congelamento").
// Fuoco: la Lanterna e la Bacchetta del Fuoco danno fuoco a cespugli ed erba
// alti. Ogni tile resta in fiamme per circa un secondo e intanto il fuoco
// passa, con un po' di ritardo casuale, a quelle vicine (anche in diagonale),
// cosi' un prato brucia in qualche secondo come in Four Swords Adventures. Il
// Medaglione della Fiamma brucia insieme tutto quello che si vede, senza
// propagarsi. Quando si spegne il cespuglio si comporta come quello tagliato:
// sparisce, lascia i suoi resti e scopre cio' che nascondeva; il terreno resta
// annerito anche lasciando l'area e tornandoci, finche' il gioco non fa
// ricrescere il cespuglio. Le fiamme feriscono Link e i nemici che le toccano.
// Ghiaccio: la Bacchetta del Ghiaccio congela i cespugli e l'erba sulla sua
// linea, il Medaglione dell'Etere tutti quelli che si vedono; il ghiaccio non
// si propaga. Il cespuglio congelato resta finche' non viene tagliato o
// sollevato: allora va in pezzi come un vaso, in blu. Il fuoco scioglie il
// ghiaccio (la tile non brucia) e il ghiaccio spegne il fuoco.
// Anche l'acqua (alta e bassa) si congela: una lastra per tile sotto il centro
// del colpo della bacchetta (una fila larga tre tile), con l'Etere tutta quella
// che si vede. Diventa ghiaccio scivoloso su cui si cammina
// (Overworld_GetTileAttributeAtLocation la fa 0x0f) e torna acqua dopo 7
// secondi (bacchetta) o 14 (Etere), o subito col fuoco. Cespugli ed erba
// congelati invece restano, come i nemici congelati del gioco.
// Fiamme, fumo, annerimento e ghiaccio sono disegnati da hd_fx.c.
#include <stdlib.h>
#include <string.h>
#include "hd_burn.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "features.h"
#include "overworld.h"
#include "sprite.h"
#include "misc.h"
#include "hd_world.h"
#include "hd_water.h"
#include "ancilla.h"
#include "hd_fx.h"
#include "snes/ppu.h"

extern int g_render_bias_x, g_render_bias_y;

enum {
  kMaxPending = 128,
  kMaxScorched = 2048,   // tile annerite ricordate (10 byte l'una)
  kMaxFrozen = 1024,     // tile congelate ricordate
  kMaxWet = 64,          // tile appena sciolte ricordate
  kWetTime = 120,        // frame in cui la tile appena sciolta non prende fuoco
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
static HdBurnTile g_burning[kHdMaxBurning];
static uint8 g_spread_at[kHdMaxBurning];  // 0xff: non passa il fuoco ai vicini
static int g_num_burning;
static HdBurnTile g_scorched[kMaxScorched];
static int g_num_scorched;
static HdBurnTile g_frozen[kMaxFrozen];
static int g_num_frozen;
// Indice veloce delle tile congelate (un bit per tile, a meno di 64x64 tile):
// HdBurn_IsFrozenAt si chiama per ogni cella d'acqua a ogni frame
static uint8 g_frozen_hint[64 * 64 / 8];
static uint32 g_frozen_ver = 1, g_hint_ver;
// Tile appena sciolte dal fuoco: per un po' non bruciano, altrimenti la
// fiammata (che controlla la stessa tile per piu' frame) o le fiamme vicine le
// incendierebbero subito dopo
static Pending g_wet[kMaxWet];
static int g_num_wet;
static uint16 g_burn_area = 0xffff;
static uint16 g_seen_area = 0xffff;  // per i segreti delle tile bruciate da lontano
static uint8 g_sfx_timer;
static uint32 g_burn_rng = 0x2545f491;

// Numeri casuali propri: non toccano quelli del gioco
static int Rand(int n) {
  g_burn_rng = g_burn_rng * 1664525 + 1013904223;
  return (g_burn_rng >> 16) % n;
}

static bool Enabled(void) {
  return !player_is_indoors && (enhanced_features0 & kFeatures0_BurnAndFreeze);
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

// Mondo unico: il fuoco passa da un'area all'altra e non si spegne quando
// Link cambia area
static bool Seamless(void) {
  return HdWorld_SeamlessOn();
}

// Mappa16 della tile, anche nelle aree vicine col mondo unico; -1 se non si sa
static int TileAt(uint16 x, uint16 y) {
  int pos = TilePos(x, y);
  if (pos >= 0)
    return overworld_tileattr[pos >> 1];
  return HdWorld_WorldTile(x, y);
}

// Area della tile (con il bit del mondo, come overworld_area_index)
static uint8 AreaOf(uint16 x, uint16 y) {
  int head = TilePos(x, y) < 0 ? HdWorld_AreaAt(x, y) : -1;
  return head >= 0 ? (uint8)head : (uint8)overworld_area_index;
}

static bool IsBurnable(uint16 x, uint16 y) {
  int t = TileAt(x, y);
  uint16 repl;
  uint8 what;
  return t >= 0 && Burnable(t, &repl, &what);
}

uint8 HdBurn_WaterCells(uint16 x, uint16 y) {
  int t = TileAt(x, y);
  if (t < 0 || HdWater_IsSky(x, y))  // cielo del Monte della Morte
    return 0;
  const uint16 *m8 = GetMap16toMap8Table() + t * 4;
  const uint8 *attr = GetMap8toTileAttr();
  uint8 mask = 0;
  for (int i = 0; i < 4; i++) {
    int e = m8[i], c = e & 0x3ff;
    // acqua alta o bassa, ma non le cascate (caselle 0x1ca-0x1cd, palette 7)
    if ((attr[e & 0x1ff] == 0x08 || attr[e & 0x1ff] == 0x09) && !((e >> 10 & 7) == 7 && c >= 0x1ca && c <= 0x1cd))
      mask |= 1 << i;
  }
  return mask;
}

bool HdBurn_TileShown(const HdBurnTile *t) {
  if (Seamless())
    return !((t->area ^ overworld_area_index) & 0x40);
  return t->area == (uint8)overworld_area_index;
}

// Spegne l'incendio in corso (le tile annerite restano)
static void ClearAll(void) {
  g_num_pending = g_num_burning = 0;
}

static int FindBurning(uint16 x, uint16 y) {
  for (int i = 0; i < g_num_burning; i++)
    if (g_burning[i].x == x && g_burning[i].y == y)
      return i;
  return -1;
}

static inline int HintBit(uint16 x, uint16 y) {
  return (x >> 4 & 63) | (y >> 4 & 63) << 6;
}

static int FindFrozen(uint16 x, uint16 y) {
  if (g_hint_ver != g_frozen_ver) {
    g_hint_ver = g_frozen_ver;
    memset(g_frozen_hint, 0, sizeof(g_frozen_hint));
    for (int i = 0; i < g_num_frozen; i++) {
      int b = HintBit(g_frozen[i].x, g_frozen[i].y);
      g_frozen_hint[b >> 3] |= 1 << (b & 7);
    }
  }
  int b = HintBit(x, y);
  if (!(g_frozen_hint[b >> 3] & 1 << (b & 7)))
    return -1;
  for (int i = 0; i < g_num_frozen; i++)
    if (g_frozen[i].x == x && g_frozen[i].y == y && g_frozen[i].area == AreaOf(x, y))
      return i;
  return -1;
}

static void RemoveFrozen(int i) {
  g_frozen[i] = g_frozen[--g_num_frozen];
  g_frozen_ver++;
}

static void AddFrozen(HdBurnTile t) {
  if (g_num_frozen == kMaxFrozen)
    memmove(g_frozen, g_frozen + 1, --g_num_frozen * sizeof(g_frozen[0]));
  g_frozen[g_num_frozen++] = t;
  g_frozen_ver++;
}

static bool IsWet(uint16 x, uint16 y) {
  for (int i = 0; i < g_num_wet; i++)
    if (g_wet[i].x == x && g_wet[i].y == y)
      return true;
  return false;
}

// Il fuoco scioglie il ghiaccio della tile: resta bagnata per kWetTime frame
static void Melt(int f) {
  HdBurnTile t = g_frozen[f];
  RemoveFrozen(f);
  if (g_num_wet == kMaxWet)
    memmove(g_wet, g_wet + 1, --g_num_wet * sizeof(g_wet[0]));
  g_wet[g_num_wet++] = (Pending){ t.x, t.y, kWetTime };
}

static void Queue(uint16 x, uint16 y, uint8 timer) {
  if (FindBurning(x, y) >= 0)
    return;
  for (int i = 0; i < g_num_pending; i++)
    if (g_pending[i].x == x && g_pending[i].y == y)
      return;
  if (g_num_pending < kMaxPending)
    g_pending[g_num_pending++] = (Pending){ x, y, timer };
}

// La tile (gia' allineata a 16) prende fuoco; se e' congelata il ghiaccio si
// scioglie e la tile non brucia (ne' subito dopo, finche' e' bagnata)
static bool StartBurning(uint16 x, uint16 y, bool spread) {
  if (!IsBurnable(x, y) || FindBurning(x, y) >= 0 || g_num_burning >= kHdMaxBurning || IsWet(x, y))
    return false;
  int f = FindFrozen(x, y);
  if (f >= 0) {
    Melt(f);
    return false;
  }
  // senza mondo unico il fuoco resta nell'area in cui e' nato; col mondo
  // unico si spegne solo passando all'altro mondo
  if (Seamless() ? ((g_burn_area ^ overworld_area_index) & 0x40) != 0 : g_burn_area != overworld_area_index)
    ClearAll();
  g_burn_area = overworld_area_index;
  int i = g_num_burning++;
  g_burning[i] = (HdBurnTile){ x, y, 0, (uint8)Rand(256), AreaOf(x, y) };
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
  if (pos < 0) {
    // tile di un'area vicina (mondo unico): cambia nel mondo persistente; il
    // segreto sotto il cespuglio si scopre quando Link entra nell'area
    int t = TileAt(x, y);
    if (t < 0 || !Burnable(t, &repl, &what) || !HdWorld_SetWorldTile(x, y, repl))
      return;
    BYTE(dung_secrets_unk1) = 0;
    Sprite_SpawnImmediatelySmashedTerrain(what, x, y);
    if (g_num_scorched == kMaxScorched)
      memmove(g_scorched, g_scorched + 1, --g_num_scorched * sizeof(g_scorched[0]));
    g_scorched[g_num_scorched++] = (HdBurnTile){ x, y, 0, (uint8)Rand(256), AreaOf(x, y), 1 };
    return;
  }
  if (!Burnable(overworld_tileattr[pos >> 1], &repl, &what))
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

// La tile (gia' allineata a 16) si congela; se bruciava il fuoco si spegne.
// Con water (kHdIceRod o kHdIceEther) anche (o solo, con only_water)
// l'acqua; il ghiaccio d'acqua gia' fatto torna nuovo se cosi' dura di piu'
static bool StartFreezing(uint16 x, uint16 y, uint8 water, bool only_water) {
  bool burnable = !only_water && IsBurnable(x, y);
  if (!burnable && !(water && HdBurn_WaterCells(x, y)))
    return false;
  int f = FindFrozen(x, y);
  if (f >= 0) {
    HdBurnTile *t = &g_frozen[f];
    if (!t->water || !water)
      return false;
    HdBurnTile n = *t;
    n.water = water, n.age = 8;
    if (HdBurn_IceLeft(&n) > HdBurn_IceLeft(t))
      *t = n;
    return true;
  }
  if (!burnable) {
    AddFrozen((HdBurnTile){ x, y, 0, (uint8)Rand(256), AreaOf(x, y), 0, water });
    return true;
  }
  int b = FindBurning(x, y);
  if (b >= 0) {
    g_num_burning--;
    g_burning[b] = g_burning[g_num_burning];
    g_spread_at[b] = g_spread_at[g_num_burning];
  }
  for (int i = 0; i < g_num_pending; i++) {
    if (g_pending[i].x == x && g_pending[i].y == y) {
      g_pending[i] = g_pending[--g_num_pending];
      break;
    }
  }
  AddFrozen((HdBurnTile){ x, y, 0, (uint8)Rand(256), AreaOf(x, y) });
  return true;
}

// Riquadro del mondo che si vede sullo schermo, tile 16x16 comprese
static void VisibleTiles(uint16 *x0, uint16 *y0, int *w, int *h) {
  Ppu *ppu = g_zenv.ppu;
  int ext = ppu->extraLeftRight;
  int rows = (ppu->renderFlags & kPpuRenderFlags_Height240) ? 240 : 224;
  *x0 = (BG2HOFS_copy2 + g_render_bias_x - ext) & ~0xf;
  *y0 = (BG2VOFS_copy2 + g_render_bias_y) & ~0xf;
  *w = 256 + 2 * ext + 16, *h = rows + 16;
}

// Il fuoco scioglie l'acqua ghiacciata della tile (gia' allineata a 16): si
// apre a chiazze come a fine tempo, ma in quanto dura una fiamma
static void MeltWater(uint16 x, uint16 y) {
  int f = FindFrozen(x, y);
  if (f < 0 || !g_frozen[f].water || g_frozen[f].water == kHdIceMelting)
    return;
  HdBurnTile n = g_frozen[f];
  n.water = kHdIceMelting, n.age = kHdMeltStart;
  // se stava gia' finendo da solo resta il tempo che gli mancava
  if (HdBurn_IceLeft(&n) < HdBurn_IceLeft(&g_frozen[f]))
    g_frozen[f] = n;
}

bool HdBurn_Ignite(uint16 x, uint16 y, bool spread) {
  if (!Enabled())
    return false;
  MeltWater(x & ~0xf, y & ~0xf);
  // sotto la pioggia il fuoco non attecchisce
  if (HdFx_IsRainingOutdoors())
    return false;
  return StartBurning(x & ~0xf, y & ~0xf, spread);
}

bool HdBurn_Freeze(uint16 x, uint16 y) {
  if (!Enabled())
    return false;
  return StartFreezing(x & ~0xf, y & ~0xf, 0, false);
}

void HdBurn_FreezeWater(uint16 x, uint16 y) {
  if (Enabled())
    StartFreezing(x & ~0xf, y & ~0xf, kHdIceRod, true);
}

void HdBurn_MeltWater(uint16 x, uint16 y) {
  if (Enabled())
    MeltWater(x & ~0xf, y & ~0xf);
}

void HdBurn_IgniteScreen(void) {
  if (!Enabled())
    return;
  uint16 x0, y0;
  int w, h;
  VisibleTiles(&x0, &y0, &w, &h);
  for (int y = 0; y < h; y += 16)
    for (int x = 0; x < w; x += 16)
      MeltWater(x0 + x, y0 + y);
  if (HdFx_IsRainingOutdoors())
    return;
  for (int y = 0; y < h; y += 16)
    for (int x = 0; x < w; x += 16)
      StartBurning(x0 + x, y0 + y, false);
}

void HdBurn_FreezeScreen(void) {
  if (!Enabled())
    return;
  uint16 x0, y0;
  int w, h;
  VisibleTiles(&x0, &y0, &w, &h);
  for (int y = 0; y < h; y += 16)
    for (int x = 0; x < w; x += 16)
      StartFreezing(x0 + x, y0 + y, kHdIceEther, false);
}

// Il cespuglio o l'erba appena tagliati o sollevati nel punto (x, y) (angolo
// dei resti) erano congelati? Se si' li dimentica: vanno in pezzi di ghiaccio
bool HdBurn_TakeFrozen(uint16 x, uint16 y) {
  if (player_is_indoors)
    return false;
  for (int t = 0; t < 2; t++) {
    uint16 tx = (x + t * 8) & ~0xf, ty = (y + t * 8) & ~0xf;
    int i = FindFrozen(tx, ty);
    if (i >= 0 && !g_frozen[i].water && !IsBurnable(tx, ty)) {
      RemoveFrozen(i);
      return true;
    }
  }
  return false;
}

bool HdBurn_IsFrozenAt(uint16 x, uint16 y) {
  if (player_is_indoors || g_num_frozen == 0)
    return false;
  x &= ~0xf, y &= ~0xf;
  int f = FindFrozen(x, y);
  return f >= 0 && (g_frozen[f].water || IsBurnable(x, y));
}

void HdBurn_Reset(void) {
  ClearAll();
  g_num_scorched = 0;
  g_num_frozen = 0;
  g_frozen_ver++;
  g_num_wet = 0;
}

// Link e' entrato in un'area dove il fuoco era arrivato da un'area vicina:
// sotto i cespugli bruciati compare quello che nascondevano (buche, scale),
// come quando bruciano nell'area di Link
static void RevealRemoteSecrets(void) {
  for (int i = 0; i < g_num_scorched; i++) {
    HdBurnTile *s = &g_scorched[i];
    if (!s->remote || s->area != (uint8)overworld_area_index)
      continue;
    s->remote = 0;
    int pos = TilePos(s->x, s->y);
    if (pos < 0 || IsBurnable(s->x, s->y))
      continue;
    uint16 secret = Overworld_RevealSecret(pos);
    BYTE(dung_secrets_unk1) = 0;
    if (secret == 0)
      continue;
    overworld_tileattr[pos >> 1] = secret;
    Overworld_Memorize_Map16_Change(pos, secret);
    Overworld_DrawMap16(pos, secret);
    nmi_load_bg_from_vram = 1;
  }
}

void HdBurn_Update(void) {
  if (player_is_indoors) {
    HdBurn_Reset();
    return;
  }
  // Il fuoco si ferma durante menu e transizioni
  if (main_module_index != 9 || submodule_index != 0)
    return;
  if (g_seen_area != overworld_area_index) {
    g_seen_area = overworld_area_index;
    RevealRemoteSecrets();
  }
  bool seamless = Seamless();
  // Terreno annerito dell'area: si fa definitivo e sparisce solo se il
  // cespuglio e' ricresciuto (il gioco li fa ricrescere rientrando nell'area)
  int n = 0;
  for (int i = 0; i < g_num_scorched; i++) {
    HdBurnTile s = g_scorched[i];
    if (s.area == (uint8)overworld_area_index || seamless && HdBurn_TileShown(&s)) {
      if (IsBurnable(s.x, s.y))
        continue;
      if (s.age < kHdScorchTime)
        s.age++;
    }
    g_scorched[n++] = s;
  }
  g_num_scorched = n;
  // Ghiaccio dell'area: sparisce se il cespuglio non c'e' piu'; quello
  // dell'acqua si scioglie da solo (HdBurn_IceLeft)
  n = 0;
  for (int i = 0; i < g_num_frozen; i++) {
    HdBurnTile s = g_frozen[i];
    if (s.area == (uint8)overworld_area_index || seamless && HdBurn_TileShown(&s)) {
      if (s.water ? HdBurn_IceLeft(&s) <= 0 || !HdBurn_WaterCells(s.x, s.y) : !IsBurnable(s.x, s.y))
        continue;
      if (s.age < 0xffff)
        s.age++;
    }
    g_frozen[n++] = s;
  }
  if (n != g_num_frozen)
    g_frozen_ver++;
  g_num_frozen = n;
  // Tile sciolte: si asciugano
  n = 0;
  for (int i = 0; i < g_num_wet; i++)
    if (--g_wet[i].timer)
      g_wet[n++] = g_wet[i];
  g_num_wet = n;
  if (g_num_pending + g_num_burning == 0)
    return;
  // Il fuoco resta nell'area in cui e' nato (col mondo unico: nel suo mondo)
  if (seamless ? ((g_burn_area ^ overworld_area_index) & 0x40) != 0 : g_burn_area != overworld_area_index) {
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
  static HdBurnTile out[kHdMaxBurning], spread[kHdMaxBurning];
  int num_spread = 0;
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
  if (player_is_indoors)
    return 0;
  if (Seamless())
    return ((g_burn_area ^ overworld_area_index) & 0x40) ? 0 : g_num_burning;
  return g_burn_area != overworld_area_index ? 0 : g_num_burning;
}

int HdBurn_GetScorched(const HdBurnTile **tiles) {
  *tiles = g_scorched;
  return player_is_indoors ? 0 : g_num_scorched;
}

int HdBurn_GetFrozen(const HdBurnTile **tiles) {
  *tiles = g_frozen;
  return player_is_indoors ? 0 : g_num_frozen;
}
