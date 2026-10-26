// Aspetto dei disegni ufficiali (OfficialArtwork), come in A Link to the Past
// Redux: Agahnim con veste rossa e pelle blu, re di Hyrule vestito di rosso e
// il medaglione del pavimento (tavola di sfondo 0x19, palazzi dell'Est, del
// Deserto, di Hera e Palazzo Oscuro) con la stella a sei punte della versione
// giapponese. Link biondo e' un'opzione a parte (BlondLink). Lo zio e il cappuccio di Agahnim ridisegnati da
// Redux non ci sono: Agahnim cambia solo i colori.
// Redux cambia la palette di Agahnim per tutti quelli che la usano e per questo
// ridipinge il ladro dei Boschi Perduti in tutto il gioco; qui la palette nuova
// vale solo dove compare Agahnim (sp5l = 14: le sue stanze, la battaglia e il
// prologo tranne la sala del trono, dove c'e' solo il re che Redux ha ritoccato
// a mano per la palette nuova) e il ladro resta com'e'.
//
// hd_art_data.h contiene solo le differenze (XOR) dalle caselle originali,
// come una patch: senza la ROM non c'e' grafica. Ogni casella ha una somma di
// controllo dell'originale e si cambia solo se corrisponde.
#include <string.h>
#include "hd_art.h"
#include "config.h"
#include "assets.h"
#include "variables.h"
#include "zelda_rtl.h"
#include "load_gfx.h"
#include "hd_art_data.h"

static bool g_art_link_on;
static uint16 g_armor_orig[countof(kArtArmorColors)];

static uint16 ArtSum(const uint8 *p, int n) {
  uint16 s = 0;
  for (int i = 0; i < n; i++)
    s += p[i] * (i + 1);
  return s;
}

static void XorTile(uint8 *p, const uint8 *x, int n) {
  for (int i = 0; i < n; i++)
    p[i] ^= x[i];
}

void HdArt_Apply(void) {
  bool on = g_config.blond_link && !g_config.link_graphics;
  if (on == g_art_link_on)
    return;
  // la somma si controlla sull'originale: prima di applicare, o dopo aver tolto
  for (int i = 0; i < countof(kArtLinkTiles); i++) {
    const ArtLinkTile *t = &kArtLinkTiles[i];
    uint8 *p = kLinkGraphics + t->tile * 32;
    if (on && ArtSum(p, 32) != t->sum)
      continue;
    XorTile(p, t->xor, 32);
    if (!on && ArtSum(p, 32) != t->sum)
      XorTile(p, t->xor, 32);  // non era cambiata
  }
  uint16 *armor = kPalette_ArmorAndGloves;
  for (int i = 0; i < countof(kArtArmorColors); i++) {
    uint16 *c = &armor[kArtArmorColors[i][0]];
    if (on)
      g_armor_orig[i] = *c, *c = kArtArmorColors[i][1];
    else
      *c = g_armor_orig[i];
  }
  g_art_link_on = on;
  // palette di Link subito, se si sta giocando (il coniglio non cambia)
  if ((main_module_index == 7 || main_module_index == 9 || main_module_index == 14) && !link_is_bunny)
    LoadActualGearPalettes();
}

static void PatchSheet(uint8 *dst, int gfx, const ArtTile *t, int n) {
  for (int i = 0; i < n; i++) {
    uint8 *p = dst + t[i].tile * 24;
    if (t[i].sheet == gfx && ArtSum(p, 24) == t[i].sum)
      XorTile(p, t[i].xor, 24);
  }
}

void HdArt_PatchBgSheet(uint8 *dst, int gfx) {
  if (g_config.official_artwork)
    PatchSheet(dst, gfx, kArtBgTiles, countof(kArtBgTiles));
}

const uint16 *HdArt_AgahnimPalette(const uint16 *src) {
  return g_config.official_artwork ? kArtAgahnimPal : src;
}

uint8 HdArt_KingOamFlags(uint8 flags) {
  return g_config.official_artwork ? kArtKingOamFlags : flags;
}
