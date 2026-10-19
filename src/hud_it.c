// Menu Start in italiano. I nomi italiani sono piu' lunghi di quelli inglesi
// e con le lettere del gioco (8 pixel l'una, 8 per riga) non ci stanno: si
// scrivono con un carattere stretto, alto come quello del gioco, e le
// intestazioni dei riquadri con un carattere piccolo come "ITEM". Le parole si
// disegnano in caselle del BG3 che il gioco usa solo per il testo dei dialoghi
// (0x180 in poi): mentre il menu e' aperto non serve, e il prossimo dialogo
// lo riscrive.
#include <string.h>
#include "hud_it.h"
#include "hud.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "config.h"
#include "util.h"

enum {
  kTileBase = 0x180,       // prima casella usata (testo dei dialoghi)
  kTileItemName = kTileBase,         // 16 caselle: nome dell'oggetto
  kTileLabels = kTileBase + 16,      // intestazioni, una dopo l'altra (31 caselle)
  kTileAbilities = kTileBase + 48,   // 6 abilita' da 5 caselle
  kColorFg = 2, kColorBg = 3,        // come le lettere del gioco
  kTileAttr = 0x2400,                // palette 1 e priorita', come le scritte del menu
};

bool HudIt_Active(void) {
  return g_config.language && StringEqualsNoCase(g_config.language, "it");
}

// Caratteri: ogni lettera e' una riga di testo per riga di pixel, '#' acceso.
// Stretto (nomi): alto 6, largo 4 (I e T 3, M, V, W e Y 5). Piccolo
// (intestazioni): alto 5, largo 3 (I 1, N 4, M e W 5), come "ITEM" del gioco.
typedef struct Glyph {
  char c;
  const char *rows[6];
} Glyph;

static const Glyph kNarrow[] = {
  { 'A', { ".##.", "#..#", "#..#", "####", "#..#", "#..#" } },
  { 'B', { "###.", "#..#", "###.", "#..#", "#..#", "###." } },
  { 'C', { ".##.", "#..#", "#...", "#...", "#..#", ".##." } },
  { 'D', { "###.", "#..#", "#..#", "#..#", "#..#", "###." } },
  { 'E', { "####", "#...", "###.", "#...", "#...", "####" } },
  { 'F', { "####", "#...", "###.", "#...", "#...", "#..." } },
  { 'G', { ".##.", "#...", "#.##", "#..#", "#..#", ".###" } },
  { 'H', { "#..#", "#..#", "####", "#..#", "#..#", "#..#" } },
  { 'I', { "###", ".#.", ".#.", ".#.", ".#.", "###" } },
  { 'J', { "...#", "...#", "...#", "...#", "#..#", ".##." } },
  { 'K', { "#..#", "#.#.", "##..", "#.#.", "#..#", "#..#" } },
  { 'L', { "#...", "#...", "#...", "#...", "#...", "####" } },
  { 'M', { "#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#" } },
  { 'N', { "#..#", "##.#", "#.##", "#..#", "#..#", "#..#" } },
  { 'O', { ".##.", "#..#", "#..#", "#..#", "#..#", ".##." } },
  { 'P', { "###.", "#..#", "#..#", "###.", "#...", "#..." } },
  { 'Q', { ".##.", "#..#", "#..#", "#..#", "#.#.", ".#.#" } },
  { 'R', { "###.", "#..#", "#..#", "###.", "#.#.", "#..#" } },
  { 'S', { ".###", "#...", ".##.", "...#", "...#", "###." } },
  { 'T', { "###", ".#.", ".#.", ".#.", ".#.", ".#." } },
  { 'U', { "#..#", "#..#", "#..#", "#..#", "#..#", ".##." } },
  { 'V', { "#...#", "#...#", "#...#", ".#.#.", ".#.#.", "..#.." } },
  { 'W', { "#...#", "#...#", "#...#", "#.#.#", "##.##", "#...#" } },
  { 'X', { "#..#", "#..#", ".##.", ".##.", "#..#", "#..#" } },
  { 'Y', { "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#.." } },
  { 'Z', { "####", "...#", "..#.", ".#..", "#...", "####" } },
  { '\'', { "#", "#", ".", ".", ".", "." } },
  { '.', { ".", ".", ".", ".", ".", "#" } },
  { '1', { ".#.", "##.", ".#.", ".#.", ".#.", "###" } },
  { '2', { ".##.", "#..#", "..#.", ".#..", "#...", "####" } },
  { '3', { "###.", "...#", ".##.", "...#", "...#", "###." } },
  { ' ', { "..", "..", "..", "..", "..", ".." } },
};

static const Glyph kSmall[] = {
  { 'A', { ".#.", "#.#", "###", "#.#", "#.#" } },
  { 'B', { "##.", "#.#", "##.", "#.#", "##." } },
  { 'C', { ".##", "#..", "#..", "#..", ".##" } },
  { 'D', { "##.", "#.#", "#.#", "#.#", "##." } },
  { 'E', { "###", "#..", "##.", "#..", "###" } },
  { 'F', { "###", "#..", "##.", "#..", "#.." } },
  { 'G', { ".##", "#..", "#.#", "#.#", ".##" } },
  { 'H', { "#.#", "#.#", "###", "#.#", "#.#" } },
  { 'I', { "#", "#", "#", "#", "#" } },
  { 'J', { "..#", "..#", "..#", "#.#", ".#." } },
  { 'K', { "#.#", "#.#", "##.", "#.#", "#.#" } },
  { 'L', { "#..", "#..", "#..", "#..", "###" } },
  { 'M', { "#...#", "##.##", "#.#.#", "#...#", "#...#" } },
  { 'N', { "#..#", "##.#", "#.##", "#..#", "#..#" } },
  { 'O', { ".#.", "#.#", "#.#", "#.#", ".#." } },
  { 'P', { "##.", "#.#", "##.", "#..", "#.." } },
  { 'Q', { ".#.", "#.#", "#.#", "##.", ".##" } },
  { 'R', { "##.", "#.#", "##.", "#.#", "#.#" } },
  { 'S', { ".##", "#..", ".#.", "..#", "##." } },
  { 'T', { "###", ".#.", ".#.", ".#.", ".#." } },
  { 'U', { "#.#", "#.#", "#.#", "#.#", "###" } },
  { 'V', { "#.#", "#.#", "#.#", "#.#", ".#." } },
  { 'W', { "#...#", "#...#", "#.#.#", "##.##", "#...#" } },
  { 'X', { "#.#", "#.#", ".#.", "#.#", "#.#" } },
  { 'Y', { "#.#", "#.#", ".#.", ".#.", ".#." } },
  { 'Z', { "###", "..#", ".#.", "#..", "###" } },
  { '.', { ".", ".", ".", ".", "#" } },
  { ' ', { "..", "..", "..", "..", ".." } },
};

static const Glyph *FindGlyph(const Glyph *font, int n, char c) {
  for (int i = 0; i < n; i++)
    if (font[i].c == c)
      return &font[i];
  return NULL;
}

// Larghezza in pixel del testo (1 pixel fra una lettera e l'altra)
static int TextWidth(const Glyph *font, int n, const char *s) {
  int w = 0;
  for (; *s; s++) {
    const Glyph *g = FindGlyph(font, n, *s);
    if (g)
      w += (int)strlen(g->rows[0]) + (s[1] ? 1 : 0);
  }
  return w;
}

// Disegna il testo nelle caselle a partire da tile (larghe ntiles, una riga di
// caselle), x pixel dal bordo sinistro e y dall'alto; le caselle vanno
// cancellate prima con ClearTiles
static void DrawText(int tile, int ntiles, const Glyph *font, int n, int height,
                     const char *s, int x, int y) {
  uint16 *vram = g_zenv.vram;
  for (; *s; s++) {
    const Glyph *g = FindGlyph(font, n, *s);
    if (!g)
      continue;
    int w = (int)strlen(g->rows[0]);
    for (int r = 0; r < height; r++) {
      for (int c = 0; c < w; c++) {
        int px = x + c, py = y + r;
        if (g->rows[r][c] != '#' || px < 0 || px >= ntiles * 8 || py < 0 || py >= 8)
          continue;
        uint16 *word = &vram[(0x7000 + (tile + px / 8) * 8 + py) & 0x7fff];
        int bit = 7 - (px & 7);
        *word = (*word & ~(0x101 << bit)) | (kColorFg & 1) << bit | (kColorFg >> 1 & 1) << (bit + 8);
      }
    }
    x += w + 1;
  }
}

static void ClearTiles(int tile, int ntiles) {
  uint16 fill = (kColorBg & 1 ? 0xff : 0) | (kColorBg & 2 ? 0xff00 : 0);
  for (int i = 0; i < ntiles * 8; i++)
    g_zenv.vram[(0x7000 + tile * 8 + i) & 0x7fff] = fill;
}

// Intestazioni: testo, caselle occupate
static const struct { const char *text; uint8 tiles; } kLabels[] = {
  { "STRUMENTO", 5 },
  { "AZIONE", 3 },
  { "EQUIPAGGIAMENTO", 8 },
  { "LABIRINTO", 5 },
  { "AMULETI", 5 },
  { "CRISTALLI", 5 },
};

int HudIt_LabelTiles(int label) {
  return kLabels[label].tiles;
}

void HudIt_DrawLabel(uint16 *dst, int label) {
  int tile = kTileLabels;
  for (int i = 0; i < label; i++)
    tile += kLabels[i].tiles;
  int n = kLabels[label].tiles;
  ClearTiles(tile, n);
  // il testo comincia dal primo pixel e sta fra la riga 1 e la 5, come "ITEM"
  DrawText(tile, n, kSmall, (int)countof(kSmall), 5, kLabels[label].text, 0, 1);
  for (int i = 0; i < n; i++)
    dst[i] = kTileAttr | (tile + i);
}

// Nome italiano dell'oggetto, con '\n' dove va a capo (righe al massimo di
// 64 pixel); stessa scelta di Hud_DrawSelectedYButtonItem
static const char *ItemName(int item, int btn_index) {
  static const char *const kBottles[8] = {
    "FUNGO", "AMPOLLA", "POZIONE\nVITA", "POZIONE\nMAGICA", "POZIONE\nCURATUTTO",
    "FATA", "APE", "APE\nDORATA",
  };
  static const char *const kItems[20] = {
    "ARCO", "BOOMERANG", "ARPIONE", "BOMBA", "FUNGO",
    "BACCHETTA DEL\nFUOCO", "BACCHETTA DEL\nGHIACCIO", "FIAMMA", "ETERE", "SISMA",
    "LANTERNA", "MARTELLO\nMAGICO", "PALA", "RETINO PER\nFARFALLE", "LIBRO DI\nMUDORA",
    "FUNGO", "BASTONE DI\nSOMARIA", "BASTONE DI\nBYRNA", "MANTELLO\nMAGICO", "",
  };
  // inventario nuovo (kNewStyleInventory): quattro bottiglie, la 16 e' la pala
  if (item >= kHudItem_Bottle1 && item <= kHudItem_Bottle4) {
    int bottle = link_bottle_info[item - kHudItem_Bottle1];
    return bottle >= 1 && bottle <= 8 ? kBottles[bottle - 1] : "";
  }
  switch (item) {
  case 0: return btn_index ? "NON\nASSEGNATO" : "";
  case 1: return link_item_bow == 2 ? "ARCO E FRECCE" : link_item_bow == 4 ? "ARCO E FRECCE\nD'ARGENTO" : "ARCO";
  case 2: return link_item_boomerang == 2 ? "BOOMERANG\nMAGICO" : "BOOMERANG";
  case 5: return link_item_mushroom != 1 ? "POLVERE\nMAGICA" : "FUNGO";
  case kHudItem_Flute: return link_item_flute != 1 ? "OCARINA" : "PALA";
  case kHudItem_Shovel: return "PALA";
  case 20: return link_item_mirror != 1 ? "SPECCHIO\nMAGICO" : "";
  }
  return item >= 1 && item <= 20 ? kItems[item - 1] : "";
}

void HudIt_DrawItemName(uint16 *dst, int item, int btn_index) {
  const char *name = ItemName(item, btn_index);
  const char *nl = strchr(name, '\n');
  char first[24];
  const char *second = name;
  first[0] = 0;
  if (nl) {
    int n = (int)(nl - name) < (int)sizeof(first) - 1 ? (int)(nl - name) : (int)sizeof(first) - 1;
    memcpy(first, name, n);
    first[n] = 0;
    second = nl + 1;
  }
  // Due righe di 8 caselle: come nel gioco, un nome corto sta sulla seconda
  // riga a sinistra; con due righe la prima e' a sinistra e la seconda a destra
  int n = (int)countof(kNarrow);
  ClearTiles(kTileItemName, 16);
  DrawText(kTileItemName, 8, kNarrow, n, 6, first, 0, 1);
  int x2 = nl ? 64 - TextWidth(kNarrow, n, second) : 0;
  DrawText(kTileItemName + 8, 8, kNarrow, n, 6, second, x2 < 0 ? 0 : x2, 1);
  for (int i = 0; i < 8; i++) {
    dst[i] = kTileAttr | (kTileItemName + i);
    dst[32 + i] = kTileAttr | (kTileItemName + 8 + i);
  }
}

void HudIt_DrawAbility(uint16 *dst, int ability, int level) {
  static const char *const kAbilities[6] = { "ALZA", "LEGGI", "PARLA", "TIRA", "CORRI", "NUOTA" };
  char buf[8];
  const char *s = kAbilities[ability];
  if (ability == 0) {
    // ALZA.1, ALZA.2, ALZA.3 come LIFT.1-3: il livello dei guanti
    memcpy(buf, "ALZA.1", 7);
    buf[5] = '0' + (level < 1 ? 1 : level > 3 ? 3 : level);
    s = buf;
  }
  // nel mezzo delle 5 caselle: le parole di 5 lettere restano staccate
  int tile = kTileAbilities + ability * 5, n = (int)countof(kNarrow);
  int x = (40 - TextWidth(kNarrow, n, s)) / 2;
  ClearTiles(tile, 5);
  DrawText(tile, 5, kNarrow, n, 6, s, x < 0 ? 0 : x, 1);
  for (int i = 0; i < 5; i++)
    dst[i] = 0x2c00 | (tile + i);  // palette 3, come le abilita' del gioco
}
